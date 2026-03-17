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
- Incremental Index for change detection via file fingerprints
- Async JSON export of scan results and index snapshots
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
make bench-full
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
                                                                                           | - single IO thread         |
                                                                                           | - index latest state       |
                                                                                           +------+---------------+-----+
                                                                                                  |               |
                                                                                                  v               v
                                                      +---------+----------+    update    +-------+-----+   +-----+--------+
                                                      | MetadataIndex      |     <----    | Exporter    |   | IndexReporter|
                                                      | - index version    |              |             |   |              |
                                                      +--------------------+              +-------+-----+   +-----+--------+
                                                                                                  |               |
                                                                                            detail.json       detail.json
                                                                                            summary.json      summary.json
                                                                                           
                                                                                           
                                                                                           
```

---

## State Machine & Polling Rules

```
 Scheduler                                  ExportManager
 poll: GET /state                           poll: GET /exporting
 ─────────────────────────────────────      ─────────────────────────────────
                              phase1
 PENDING ──► RUNNING ────┐    polling          UNAVAILABLE           
    │            │       │                         │
  cancel      cancel     |                         │
    │            │       │                         │
    ▼            ▼       ▼                         ▼
DROPPED✕   CANCELED✕   DONE✕                   EXPORTING       phase2
                                                   │             polling
                                                   ▼
                                                EXPORTED✕

 ✕ = death state, stop polling
```

| State | Phase | Poll | Notes |
|---|---|---|---|
| `PENDING` `RUNNING` | Scheduler | `GET /state` | scan in progress |
| `DONE` `CANCELED` | Scheduler | ✕ stop | scan finished, export starting |
| `DROPPED` `FAILED` | Scheduler | ✕ stop  | — |
| `UNAVAILABLE` | ExportManager | ✕ no polling | scan not yet finished, export not yet started |
| `EXPORTING` | ExportManager | `GET /exporting` | export in progress |
| `EXPORTED` | ExportManager | ✕ stop  | user can fetch results via `/export_summary` `/export_detail` |

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
│   ├── common/                   # Shared data structures (FileEvent, Entry)
│   ├── concurrency/              # ThreadPool, JobQueue templates
│   ├── daemon_system/            # Daemon, HttpServer, Scheduler
│   ├── scan_system/              # Manager, MultiScanner
│   ├── export_system/            # ExportManager, Exporter, MetadataIndex
│   └── utils/                    # Config, AsyncLogger, Metrics, Formater
├── web/                          # Frontend HTML/JS
├── tests/                        # Unit and integration tests
├── benchmark/                    # Performance benchmarking
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
        └── ExportManager (1 IO thread)
                  ├─ MetadataIndex
                  ├─ Exporter
                  └─ IndexReporter          
```


## Configuration

`config.ini` sections and key parameters:

| Section | Key | Default | Description |
|---|---|---|---|
| `[daemon]` | `user_download_sec` | `60` | Grace period after shutdown for client downloads |
| `[httpserver]` | `server_port` | `8080` | Listening port |
| `[httpserver]` | `fd_pool_num_threads` | `8` | HTTP worker thread count |
| `[httpserver]` | `fd_queue_max_size` | `8` | HTTP request queue capacity |
| `[scheduler]` | `max_concurrent_scan` | `3` | Max simultaneous scans |
| `[scheduler]` | `queue_max_size` | `4` | Pending scan queue capacity |
| `[manager]` | `scan_pool_num_threads` | `4` | Scan worker thread count |
| `[manager]` | `scan_queue_max_size` | `1000` | Scan job queue capacity |
| `[asynclogger]` | `log_dir` | *(path)* | Directory for log files |
| `[asynclogger]` | `queue_max_size` | `8192` | Log queue capacity |


## Benchmark Design

### Config

Each profile is described by three parameters: 
- `scan_pool_num_threads`
- `max_concurrent_scan`
- `fd_pool_num_threads` 


