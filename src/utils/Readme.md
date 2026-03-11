## Async Logger Architecture


---


## Logger Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      AsyncLogger                        │
├─────────────────────────────────────────────────────────┤
│                                                         │
│   Caller threads (any)        Log Worker thread (1)     │
│   ─────────────────────       ─────────────────────     │
│                                                         │
│   error() / warn()            worker_loop()             │
│   info()  / debug()               │                     │
│        │                      consume()                 │
│        ▼                          │                     │
│     helper()               ┌──────┴─────────┐           │
│        │                   │   log file     │           │
│   ┌────┴──────────┐        │  (or stderr)   │           │
│   │  log_queue    │───────►│                │           │
│   │  queue<Item>  │        │  write_log()   │           │
│   │  (bounded)    │        └────────────────┘           │
│   └───────────────┘                                     │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

---

## Queue-Full Policy

```
                  log_queue full?
                        │
            ┌───────────┴───────────┐
         Info/Debug              Warn/Error
            │                       │
     Drop immediately           cv.wait()
            │                  until space
            ▼                       │
    fallback to stderr              ▼
       (stderr)               enqueue when free

```

-----

## Queue-Full Behavior by Log Level

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │  Level         Queue full behavior                                   │
  ├──────────────────────────────────────────────────────────────────────┤
  │  Info / Debug  Drop immediately → print_to_terminal() (non-blocking) │
  ├──────────────────────────────────────────────────────────────────────┤
  │  Warn / Error  Block (cv.wait) until queue has space or stop         │
  │                ensures critical messages are not lost                │
  └──────────────────────────────────────────────────────────────────────┘
```

--


## SyncLogger

```
┌──────────────────────────────────────┐
│            SyncLogger                │
│          (Singleton)                 │
│                                      │
│   mtx  ──► protects cerr + buf       │
│                                      │
│   Public API:                        │
│   ┌──────────────────────────────┐   │
│   │  info(msg)                   │   │
│   │  warn(msg)                   │   │
│   │  debug(msg)                  │   │
│   │  error(msg)                  │   │
│   └──────────────────────────────┘   │
└──────────────────────────────────────┘
```



---

## AsyncLogger vs SyncLogger

```
  ┌─────────────────────┬──────────────────┬──────────────────┐
  │                     │  AsyncLogger     │  SyncLogger      │
  ├─────────────────────┼──────────────────┼──────────────────┤
  │  Caller blocks?     │  No              │  Yes             │
  │  Background thread  │  Yes (1)         │  No              │
  │  Output             │  File + stderr   │  stderr only     │
  │  Queue              │  Yes (bounded)   │  No              │
  │  Can drop logs?     │  Yes (Info/Debug)│  Never           │
  │  Metrics tracking   │  Yes             │  No              │
  └─────────────────────┴──────────────────┴──────────────────┘
```

