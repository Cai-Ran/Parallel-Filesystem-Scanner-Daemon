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
│  poll metrics until all zero:                       │
│    scan_running         == 0  ┐                     │
│    scan_pending         == 0  │  wait...            │
│    scan_jobs_unfinished == 0  │                     │
│    export_pending       == 0  │                     │
│    export_running       == 0  ┘                     │
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
  set_export_dir()    ──────────► manager.set_export_dir()
  check_exported()    ──────────► manager.check_exported()
  export_result()     ──────────► manager.export_report()
  get_newest_index()  ──────────► manager.get_newest_index()
  index_report()      ──────────► manager.index_report()
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
                     ──────                                                            set_export_dir()
                     TCP conn ──► server_socket                                        submit_scan()
                                   0.0.0.0:port                                        cancel_scan()
                                        │                                              get_state()
                                        │ accept()                                     check_exported()
                                        ▼                                              export_result()
                               ┌───────────────┐                                       index_report()
                               │  HttpServer   │                                       get_newest_index()
                               │               │                                       shutdown()
                               │  ┌──────────┐ │   handle_request()                    ─────────────────
"Server busy" 429 ← full     ← │  │ fd_pool  │─┼──────────────────────────────────►    Daemon API 
                               │  │  FIFO    │ │   read_request()
"Unavailable" 503 ← shutdown ← │  │ N threads│ │   router()
                               │  └──────────┘ │
                               │               │   Guards:
                               │  drain_flag   │   export_dir_set → restrict routes
                               │  stop_flag    │   drain_flag     → restrict routes
                               └───────────────┘
                                       │
                                       ▼
                                client fd closed
```

### Router Guards

```
  export_dir not set  →  only allow:
      GET  /                 POST /export_dir
      GET  /metrics          POST /shutdown
      GET  /download_time

  drain_flag set      →  only allow:
      GET  /                 GET  /metrics
      GET  /state            GET  /exporting
      GET  /export_summary   GET  /export_detail
      GET  /index_summary    GET  /index_detail
```

---

### HTTP Endpoints

| Method | Path | Params | Action |
|---|-----|-------|---|
| `GET`  | `/` | — | Serve frontend HTML |
| `POST` | `/export_dir` | `dir=<path>` | Set export directory (required before scans) |
| `POST` | `/scan` | `root=<path>` | Submit a new scan; returns `{id}` |
| `POST` | `/cancel` | `id=<id>` | Cancel a single pending or running scan |
| `GET`  | `/state` | `id=1,2,3` | **Batch poll** — query states for comma-separated IDs; returns `[{id, state}, ...]` |
| `GET`  | `/exporting` | `id=1,2,3` | **Batch poll** — query export states; returns `[{id, export_state}, ...]` |
| `GET`  | `/export_summary` | `id=<id>` | Serve export summary HTML for one scan |
| `GET`  | `/export_detail` | `id=<id>` | Serve export detail JSON for one scan |
| `POST` | `/index` | — | Trigger index snapshot; returns `{state, version, timestamp}` |
| `GET`  | `/index_summary` | `id=<version>` | Serve index summary HTML for one version |
| `GET`  | `/index_detail` | `id=<version>` | Serve index detail JSON for one version |
| `GET`  | `/metrics` | — | Return runtime metrics JSON |
| `GET`  | `/download_time` | — | Return `{user_download_sec}`: user download time after graceful shutdown |
| `POST` | `/shutdown` | — | Initiate graceful shutdown |