- **Baseline** (`1/1/1`): near single-threaded — one scan worker, one fd worker, one scan allowed at a time. Used as the reference point for speedup calculation.
- **`concurrent_current`**: the production-style config being tested, with asymmetric thread/concurrency settings.
- **`concurrent_2/4/8/10`**: symmetric scaling profiles where all three parameters are set to the same value (e.g. `concurrent_8` = `8/8/8`). Used to observe how throughput and resource usage scale with worker count.

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
  - In this run, `scan_entries_per_root_expected=206365` (`dirs_per_root=1365` + `files_per_root=205000`) from fake dataset config.
  - `cancel_flow` uses the same formula and therefore is usually `0` because `done=0` under cancel-target scenarios.
- Backpressure: HTTP 429 count under overload.
- Timeout(-1): client did not receive response (network/transport level).
- Resource usage: process CPU (`avg`, `p95`) and RSS memory peak (MB).
- Store mode (`store`):
  - `store=true`: keep exported result/index files under each profile `exports/` directory.
  - `store=false`: benchmark removes `exports/` directory after setup, so export files are not kept and DOES NOT export I/O at all.
- Per-profile/raw metrics are available in `results.json` for deeper analysis.

### Scenarios
1. `single_big_scan`: one large root scan.
- config in this run: `drop_caches_before_run=true` for `single_big_scan`.
- `single_big_scan` drops Linux page cache before each profile run to keep speedup comparisons closer to cold-cache conditions.
2. `burst_submit`: 12 threads concurrently fire 60 `POST /scan` requests in one shot, then wait for all accepted scans to reach a terminal state. Latency is measured from accepted `POST /scan` timestamp to terminal `DONE`.
3. `overload_queue`: 20 threads hammer `POST /scan` with no sleep for 20 seconds; request total is not fixed — it is determined by how fast the service accepts responses within the time window. Throughput is computed as `DONE` count per elapsed window.
4. `cancel_flow`: two-phase model. Phase 1 — 10 threads submit 40 `POST /scan`; once all submissions complete, immediately snapshot IDs still in `PENDING/RUNNING`. Phase 2 — sleep `cancel_delay_sec`, then 10 threads `POST /cancel` for every snapshotted ID. Cancel latency/throughput are measured from accepted `POST /cancel` to terminal `CANCELED/DROPPED`.
- config in this run: `cancel_delay_sec=0.5`.
- a larger `cancel_delay_sec` means more scans finish before the cancel arrives, increasing `409` conflict rate.

---

### 1. Store mode: `false` - without IO operation and in-mem index
- Store mode in this run: `false`
- System: `nproc=16`, `total_mem=7.57 GB`


#### Summary Table

---

