# File System Scanning Daemon — Architecture
[![CI](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/ci.yml)
[![Sanitizers](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/sanitizers.yml/badge.svg?branch=master)](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/sanitizers.yml)
[![Clang-Tidy](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/clang-tidy.yml/badge.svg?branch=master)](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/clang-tidy.yml)
[![Coverage](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/coverage.yml/badge.svg?branch=master)](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/coverage.yml)
[![Benchmark](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/benchmark.yml/badge.svg?branch=master)](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/benchmark.yml)
[![CodeQL](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/codeql.yml/badge.svg?branch=master)](https://github.com/Cai-Ran/Parallel-Filesystem-Scanner-Daemon/actions/workflows/codeql.yml)


> Scope note: current Sanitizers/Coverage focus on unit-tested concurrency modules (`job_queue`, `thread_pool`) and selected `utils`, not full daemon/API end-to-end paths.


## Table of Contents

- [Overview](#overview)
- [Quick Start](#quick-start)
- [System Architecture](#System-Architectur)
- [Scan Lifecycle Diagram](#Scan-Lifecycle-Diagram)
- [State Machine](#state-machine--polling-rules)
- [Directory Structure](#directory-structure)
- [Configuration](#configuration)

---

## Overview

A concurrent file system scanning daemon with:

- Multi-threaded DFS directory traversal (LIFO job queue → small working set)
- Rate-limited scan dispatch 
- Incremental SQLite index for persistent change detection across scans
- Async DB export via two dedicated write threads 
- HTTP REST API for client interaction
- Graceful shutdown with configurable download grace period
- Threads all coordinated with bounded queues and condition variables

### Demo

![Demo](assets/demo.gif)

If embedded playback is not supported on your Markdown viewer, [open the demo video](./assets/demo.mp4).

---

## Quick Start

### 1. Run on Linux

1. Build the project:

```bash
make
```

2. Start the daemon:

```bash
./service
```

3. Open your local browser:

```text
http://localhost:8080
```

### 2. Benchmark

1. Run the full benchmark suite:

```bash
make bench
```

2. Output is in benchmark/runs/ folder


---




## System Architecture

```text
+----------------------+          HTTP          +-------------------------------------+
| Browser / API Client | ---------------------> | HttpServer                          |
|                      |                        | - ThreadPool<int> (FIFO)            |
+----------------------+                        +------------------+------------------+
                                                                  |
                                                                  | calls Daemon API
                                                                  v
                                                     +------------+------------+
                                                     | Daemon                  |  <------------  SIGINT / SIGTERM
                                                     | - owns Scheduler        |
                                                     | - owns Manager          |
                                                     +------------+------------+
                                                                  |
                         +----------------------------------------+----------------------------------------+
                         |                                                                                 |
                         v                                                                                 v
           +-------------+--------------+                                                  +--------------+-------------+
           | Scheduler                  | <-------------- notify finished ---------------- | Manager                    |
           | - pending queue            |                                                  | - scan registry            |
           | - state map                |                                       ---------> | - callbacks to Scheduler   |
           +-------------+--------------+                                       |          +--------------+-------------+
                         |                                                      |                         |
                         | dispatch root                                        |                         | submit ScanData
                         v                                                      |                         v
           +-------------+--------------+                                       |          +--------------+-------------+
           | start_new_scan(scan_id)    | ---------------------------------------          | ThreadPool<ScanData>       |
           +----------------------------+                                                  | (LIFO)                     |
                                                                                           +--------------+-------------+
                                                                                                          |
                                                                                                          v
                                                                                           +--------------+-------------+
                                                                                           | MultiScanner               |
                                                                                           | - DFS-like traversal       |
                                                                                           | - builds ScanResult        |
                                                                                           +--------------+-------------+
                                                                                                          |
                                                                                                          | completed result
                                                                                                          v
                                                                                           +--------------+-------------+
                                                                                           | ExportManager              |
                                                                                           | - result_thread (DB write) |
                                                                                           | - delete_thread (DB delete)|
                                                                                           +--------------+-------------+
                                                                                                          |
                                                                                                          v
                                                                                           +--------------+-------------+
                                                                                           | SQLite Database            |
                                                                                           | - index_table              |
                                                                                           | - scan_task_table          |
                                                                                           | - scan_diff_table          |
                                                                                           +----------------------------+
```

---

## State Machine & Polling Rules

```
 Scheduler                                  ExportManager
 poll: GET /state                           poll: GET /exporting
 ─────────────────────────────────────      ─────────────────────────────────
                              
 PENDING ──► RUNNING ────┐    polling           EXPORTING  
    │            │       │                         │
  cancel      cancel     |                         │
    │            │       │                         │
    ▼            ▼       ▼                         ▼
DROPPED✕   CANCELED✕   DONE✕                  EXPORTED✕   
                                    
                                                   
                                                

 ✕ = death state, stop polling
```

| State | Phase | Poll | Notes |
|---|---|---|---|
| `PENDING` `RUNNING` | Scheduler | `GET /state` | scan in progress |
| `DONE` `CANCELED` | Scheduler | ✕ stop | scan finished, export starting |
| `DROPPED` `FAILED` | Scheduler | ✕ stop  | — |
| `UNAVAILABLE` | ExportManager | ✕ no polling | `DROPPED` `FAILED` scans |
| `EXPORTING` | ExportManager | `GET /exporting` | writing results to SQLite DB |
| `EXPORTED` | ExportManager | ✕ stop  | user can fetch results via `/scan_diff_summary` `/scan_diff_detail` |

---




## Scan Lifecycle Diagram

```text
            [Thread Pool]                     [Task Queue]                       [Thread Pool]       [Task Queue]
Client        HttpServer        Daemon          Scheduler         Manager         Scan Worker       ExportManager
  |               |               |                |                |                 |                  |
1 | POST /scan    |               |                |                |                 |                  |
  |-------------->|               |                |                |                 |                  |
2 |               | submit_scan() |                |                |                 |                  |
  |               |-------------->|                |                |                 |                  |
  |               |               | submit_scan_root(root): push_queue                |                  |
  |               |               |--------------->|                |                 |                  |
3a|<--------------| return HTTP 200                                 |                 |                  |
3b|<--------------| return HTTP 429               pending queue full|                 |                  |
  |               |               |                |                |                 |                  |
4 |               |               |                |start_new_scan(scan_id, root)     |                  |               
  |               |               |                |--------------->|                 |                  |
  |               |               |                |                | submit root job |                  |
5a|               |               |                |                |---------------->| Enqueue          |
  |               |               |                |                |                 |                  |
5b|               |               |reject run scan |<-------------- |<----------------| QueueFull        |

  |=============================== DONE flow =========================================|                  |
6 |               |               |                |                |<----------------| task_on_job_finish
7 |               |           notify_scan_finished |<---------------|                 |                  |
8 |               |               |                |                |------------------------------->    |
9 |               |               |                |                | push_queue(result)                 |

  |========================== Cancel A: PENDING -> DROPPED ===========================|                  |
c1| POST /cancel?id=...           |                |                |                 |                  |
  |-------------->|               |                |                |                 |                  |
c2|               | cancel_scan(id)                |                |                 |                  |
  |               |-------------->|                |                |                 |                  |
c3|               |               | cancel(id)     |                |                 |                  |
  |               |               |--------------->| state: PENDING -> DROPPED        |                  |
c4|<--------------| return HTTP 200                |                |                 |                  |

  |========================== Cancel B: RUNNING/DISPATCHING -> CANCELED ==============|                  |
r1| POST /cancel?id=...           |                |                |                 |                  |
  |-------------->|               |                |                |                 |                  |
r2|               | cancel_scan(id)                |                |                 |                  |
  |               |-------------->|                |                |                 |                  |
r3|               |               | cancel(id)     |                |                 |                  |
  |               |               |--------------->| state: RUNNING/DISPATCHING -> CANCELED              |
r4|<--------------|  return HTTP 200               |                |                 |                  |
  |               |               |                |--------------->|cancel_scan(id)  |                  |
  |               |               |                |                |                 |                  |
  |               |               |                |                |---------------->| 
r6|               |               |                |                |<----------------| task_on_job_finish
r7|               |           notify_scan_finished |<---------------|                 |                  |
r8|               |               |                |                |----------------------------------->|
r9|               |               |                |                | push_queue({..., canceled=true})   |


```
------




## Directory Structure

```
Parallel-Filesystem-Scanner-Daemon/
├── src/
│   ├── main.cpp                  # Entry point
│   ├── common/                   # Shared data structures (FileEvent, ScanTaskRow, db_types)
│   ├── concurrency/              # ThreadPool, JobQueue templates
│   ├── daemon_system/            # Daemon, HttpServer, Scheduler
│   ├── scan_system/              # Manager, MultiScanner
│   ├── export_system/            # ExportManager
│   │   └── db_wrapper/           # SQLite layer (IndexWriter, IndexReader, ScanTable, ScanDiff)
│   └── utils/                    # Config, AsyncLogger, Metrics, Formater
├── web/                          # Frontend HTML/JS
├── tests/                        # Unit and integration tests
├── benchmark/                    # Performance benchmarking
├── db/                           # SQLite database files (created at build time)
├── log/                          # Runtime log files
├── config.ini                    # Runtime configuration
└── makefile                      # Build system
```

---

### Module Ownership Tree

```
main.cpp
└── Daemon
    ├── HttpServer       
    |   └── fd_pool:   ThreadPool<int>
    ├── Scheduler         
    └── Manager
        ├── MultiScanner
        ├── scan_pool: ThreadPool<ScanData>  
        └── ExportManager
              ├─ result_thread  (JobQueue<FileEvent>  → SQLite write)
              └─ delete_thread  (JobQueue<DeleteTask> → SQLite mark deleted)
```


## Configuration

`config.ini` sections and key parameters:

| Section | Key | Default | Description |
|---|---|---|---|
| `[daemon]` | `user_download_sec` | `60` | Grace period after shutdown for client downloads |
| `[httpserver]` | `server_port` | `8080` | Listening port |
| `[httpserver]` | `fd_pool_num_threads` | `16` | HTTP worker thread count |
| `[httpserver]` | `fd_queue_max_size` | `8` | HTTP request queue capacity |
| `[scheduler]` | `max_concurrent_scan` | `3` | Max simultaneous scans |
| `[scheduler]` | `queue_max_size` | `4` | Pending scan queue capacity |
| `[manager]` | `scan_pool_num_threads` | `4` | Scan worker thread count |
| `[manager]` | `scan_queue_max_size` | `1000` | Scan job queue capacity |
| `[asynclogger]` | `log_dir` | *(path)* | Directory for log files |
| `[asynclogger]` | `queue_max_size` | `8192` | Log queue capacity |
| `[export_manager]` | `result_que_size` | `524288` | Result write queue capacity |
| `[export_manager]` | `delete_que_size` | `4` | Delete queue capacity |
| `[db]` | `db_path` | `./db/scan_database.db` | SQLite database file path |
| `[db]` | `batch_size` | `32768` | Max rows per SQLite write batch |
| `[db]` | `fsync` | `FALSE` | Enable fsync after each write batch |


## Benchmark Design

### Config

Each profile is described by three parameters: 
- `max_concurrent_scan`
- `scan_pool_num_threads`
- `fd_pool_num_threads` 


- **Baseline** (`1/1/16`): near single-threaded — one scan worker, one fd worker, one scan allowed at a time. Used as the reference point for speedup calculation.
- **`concurrent_current`**: the production-style config being tested, with asymmetric thread/concurrency settings.
- **`concurrent_2/3/4`**: symmetric scaling profiles where all three parameters are set to the same value for  scan (e.g. `concurrent_3` = `3/3/16`). Used to observe how throughput and resource usage scale with worker count.

---

### Measured Metrics
- Latency:
  - scan scenarios: average and p95 from accepted `POST /scan` to terminal state `DONE`.
  - `cancel_flow`: average and p95 from accepted `POST /cancel` to terminal state (`CANCELED` or `DROPPED`).
- Throughput:
  - scan scenarios: completed (`DONE`) scans per minute.
  - `cancel_flow` scenario: terminal cancels (`CANCELED` + `DROPPED`) per minute.
- Scan total expected (entries):
  - Formula: `scan_total_expected_entries = done * scan_entries_per_root_expected`.
  - In this run, `scan_entries_per_root_expected=83365` (`dirs_per_root=1365` + `files_per_root=82000`) from fake dataset config.
  - `cancel_flow` uses the same formula and therefore is usually `0` because `done=0` under cancel-target scenarios.
- Backpressure: HTTP 429 count under overload.
- Timeout(-1): client did not receive response (network/transport level).
- Resource usage: process CPU (`avg`, `p95`) and RSS memory peak (MB).
- Per-profile/raw metrics are available in `results.json` for deeper analysis.

### Scenarios
1. `single_big_scan`: one large root scan.
- config in this run: `drop_caches_before_run=true` for `single_big_scan`.
- `single_big_scan` drops Linux page cache before each profile run to keep speedup comparisons closer to cold-cache conditions.
2. `burst_submit`: 12 threads concurrently fire 60 `POST /scan` requests in one shot, then wait for all accepted scans to reach a terminal state. Latency is measured from accepted `POST /scan` timestamp to terminal `DONE`.
3. `overload_queue`: 20 threads hammer `POST /scan` with no sleep for 20 seconds; request total is not fixed — it is determined by how fast the service accepts responses within the time window. Throughput is computed as `DONE` count per elapsed window.
4. `cancel_flow`: two-phase model. Phase 1 — 10 threads submit 40 `POST /scan`; once all submissions complete, immediately snapshot IDs still in `PENDING/RUNNING`. Phase 2 — sleep `cancel_delay_sec`, then 10 threads `POST /cancel` for every snapshotted ID. Cancel latency/throughput are measured from accepted `POST /cancel` to terminal `CANCELED/DROPPED`.
- config in this run: `cancel_delay_sec=1.5`.
- a larger `cancel_delay_sec` means more scans finish before the cancel arrives, increasing `409` conflict rate.

---

### Results
- System: `nproc=16`, `total_mem=7.57 GB`
- `scan_entries_per_root_expected=83365` (`dirs_per_root=1365` + `files_per_root=82000`)
- SQLite DB is deleted before each profile run to ensure a clean starting state.

#### single_big_scan — Speedup vs Baseline

Scenario goal: measure scan-path parallelization efficiency on one large root by comparing completion time against the single-thread baseline.

| Profile | latency (s) | Speedup |
|---|---:|---:|
| baseline_single | 1.7247 | 1.000x |
| concurrent_2 | 0.7664 | 2.250x |
| concurrent_3 | 0.4033 | 4.276x |
| concurrent_4 | 1.4935 | 1.155x |
| concurrent_current | 0.4045 | 4.264x |

---

#### burst_submit — Done Rate (Effective Capacity Under Burst, 60 requests)

Scenario goal: test effective capacity and protection behavior under sudden burst traffic (fixed-count concurrent submit).

| Profile | Req Total | Reject 429 | Done | Done % |  Latency Avg (s) | Latency P95 (s) | CPU avg (%) | RSS peak (MB) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_single | 60 | 55 | 5 | 8.3% |  4.9137 | 8.8290 | 143.49 | 34.66 |
| concurrent_2 | 60 | 54 | 6 | 10.0% | 2.9277 | 5.2761 | 230.86 | 147.57 |
| concurrent_3 | 60 | 53 | 7 | 11.7% | 4.2040 | 9.0353 | 248.18 | 138.88 |
| concurrent_4 | 60 | 52 | 8 | 13.3% |  24.2463 | 34.2080 | 170.01 | 159.47 |
| concurrent_current | 60 | 53 | 7 | 11.7% |  12.7981 | 24.2269 | 168.60 | 138.21 |

---

#### overload_queue — Throughput vs Resource Cost

Scenario goal: test stability under sustained pressure (time-window (20s) continuous submit), including whether the system rejects excess load instead of stalling.

| Profile | Req Total | Done | Reject 429 | Timeout (-1) | Throughput (/min) | CPU avg (%) | CPU p95 (%) | RSS Peak (MB) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_single | 33948 | 20 | 33926 | 0 | 42.482 | 153.15 | 306.85 | 69.62 |
| concurrent_2 | 25024 | 23 | 24998 | 0 | 60.319 | 253.82 | 395.48 | 143.77 |
| concurrent_3 | 21173 | 26 | 21145 | 0 | 63.780 | 286.77 | 460.87 | 143.34 |
| concurrent_4 | 17851 | 22 | 17824 | 0 | 37.780 | 230.81 | 500.00 | 161.84 |
| concurrent_current | 30319 | 21 | 30293 | 0 | 33.858 | 197.94 | 436.36 | 151.00 |

---

#### cancel_flow — Cancel Latency & Conflict Rate

Scenario method: two-phase model. Phase 1 submits a batch of scans concurrently, then snapshots IDs still in `PENDING/RUNNING`; Phase 2 waits `cancel_delay_sec` and sends `POST /cancel` for that snapshot. This measures cancel latency and late-cancel conflict (`409`) when targets have already finished or left cancellable states before `POST /cancel`.

| Profile | Req Total | 409 | DROPPED | CANCELED | DONE | Throughput (/min) | Latency Avg (ms) | Latency P95 (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_single | 5 | 2 | 2 | 1 | 0 | 117.124 | 0.6 | 0.7 |
| concurrent_2 | 6 | 2 | 1 | 3 | 0 | 154.178 | 1.6 | 2.3 |
| concurrent_3 | 7 | 2 | 1 | 4 | 0 | 193.300 | 3.1 | 6.2 |
| concurrent_4 | 8 | 1 | 1 | 6 | 0 | 268.148 | 7.8 | 15.4 |
| concurrent_current | 7 | 1 | 1 | 5 | 0 | 230.431 | 4.0 | 7.0 |

---

## Analysis

### single_big_scan

Scales from `2.250x` (`concurrent_2`) to a peak of `4.276x` (`concurrent_3`). `concurrent_current` closely matches at `4.264x`.

`concurrent_4` collapses to `1.155x` despite having more workers — a clear signal of SQLite write saturation. The single `result_thread` cannot drain file events fast enough when four scan workers produce results simultaneously; the result queue backs up and end-to-end latency nearly returns to baseline.

`concurrent_3` and `concurrent_current` represent the aligned operating point where scan production rate matches `result_thread` drain capacity.

### burst_submit

`Reject 429` is high across all profiles (`52`–`55`), confirming backpressure is active and preventing queue collapse under the 60-request burst.

Effective capacity rises modestly with thread count (`Done`: `5` → `8`; `8.3%` to `13.3%`). Latency spikes sharply at `concurrent_4` (avg `24.2s`, p95 `34.2s`), consistent with the SQLite write saturation seen in `single_big_scan`: more workers complete scans faster than `result_thread` can write results, causing accumulated scan results to queue up.

`concurrent_2` achieves the lowest latency (`2.9s` avg) and `concurrent_3` remains moderate (`4.2s` avg). `concurrent_current` lands at `12.8s` avg — above `concurrent_3` due to its asymmetric thread configuration.

### overload_queue

All profiles record `Timeout(-1)=0`, confirming the HTTP server sheds excess load immediately via `429` without stalling client connections.

Throughput peaks at `concurrent_3` (`63.780/min`) and `concurrent_2` (`60.319/min`). Beyond that, it drops: `concurrent_4` (`37.780/min`) and `concurrent_current` (`33.858/min`). The same SQLite bottleneck dominates: once scan workers saturate `result_thread`, completed scans accumulate in the result queue and effective throughput falls even as raw submission volume rises.

`baseline_single` achieves `42.482/min` — higher than `concurrent_4` because its single worker never overloads the DB write path.

### cancel_flow

Cancel latency stays low across all profiles (avg `0.6`–`7.8 ms`; p95 `0.7`–`15.4 ms`), confirming the cancel control plane is independent of the DB write path — SQLite write saturation does not affect cancel responsiveness.

`409` conflict count decreases with concurrency. At higher concurrency more scans remain actively `RUNNING` when `POST /cancel` arrives after `cancel_delay_sec=1.5`, making them cancellable. At low concurrency, scans complete serially and are already in a terminal state before the cancel request reaches them.

Cancel throughput scales with concurrency: `117.124/min` (`baseline_single`) → `268.148/min` (`concurrent_4`), with `concurrent_current` at `230.431/min`.

## Conclusion
- The system demonstrates clear parallel scan gains, robust backpressure behavior, and fast cancel-path responsiveness.
- The primary bottleneck under load is the single SQLite `result_thread`: when scan workers produce file events faster than it can commit batches to `index_table`, throughput degrades.
- concurrent scan = 3 are the practical operating points: they align scan production rate with DB write throughput and achieve the best end-to-end performance.



## License
This project is licensed under the Apache License 2.0 - see the [LICENSE](./LICENSE) file for details.
