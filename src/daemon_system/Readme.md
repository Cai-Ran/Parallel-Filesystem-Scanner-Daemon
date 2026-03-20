# Daemon System Architecture

---

## Overview

```
  OS / Main
  ─────────
  SIGINT / SIGTERM ────────┐
                           | shutdown_flag = true
                           |
                           ▼
                    ┌─────────────────────────────────────────┐
                    │                 Daemon                  │
                    │           daemon.h / daemon.cpp         │
                    │                                         │
                    │   ┌─────────┐ ┌───────────┐ ┌────────┐  │
                    │   │ Manager │ │ Scheduler │ │  Http  │  │
                    │   │         │ │           │ │ Server │  │
                    │   └────┬────┘ └─────┬─────┘ └────────┘  │
                    │        │            │                   │
                    │        └────────────┘                   │
                    │         callbacks                       │
                    └─────────────────────────────────────────┘
```


## Graceful Shutdown (Drain -> Shutdown)

```
SIGINT / SIGTERM received | POST /shutdown
         │
         ▼
┌──────────────────────────────────────────────────┐
│  Phase 1: DRAIN                                  │
│                                                  │
│  httpserver.drain()                              │
│                                                  │
│  POST /scan      → 403  ✗  blocked               │
│  POST /cancel    → 403  ✗  blocked               │
│                                                  │
│  GET  /state     → 200  ✓  still alive           │
│  GET  /exporting → 200  ✓  still alive           │
│  GET  /metrics   → 200  ✓  still alive           │
│  GET  /export_*  → 200  ✓  still alive           │
│  GET  /index_*   → 200  ✓  still alive           │
└──────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│  Phase 2: CANCEL & WAIT                             │
│                                                     │
│  scheduler.shutdown()                               │
│    → cancel all running / pending scans             │
│                                                     │
│  poll metrics until all zero:                           │
│    scan_running              == 0  ┐                │
│    scan_pending              == 0  │  wait...       │
│    scan_jobs_unfinished      == 0  │                │
│    export_result_pending     == 0  │                │
│    export_result_running     == 0  │                │
│    export_delete_pending     == 0  │                │
│    export_delete_running     == 0  ┘                │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│  Phase 3: GRACE PERIOD                              │
│                                                     │
│  sleep USER_DOWNLOAD_SEC  (configurable)            │
│                                                     │
│  client can still fetch results:                    │
│    GET /export_summary, /export_detail              │
│    GET /index_summary,  /index_detail               │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│  Phase 4: SHUTDOWN                                  │
│                                                     │
│  httpserver.shutdown()  → close server socket       │
│  join threads                                       │
└─────────────────────────────────────────────────────┘

```

---

## Daemon API

```
  HttpServer calls                Daemon delegates to

  submit_scan()       ──────────► scheduler.submit_scan_root()
  get_state()         ──────────► scheduler.get_state()
  cancel_scan()       ──────────► scheduler.cancel()
  shutdown()          ──────────► shutdown_flag = true
  check_exported()    ──────────► manager.check_exported()
```

---

## Scheduler

```
                         scheduler.h / scheduler.cpp

  HttpServer threads                        Manager callbacks
  ──────────────────                        ─────────────────
  submit_scan_root() ──┐               ┌── notify_scan_finished(id)
  cancel()          ───┤               ├── notify_dispatch_available()
                       │               │
                       ▼               │
            ┌──────────────────────────────────────────┐
            │               Scheduler                  │
            │                                          │
            │   ┌────────────────────────────────┐     │
            │   │  state_map                     │     │
            │   │  scan_id → RequestState        │     │
            │   └────────────────────────────────┘     │
            │                                          │
            │   ┌────────────────────────────────┐     │
            │   │  pending_queue                 │     │
            │   └──────────────┬─────────────────┘     │
            │                  │                       │
            │   ┌──────────────▼─────────────────┐     │
            │   │  run()  dispatch loop          │     │
            │   │  cv.wait() → pop()   → Manager │     │
            │   └────────────────────────────────┘     │
            │                                          │
            │   Config: MAX_CONCURRENT_SCAN            │
            │           QUEUE_MAX_SIZE                 │
            │   Lock:   mtx + cv                       │
            └──────────────────────────────────────────┘
                       │
                       ▼
                    Manager
                ────────────────── 
                  start_new_scan()
                  cancel_scan()
                  wait_to_finish()
                  shutdown()
```