| Scenario        | Profile            |   Config | Latency Avg (s) | Latency P95 (s) | Throughput (/min) | Expect_Scanned Size (entries) | Req_Total | Done | Reject (429) (cancel:409) | Timeout (-1) | CPU avg (%) | CPU p95 (%) | RSS peak (MB) |
| --------------- | ------------------ | -------: | -----: | ------: | -------: | --------------: | --------: | ---: | ---------------: | -----------: | ---------: | ---------: | -----------: |
| single_big_scan | baseline_single    |    1/1/1 |         2.3451 |         2.3451 |            25.472 |                       206365 |         1 |    1 |                         0 |            0 |      74.00 |     115.10 |        47.35 |
| single_big_scan | concurrent_2       |    2/2/2 |         0.8641 |         0.8641 |            68.627 |                       206365 |         1 |    1 |                         0 |            0 |     137.12 |     198.82 |        46.92 |
| single_big_scan | concurrent_4       |    4/4/4 |         0.7295 |         0.7295 |            81.105 |                       206365 |         1 |    1 |                         0 |            0 |     252.07 |     309.68 |        47.09 |
| single_big_scan | concurrent_8       |    8/8/8 |         0.6143 |         0.6143 |            96.069 |                       206365 |         1 |    1 |                         0 |            0 |     467.16 |     589.63 |        49.88 |
| single_big_scan | concurrent_10      | 10/10/10 |         0.5886 |         0.5886 |           100.220 |                       206365 |         1 |    1 |                         0 |            0 |     589.80 |     816.30 |        50.40 |
| single_big_scan | concurrent_current |    3/3/8 |         1.1611 |         1.1611 |            51.228 |                       206365 |         1 |    1 |                         0 |            0 |     209.48 |     258.06 |        47.55 |
| burst_submit    | baseline_single    |    1/1/1 |         3.6481 |         6.4365 |            44.544 |                      1031825 |        60 |    5 |                        55 |            0 |      82.92 |     140.15 |        62.96 |
| burst_submit    | concurrent_2       |    2/2/2 |         3.4290 |         5.4953 |            64.261 |                      1238190 |        60 |    6 |                        54 |            0 |     151.19 |     206.45 |        92.24 |
| burst_submit    | concurrent_4       |    4/4/4 |         2.0618 |         3.1374 |           146.819 |                      1650920 |        60 |    8 |                        52 |            0 |     329.79 |     412.90 |       164.01 |
| burst_submit    | concurrent_8       |    8/8/8 |         2.5838 |         4.5153 |           154.804 |                      2476380 |        60 |   12 |                        48 |            0 |     605.92 |     742.86 |       276.46 |
| burst_submit    | concurrent_10      | 10/10/10 |         2.5445 |         4.1243 |           188.488 |                      2889110 |        60 |   14 |                        46 |            0 |     755.11 |     906.67 |       524.92 |
| burst_submit    | concurrent_current |    3/3/8 |         3.3450 |         4.8779 |            82.882 |                      1444555 |        60 |    7 |                        53 |            0 |     222.55 |     290.91 |       130.06 |
| overload_queue  | baseline_single    |    1/1/1 |        11.7896 |        20.0092 |            74.647 |                      5778220 |     39462 |   28 |                     39434 |            0 |     107.65 |     150.52 |        88.80 |
| overload_queue  | concurrent_2       |    2/2/2 |        12.5517 |        20.0087 |            99.375 |                      7635505 |     34002 |   37 |                     33965 |            0 |     193.62 |     244.14 |       105.29 |
| overload_queue  | concurrent_4       |    4/4/4 |        11.4750 |        20.4077 |           148.694 |                     11556440 |     26658 |   56 |                     26602 |            0 |     352.90 |     463.16 |       173.72 |
| overload_queue  | concurrent_8       |    8/8/8 |        11.3031 |        20.0138 |           209.293 |                     16509200 |     17288 |   80 |                     17208 |            0 |     713.81 |     984.94 |       628.34 |
| overload_queue  | concurrent_10      | 10/10/10 |        12.3456 |        20.0169 |           218.726 |                     17747390 |     17949 |   86 |                     17863 |            0 |     840.77 |    1131.47 |       846.31 |
| overload_queue  | concurrent_current |    3/3/8 |        10.9769 |        20.0087 |           132.709 |                     10111885 |     29239 |   49 |                     29190 |            0 |     291.24 |     384.21 |       148.23 |
| cancel_flow     | baseline_single    |    1/1/1 |         0.0009 |         0.0012 |           556.371 |                            0 |         5 |    0 |                         0 |            0 |     113.15 |     153.63 |        44.09 |
| cancel_flow     | concurrent_2       |    2/2/2 |         0.0014 |         0.0018 |           669.332 |                            0 |         6 |    0 |                         0 |            0 |     168.34 |     204.52 |        58.78 |
| cancel_flow     | concurrent_4       |    4/4/4 |         0.0019 |         0.0032 |           764.345 |                            0 |         8 |    0 |                         1 |            0 |     298.53 |     385.00 |        75.75 |
| cancel_flow     | concurrent_8       |    8/8/8 |         0.0069 |         0.0141 |          1252.213 |                            0 |        12 |    0 |                         0 |            0 |     684.36 |     793.55 |       234.21 |
| cancel_flow     | concurrent_10      | 10/10/10 |         0.0050 |         0.0103 |          1486.604 |                            0 |        14 |    0 |                         0 |            0 |     853.58 |     950.00 |       290.84 |
| cancel_flow     | concurrent_current |    3/3/8 |         0.0015 |         0.0023 |           665.539 |                            0 |         7 |    0 |                         1 |            0 |     219.38 |     258.06 |        49.75 |

---

-----

### 2. Store mode: `true` - with IO operation and in-mem index
- Store mode in this run: `true`
- System: `nproc=16`, `total_mem=7.57 GB`

#### Summary Table
---

