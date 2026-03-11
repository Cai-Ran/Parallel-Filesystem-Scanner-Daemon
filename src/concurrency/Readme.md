## Concurrency Architecture


```

┌─────────────────────────────────────────┐
│             ThreadPool<T>               │
│                                         │
│   NUM_THREADS                           │
│   pool      ──► [ t0 | t1 | ... | tN ]  │
│   job_fn    ──► user-provided callback  │
│                                         │
│   ┌─────────────────────────────────┐   │
│   │         JobQueue<T>             │   │
│   │                                 │   │
│   │  policy   ──► FIFO or LIFO      │   │
│   │  deque<T> ──► bounded buffer    │   │
│   │  metrics  ──► external monitor  │   │
│   └─────────────────────────────────┘   │
└─────────────────────────────────────────┘

```

## Data Flow


```
  Caller
    │
    │  submit(item)
    ▼
┌──────────────┐   Full?      ──► return Full
│  JobQueue    │   Shutdown?  ──► return Shutdown
│              │
│  [ ][ ][ ]   │   ──► push item, notify one thread
└──────┬───────┘
       │
       │  (thread wakes up)
       ▼
┌──────────────────────────────┐
│  Worker Threads              │
│                              │
│  t0: pop → job_fn(item)      │
│  t1: pop → job_fn(item)      │
│  tN: pop → waiting...        │
└──────────────────────────────┘
```
## FIFO vs LIFO

```
  FIFO (HttpServer)          LIFO (Scanner)

  push →  [A][B][C]          push →  [A][B][C]
  pop  ←   A  (front)        pop  ←         C  (back)

  fair request ordering      newest scan job first (iterative DFS-like)
```

## Shutdown 

```
  shutdown()
      │
      ├─► stop_flag = true
      ├─► wake all threads
      │
      └─► threads: pop() returns false → exit
              │
              └─► main thread joins all → done
```



## Composition & Data Flow

```
  ThreadPool<T>
  ┌───────────────────────────────────────────────────────────────┐
  │                                                               │
  │   caller                    JobQueue<T>          N workers    │
  │                          ┌─────────────┐                      │
  │  submit(T&&) ───────────►│  try_push() │                      │
  │                          │             │   work_forever()     │
  │                          │  deque<T>   │◄─── pop() blocks     │
  │  shutdown()  ──────────► │  stop_flag  │     until item or    │
  │                          │  mtx + cv   │     shutdown         │
  │  jobs_in_queue() ───────►│  size()     │                      │
  │                          └─────────────┘                      │
  │                                │                              │
  │                          item popped                          │
  │                                │                              │
  │                                ▼                              │
  │                          job_fn(item)   ← set by start()      │
  └───────────────────────────────────────────────────────────────┘
```

---

## Dispatch Policy Comparison

```
  ┌──────────────────────────────────────────────────────────────────┐
  │  Policy    Pop order   Use case in project                       │
  ├──────────────────────────────────────────────────────────────────┤
  │  LIFO      pop_back()  scan_pool (Manager)                       │
  │                        Newer directory jobs run first → DFS-like │
  │                        keeps working set small; deeper paths     │
  │                        completed before breadth expands          │
  ├──────────────────────────────────────────────────────────────────┤
  │  FIFO      pop_front() fd_pool (HttpServer)                      │
  │                        Requests served in arrival order          │
  │                        fair, predictable latency                 │
  └──────────────────────────────────────────────────────────────────┘
```

---

## Instantiations in the Project

```
  ThreadPool<ScanData>   scan_pool     (Manager)
  ─────────────────────────────────────────────────────────────
  Policy      : LIFO
  queue_size  : Config.manager.scan_queue_max_size
  num_threads : Config.manager.scan_pool_num_threads
  job_fn      : MultiScanner::worker_job(ScanData)
  Metrics     : scan_jobs_submitted / scan_jobs_enqueue_reject
                scan_jobs_queued


  ThreadPool<int>        fd_pool       (HttpServer)
  ─────────────────────────────────────────────────────────────
  Policy      : FIFO
  queue_size  : Config.httpserver.fd_queue_max_size
  num_threads : Config.httpserver.fd_pool_num_threads
  job_fn      : HttpServer::handle_request(int client_fd)
  Metrics     : request_jobs_submitted / request_jobs_failed
                request_jobs_queued
```



