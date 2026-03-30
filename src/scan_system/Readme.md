## Scan System Architecture

```
                        ┌─────────────────────────────┐
                        │          Callers            │
                        │   Scheduler      Daemon     │
                        └────────┬────────────────────┘
                                 │
                                 ▼
                        ┌─────────────────────────────┐
                        │          Manager            │
                        │                             │
                        │  registry (scan_id → Task)  │
                        │  ├── callbacks to Scheduler │
                        │  ├── mutex + condvar        │
                        │  └── dispatch_failed flag   │
                        └────────┬────────────────────┘
                                 │
               ┌─────────────────┼──────────────────────┐
               │                 │                      │
               ▼                 ▼                      ▼
  ┌────────────────────┐  ┌─────────────┐  ┌──────────────────────┐
  │    ThreadPool      │  │ MultiScanner│  │    ExportManager     │
  │  (LIFO JobQueue)   │◄─┤             │  │  (background thread) │
  │                    │  │  worker_job │  │                      │
  │  N worker threads  │─►│  per node   │  │  consumes result     │
  └────────────────────┘  └──────┬──────┘  │  writes to SQLite DB │
                                 │         └──────────────────────┘
                    ┌────────────┴──────────────┐
                    │                           │
                    ▼                           ▼
             ┌────────────┐             ┌───────────────┐
             │    FILE    │             │      DIR      │
             │            │             │               │
             │ record     │             │ opendir +     │
             │ FileEvent  │             │ readdir →     │
             │            │             │ spawn child   │
             └────────────┘             │ ScanData jobs │
                                        └───────────────┘
```

---

## Scan Lifecycle

```
  Scheduler
      │
      │  start_new_scan()
      ▼
  Manager ──── create ScanContext ────► registry
      │
      │  enqueue root job
      ▼
  ThreadPool
      │
      │  dispatch to worker thread
      ▼
  MultiScanner::worker_job()
      │
      ├── is FILE/LINK ──► record FileEvent ──► ScanResult
      │
      └── is DIR ─────────► spawn children
                                 │
                    ┌────────────┴───────────────┐
                    │                            │
                    ▼                            ▼
              queue has room              *queue is full*
                    │                            │
              enqueue to pool            *run on same thread*
                                         (fallback recursion)

      last job finishes (unfinished_jobs → 0)
              │
              ├──► notify Scheduler (scan done)
              ├──► transfer result out of registry
              └──► ExportManager (async write to disk)
```

```
────────────────────────────────────────────────
Case 1: Scan Thread Queue No full
────────────────────────────────────────────────

         A
       / | \
      B  C  D
     / \
    E   F

Step 1: pop A, scan A → children [B,C,D]
        **reverse** submit → queue (LIFO): [B, C, D]
                                            ↑ 
                                           top
Step 2: pop B, scan B → children [E,F]
        **reverse** submit → queue (LIFO): [E, F, C, D]
                                            

Step 3: pop E  →  leaf
Step 4: pop F  →  leaf
Step 5: pop C  →  leaf
Step 6: pop D  →  leaf

Scan order: A → B → E → F → C → D  (iterative DFS ✓)


────────────────────────────────────────────────
Case 2: Queue full when submitting B 
────────────────────────────────────────────────

         A
       / | \
      B  C  D
     / \
    E   F

Step 1: pop A, scan A → children [B,C,D]
        reverse submit:
          submit D → queue: [D]      ✓
          submit C → queue: [C, D]   ✓
          submit B → FULL
                      │
            fall back to single thread to deal B subtree
─────────────────────────────────────────────────────────────────── 
                      └─ worker_job(B) inline          ← [recursive DFS begins]
                            scan B → children [E,F]
                            **reverse** scan:
                                worker_job(F)  ← leaf
                                worker_job(E)  ← leaf
                            finish B                   ← [recursive DFS ends]

── back to parallel ────────────────────────────────────────────────

Step 2: pop E  →  leaf
Step 3: pop F  →  leaf
Step 4: pop C  →  leaf
Step 5: pop D  →  leaf

Scan order: A → B(inline) → F → E → C → D  (DFS ✓, still complete)

---

## Data Ownership

```
  Manager
    └── registry
          └── ScanTask
                ├── root_path
                └── ScanContext  ◄──── shared across all jobs in one scan
                      ├── canceled        (Manager writes, Scanner reads)
                      ├── unfinished_jobs (Scanner increments / decrements)
                      └── ScanResult
                            └── [ FileEvent, FileEvent, FileEvent, ... ]
```


---

**Design Highlights:**
- **LIFO Queue** — deeper nodes are processed first, limiting the number of pending jobs held in memory simultaneously
- **Queue Full Fallback** — when the queue is full, child jobs run synchronously on the current worker thread rather than being dropped
- **Slot Reservation** — `max_concurrent_scan - 1` slots are reserved for root jobs, preventing non-root jobs from starving new scan submissions
- **Shared ScanContext** — all `ScanData` jobs within one scan share a single `ScanContext`; atomic counters coordinate completion detection across threads