| Scenario        | Profile            |   Config | Latency Avg(s) | Latency P95(s) | Throughput (/min) | Expect_Scanned Size(entries) | Req_Total | Done | Reject (429) (cancel:409) | Timeout (-1) | CPU avg (%) | CPU p95 (%) | RSS peak (MB) |
| --------------- | ------------------ | -------: | -------------: | -------------: | ----------------: | ---------------------------: | --------: | ---: | ------------------------: | -----------: | ---------: | ---------: | -----------: |
| single_big_scan | baseline_single    |    1/1/1 |         1.4773 |         1.4773 |            40.337 |                       206365 |         1 |    1 |                         0 |            0 |      66.09 |     100.00 |        51.90 |
| single_big_scan | concurrent_2       |    2/2/2 |         1.2933 |         1.2933 |            46.031 |                       206365 |         1 |    1 |                         0 |            0 |     137.73 |     198.79 |        51.54 |
| single_big_scan | concurrent_4       |    4/4/4 |         0.6077 |         0.6077 |            97.121 |                       206365 |         1 |    1 |                         0 |            0 |     254.52 |     320.00 |        55.72 |
| single_big_scan | concurrent_8       |    8/8/8 |         0.5387 |         0.5387 |           109.329 |                       206365 |         1 |    1 |                         0 |            0 |     453.85 |     551.21 |        49.98 |
| single_big_scan | concurrent_10      | 10/10/10 |         1.0503 |         1.0503 |            56.579 |                       206365 |         1 |    1 |                         0 |            0 |     625.59 |     786.21 |        49.97 |
| single_big_scan | concurrent_current |    3/3/8 |         0.6761 |         0.6761 |            87.437 |                       206365 |         1 |    1 |                         0 |            0 |     191.45 |     252.82 |        48.20 |
| burst_submit    | baseline_single    |    1/1/1 |         3.5877 |         5.9855 |            47.662 |                      1031825 |        60 |    5 |                        55 |            0 |      97.03 |     193.94 |       289.61 |
| burst_submit    | concurrent_2       |    2/2/2 |         7.1341 |        11.4584 |            30.795 |                      1238190 |        60 |    6 |                        54 |            0 |     202.06 |     278.32 |       333.66 |
| burst_submit    | concurrent_4       |    4/4/4 |         2.2399 |         3.7129 |           124.535 |                      1650920 |        60 |    8 |                        52 |            0 |     327.00 |     418.75 |       447.82 |
| burst_submit    | concurrent_8       |    8/8/8 |         3.7088 |         6.6572 |           102.385 |                      2476380 |        60 |   12 |                        48 |            0 |     561.00 |     670.97 |       713.49 |
| burst_submit    | concurrent_10      | 10/10/10 |        11.8861 |        14.7046 |            53.155 |                      2889110 |        60 |   14 |                        46 |            0 |     847.65 |    1263.16 |       893.16 |
| burst_submit    | concurrent_current |    3/3/8 |         2.0938 |         3.3442 |           121.279 |                      1444555 |        60 |    7 |                        53 |            0 |     253.39 |     350.00 |       431.88 |
| overload_queue  | baseline_single    |    1/1/1 |        11.2498 |        20.0082 |            90.789 |                      7016410 |     45936 |   34 |                     45902 |            0 |     129.21 |     200.00 |      1561.33 |
| overload_queue  | concurrent_2       |    2/2/2 |        13.4745 |        20.2220 |            51.967 |                      4127300 |     16442 |   20 |                     16422 |            0 |     209.65 |     292.27 |       947.91 |
| overload_queue  | concurrent_4       |    4/4/4 |        11.6289 |        20.0097 |           125.778 |                     10111885 |     30855 |   49 |                     30806 |            0 |     326.26 |     421.05 |      2614.78 |
| overload_queue  | concurrent_8       |    8/8/8 |        12.8050 |        20.0287 |           159.435 |                     14239185 |     12830 |   69 |                     12761 |            0 |     679.11 |     907.46 |      4192.60 |
| overload_queue  | concurrent_10      | 10/10/10 |        13.2367 |        20.0293 |           109.019 |                      9905520 |      8073 |   48 |                      8025 |            0 |     800.98 |    1129.85 |      2938.85 |
| overload_queue  | concurrent_current |    3/3/8 |        11.0301 |        20.0059 |           149.846 |                     11556440 |     39023 |   56 |                     38967 |            0 |     289.46 |     376.47 |      2994.75 |
| cancel_flow     | baseline_single    |    1/1/1 |         0.0011 |         0.0013 |           447.933 |                            0 |         5 |    0 |                         1 |            0 |     106.67 |     182.21 |        94.00 |
| cancel_flow     | concurrent_2       |    2/2/2 |         0.0013 |         0.0020 |           554.819 |                            0 |         6 |    0 |                         1 |            0 |     196.25 |     282.35 |       109.51 |
| cancel_flow     | concurrent_4       |    4/4/4 |         0.0030 |         0.0053 |           732.081 |                            0 |         8 |    0 |                         1 |            0 |     327.62 |     422.54 |       179.89 |
| cancel_flow     | concurrent_8       |    8/8/8 |         0.0069 |         0.0160 |          1260.935 |                            0 |        12 |    0 |                         0 |            0 |     718.91 |     856.00 |       148.23 |
| cancel_flow     | concurrent_10      | 10/10/10 |         0.0111 |         0.0235 |          1461.763 |                            0 |        14 |    0 |                         0 |            0 |     793.25 |     950.71 |       200.22 |
| cancel_flow     | concurrent_current |    3/3/8 |         0.0018 |         0.0029 |           625.277 |                            0 |         7 |    0 |                         1 |            0 |     304.30 |     405.40 |       153.12 |