---

## RequestState Lifecycle

```
              submit_scan_root()
                     │
                     ▼
                  PENDING ──────────────────────────────► DROPPED
                     │          cancel() while PENDING
                     │
              run() dequeues
                     │
                     ▼          cancel() while DISPATCHING 
                DISPATCHING ─────────────────────────────► CANCELED
                     │                                       │
              manager.start_new_scan()                       │
                     │                                       │
         ┌───────────┼───────────┐                           │
      accepted    queue_full   error                         │
         │           │           │                           │
         ▼           ▼           ▼                           │
      RUNNING      PENDING     FAILED                        │
         │         (retry)                                   │
         │                                                   │
  notify_scan_finished()   cancel() while RUNNING ───────────┘
         │
         ▼
        DONE

  Note: DISPATCHING is internal only; exposed as PENDING via get_state()
```

---

## HttpServer

```
                        httpserver.h / httpserver.cpp

                    Client                                                   
                     ──────                                                            submit_scan()
                     TCP conn ──► server_socket                                        cancel_scan()
                                   0.0.0.0:port                                        get_state()
                                        │                                              check_exported()
                                        │ accept()                                     shutdown()
                                        ▼                                              ─────────────────
                               ┌───────────────┐                                       Daemon API
                               │  HttpServer   │                                       
                               │               │
                               │  ┌──────────┐ │   handle_request()
"Server busy" 429 ← full     ← │  │ fd_pool  │─┼──────────────────────────────────►
                               │  │  FIFO    │ │   read_request()
"Unavailable" 503 ← shutdown ← │  │ N threads│ │   router()
                               │  └──────────┘ │
                               │               │   Guards:
                               │  drain_flag   │   drain_flag → restrict routes
                               │  stop_flag    │
                               └───────────────┘
                                       │
                                       ▼
                                client fd closed
```

### Router Guards

```
  drain_flag set  →  only allow:
      GET  /                     GET  /metrics
      GET  /state                GET  /exporting
      GET  /scan_diff_summary    GET  /scan_diff_detail
      GET  /index_summary        GET  /index_detail
```

---

### HTTP Endpoints

| Method | Path | Params | Action |
|---|-----|-------|---|
| `GET`  | `/` | — | Serve frontend HTML |
| `POST` | `/scan` | `root=<path>` | Submit a new scan; returns `{id}` |
| `POST` | `/cancel` | `id=<id>` | Cancel a single pending or running scan |
| `GET`  | `/state` | `id=1,2,3` | **Batch poll** — query states for comma-separated IDs; returns `[{id, state}, ...]` |
| `GET`  | `/exporting` | `id=1,2,3` | **Batch poll** — query export states; returns `[{id, export_state}, ...]` |
| `GET`  | `/scans_history` | `page=x&limit=x` | Paginated list of all scan tasks from DB |
| `GET`  | `/scan_diff_summary` | `id=<id>` | Diff summary for one scan (added/modified/deleted counts) |
| `GET`  | `/scan_diff_detail` | `id=x&state=ALL&page=x&limit=x` | Paginated diff entries for one scan |
| `GET`  | `/index_summary` | `type=folder\|extension` | Aggregated index stats grouped by folder or extension |
| `GET`  | `/index_detail` | `search=x&page=x&limit=x` | Search file index by keyword, paginated |
| `GET`  | `/metrics` | — | Return runtime metrics JSON |
| `GET`  | `/download_time` | — | Return `{user_download_sec}`: user download time after graceful shutdown |
| `POST` | `/shutdown` | — | Initiate graceful shutdown |