---

---

### 3. Key Metric Comparisons by Scenario

### single_big_scan — Speedup vs Baseline

Scenario goal: measure scan-path parallelization efficiency on one large root by comparing completion time against the single-thread baseline.

##### store=false

| Profile | elapsed (s) | Speedup |
|---|---:|---:|
| baseline_single | 2.3555 | 1.000x |
| concurrent_2 | 0.8743 | 2.694x |
| concurrent_4 | 0.7398 | 3.184x |
| concurrent_8 | 0.6246 | 3.771x |
| concurrent_10 | 0.5987 | 3.934x |
| concurrent_current | 1.1712 | 2.011x |

##### store=true

| Profile | elapsed (s) | Speedup |
|---|---:|---:|
| baseline_single | 1.4875 | 1.000x |
| concurrent_2 | 1.3035 | 1.141x |
| concurrent_4 | 0.6178 | 2.408x |
| concurrent_8 | 0.5488 | 2.710x |
| concurrent_10 | 1.0605 | 1.403x |
| concurrent_current | 0.6862 | 2.168x |

---

#### burst_submit — Done Rate (Effective Capacity Under Burst, 60 requests)

Scenario goal: test effective capacity and protection behavior under sudden burst traffic (fixed-count concurrent submit).

##### store=false

| Profile | Req Total | Reject 429 | Done | Done % | Throughput (/min) | Latency Avg (s) | Latency P95 (s) | CPU avg (%) | RSS peak (MB) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_single | 60 | 55 | 5 | 8.3% | 44.544 | 3.6481 | 6.4365 | 82.92 | 62.96 |
| concurrent_2 | 60 | 54 | 6 | 10.0% | 64.261 | 3.4290 | 5.4953 | 151.19 | 92.24 |
| concurrent_4 | 60 | 52 | 8 | 13.3% | 146.819 | 2.0618 | 3.1374 | 329.79 | 164.01 |
| concurrent_8 | 60 | 48 | 12 | 20.0% | 154.804 | 2.5838 | 4.5153 | 605.92 | 276.46 |
| concurrent_10 | 60 | 46 | 14 | 23.3% | 188.488 | 2.5445 | 4.1243 | 755.11 | 524.92 |
| concurrent_current | 60 | 53 | 7 | 11.7% | 82.882 | 3.3450 | 4.8779 | 222.55 | 130.06 |

##### store=true

| Profile | Req Total | Reject 429 | Done | Done % | Throughput (/min) | Latency Avg (s) | Latency P95 (s) | CPU avg (%) | RSS peak (MB) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_single | 60 | 55 | 5 | 8.3% | 47.662 | 3.5877 | 5.9855 | 97.03 | 289.61 |
| concurrent_2 | 60 | 54 | 6 | 10.0% | 30.795 | 7.1341 | 11.4584 | 202.06 | 333.66 |
| concurrent_4 | 60 | 52 | 8 | 13.3% | 124.535 | 2.2399 | 3.7129 | 327.00 | 447.82 |
| concurrent_8 | 60 | 48 | 12 | 20.0% | 102.385 | 3.7088 | 6.6572 | 561.00 | 713.49 |
| concurrent_10 | 60 | 46 | 14 | 23.3% | 53.155 | 11.8861 | 14.7046 | 847.65 | 893.16 |
| concurrent_current | 60 | 53 | 7 | 11.7% | 121.279 | 2.0938 | 3.3442 | 253.39 | 431.88 |

---

#### overload_queue — Throughput vs Resource Cost

Scenario goal: test stability under sustained pressure (time-window (20s) continuous submit), including whether the system rejects excess load instead of stalling.

##### store=false

| Profile | Req Total | Done | Reject 429 | Timeout (-1) | Throughput (/min) | CPU avg(%) | CPU p95 (%) | RSS Peak (MB) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_single | 39462 | 28 | 39434 | 0 | 74.647 | 107.65 | 150.52 | 88.80 |
| concurrent_2 | 34002 | 37 | 33965 | 0 | 99.375 | 193.62 | 244.14 | 105.29 |
| concurrent_4 | 26658 | 56 | 26602 | 0 | 148.694 | 352.90 | 463.16 | 173.72 |
| concurrent_8 | 17288 | 80 | 17208 | 0 | 209.293 | 713.81 | 984.94 | 628.34 |
| concurrent_10 | 17949 | 86 | 17863 | 0 | 218.726 | 840.77 | 1131.47 | 846.31 |
| concurrent_current | 29239 | 49 | 29190 | 0 | 132.709 | 291.24 | 384.21 | 148.23 |

##### store=true

| Profile | Req Total | Done | Reject 429 | Timeout (-1) | Throughput (/min) | CPU avg (%) | CPU p95 (%) | RSS Peak (MB) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_single | 45936 | 34 | 45902 | 0 | 90.789 | 129.21 | 200.00 | 1561.33 |
| concurrent_2 | 16442 | 20 | 16422 | 0 | 51.967 | 209.65 | 292.27 | 947.91 |
| concurrent_4 | 30855 | 49 | 30806 | 0 | 125.778 | 326.26 | 421.05 | 2614.78 |
| concurrent_8 | 12830 | 69 | 12761 | 0 | 159.435 | 679.11 | 907.46 | 4192.60 |
| concurrent_10 | 8073 | 48 | 8025 | 0 | 109.019 | 800.98 | 1129.85 | 2938.85 |
| concurrent_current | 39023 | 56 | 38967 | 0 | 149.846 | 289.46 | 376.47 | 2994.75 |


---

#### cancel_flow — Cancel Latency & Conflict Rate

Scenario method: two-phase model. Phase 1 submits a batch of scans concurrently, then snapshots IDs still in `PENDING/RUNNING`; Phase 2 waits `cancel_delay_sec` and sends `POST /cancel` for that snapshot. This measures cancel latency and late-cancel conflict (`409`) when targets have already finished or left cancellable states before `POST /cancel`.

##### store=false

| Profile | Req Total | 409 | DROPPED | CANCELED | DONE | Throughput (/min) | Latency Avg (ms) | Latency P95 (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_single | 5 | 0 | 0 | 5 | 0 | 556.371 | 0.9 | 1.2 |
| concurrent_2 | 6 | 0 | 1 | 5 | 0 | 669.332 | 1.4 | 1.8 |
| concurrent_4 | 8 | 1 | 0 | 7 | 0 | 764.345 | 1.9 | 3.2 |
| concurrent_8 | 12 | 0 | 2 | 10 | 0 | 1252.213 | 6.9 | 14.1 |
| concurrent_10 | 14 | 0 | 0 | 14 | 0 | 1486.604 | 5.0 | 10.3 |
| concurrent_current | 7 | 1 | 1 | 5 | 0 | 665.539 | 1.5 | 2.3 |

##### store=true

| Profile | Req Total | 409 | DROPPED | CANCELED | DONE | Throughput (/min) | Latency Avg (ms) | Latency P95 (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_single | 5 | 1 | 2 | 2 | 0 | 447.933 | 1.1 | 1.3 |
| concurrent_2 | 6 | 1 | 1 | 4 | 0 | 554.819 | 1.3 | 2.0 |
| concurrent_4 | 8 | 1 | 0 | 7 | 0 | 732.081 | 3.0 | 5.3 |
| concurrent_8 | 12 | 0 | 0 | 12 | 0 | 1260.935 | 6.9 | 16.0 |
| concurrent_10 | 14 | 0 | 1 | 13 | 0 | 1461.763 | 11.1 | 23.5 |
| concurrent_current | 7 | 1 | 1 | 5 | 0 | 625.277 | 1.8 | 2.9 |


---

## Analysis

### single_big_scan

Scenario goal: measure scan-path parallelization efficiency on one large root.

`store=false` scales from `2.694x` (`concurrent_2`) to `3.934x` (`concurrent_10`), with normal diminishing returns at higher worker counts.

`store=true` peaks at `concurrent_8` (`2.710x`) and drops at `concurrent_10` (`1.403x`) because this mode adds a single-thread IO export path plus in-memory index maintenance. When scan production rate exceeds that path's drain rate, queueing overhead grows and net speedup falls.

`concurrent_current (3/3/8)` remains a balanced point (`2.011x` without store IO, `2.168x` with store IO).

### burst_submit

Scenario goal: measure effective capacity and protection behavior under an instantaneous fixed burst (`60` requests).

`Reject 429` is high in all profiles (`46` to `55`), confirming backpressure is active.

Effective capacity is shown by `Done` (`5, 6, 8, 12, 14, 7`; `8.3%` to `23.3%` done rate), and the acceptance pattern is the same across store modes.

The key difference is post-scan draining cost in `store=true`: completed scans still pass through single-thread IO export and in-memory index maintenance. If scans complete faster than that path can drain, work accumulates, RSS rises, and tail latency expands, which can collapse burst throughput on aggressive profiles (`concurrent_10`: `53.155/min`, `11.8861s`, `893.16 MB`).

Under that constraint, `concurrent_current` is more efficient than high-thread symmetric settings in `store=true` (`121.279/min`, `2.0938s`, `431.88 MB`).

### overload_queue

Scenario goal: measure stability under sustained pressure and quantify throughput vs resource cost.

All profiles show very high `429` with `Timeout(-1)=0`, which indicates HTTP-server backpressure is working as intended: overload is shed immediately instead of stalling into client timeouts.

Done ratio is consistently very low (below `0.6%`) because this scenario is intentionally driven into the backpressure region, where the primary behavior under sustained overload is fast reject (`429`) to protect service stability.

`store=false` throughput scales up to `218.726/min` (`concurrent_10`).

`store=true` is non-monotonic (`concurrent_8` best at `159.435/min`, `concurrent_10` down to `109.019/min`) with higher CPU p95 and heavy RSS. This is consistent with the same bottleneck: single-thread IO export + in-memory index maintenance becoming the limiter under sustained high scan completion rates.

`concurrent_current` is a practical middle ground in `store=true` (`149.846/min`, CPU p95 `376.47%`, RSS `2994.75 MB`).

### cancel_flow

Scenario goal: measure cancel control-plane responsiveness and terminal convergence after delayed cancel submission.

Cancel latency remains low (avg `0.9` to `11.1 ms`).

`409` stays low (`0` or `1` per profile). In this benchmark, `409` means the target scan had already completed (or left the cancellable state) at `POST /cancel` time; the low count suggests `cancel_delay_sec=0.5` is a reasonable simulation of user-triggered cancel timing.

`DONE` is `0` in this scenario window; convergence for accepted cancels is represented by `CANCELED` + `DROPPED`.

Cancel throughput rises with concurrency, from `447.933/min` (`baseline_single`, store=true) to about `1461.763` to `1486.604/min` (`concurrent_10`).

## Conclusion
- The system demonstrates clear parallel scan gains, robust backpressure behavior, and fast cancel-path responsiveness.
- The primary bottleneck under load remains export-side pressure in `store=true`.
- `concurrent_current (3/3/8)` is a practical production trade-off: strong throughput with lower CPU/RSS risk than aggressive high-thread symmetric profiles.



## License
This project is licensed under the Apache License 2.0 - see the [LICENSE](./LICENSE) file for details.

