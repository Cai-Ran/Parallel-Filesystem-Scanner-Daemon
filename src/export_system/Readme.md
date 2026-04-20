## Export System Architecture

```
┌────────────────────────────────────────────────────────────┐
│                        ExportManager                        │
│                                                             │
│  ┌─────────────────────┐   ┌──────────────────────────┐   │
│  │    Result Queue     │   │   Completed Scans Set    │   │
│  │   (bounded MPSC)    │   │  loaded on start(),      │   │
│  │                     │   │  updated after finalize  │   │
│  └──────────┬──────────┘   └──────────────────────────┘   │
│             │ result thread                    ▲            │
│             ▼                                 │ insert      │
│      ┌────────────────────────────────────┐   │            │
│      │           File Event               │   │            │
│      │  normal   ──► batch upsert to DB   │   │            │
│      │  sentinel ──► finalize scan ───────┼───┘            │
│      └────────────────────────────────────┘                │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐  │
│  │               Pending Scan Registry                  │  │
│  │  insert_scan_task() ──► registered before scan       │  │
│  │  consumed by finalize when sentinel arrives          │  │
│  └──────────────────────────────────────────────────────┘  │
└────────────────────────────┬────────────────────────────────┘
                             │
                             ▼
              ┌──────────────────────────────────────┐
              │              db_wrapper/             │
              │                                      │
              │  IndexWriter ──► index_history       │
              │              ──► index_current       │
              │  ScanTable   ──► scan_task_table     │
              │  ScanDiff    ──► scan_diff_table     │
              │  IndexReader ──► queries for HTTP    │
              └──────────────────────────────────────┘
```

---

## Concurrency Model

```
  Scanner threads (multiple)           Result thread (1)
  ──────────────────────────           ──────────────────────────────────
  push file events                     pop from queue
       │                                    │
       │  bounded MPSC queue                ├── normal ──► batch upsert
       └───────────────────────────────────►│              (commit per batch size)
                                            │
                                            └── sentinel ──► commit batch
                                                             finalize scan:
                                                               write scan metadata
                                                               mark deleted files
                                                               compute scan diff
                                                               update completed set
                                                               notify scheduler

  Caller threads                       HTTP worker threads (multiple)
  ──────────────────                   ──────────────────────────────
  insert_scan_task()                   check_exported()  ── completed set (mutex)
  (before scan starts)                 IndexReader calls ── per-request connection
                                                            (SQLite WAL)
```

---

## Database Schema

| Table | Primary Key | Purpose |
|---|---|---|
| `index_history` | `(path, last_scan_id)` | Append-only record of all states per scan |
| `index_current` | `path` | Latest alive/error entry per path (INSERT OR REPLACE) |
| `scan_task_table` | `scan_id` | Scan metadata: root, time, state, counts, total size |
| `scan_diff_table` | `(scan_id, path)` | Per-file diff state relative to previous scan |

WAL mode enabled; `synchronous` configurable via `db.fsync`. Write conflicts use 5000 ms busy timeout.

---

## Export Lifecycle

```
  Scanner threads
       │ push file events
       ▼
  ┌──────────────┐
  │ Result Queue │  bounded MPSC
  └──────┬───────┘
         │ result thread pops
         ▼
     sentinel?
     ─────────────────────────────────────────────────
     no                            yes
     │                             │
     ▼                             ▼
  ┌─────────────────┐     ┌────────────────────────────┐
  │   batch upsert  │     │  commit pending batch      │
  │  index_history  │     │  write scan metadata       │
  │  index_current  │     │  mark deleted files        │
  │  (per batch sz) │     │  compute & write scan diff │
  └─────────────────┘     │  add to completed set      │
                          │  notify scheduler          │
                          └────────────────────────────┘
```

---

## ExportManager Public API

| Method | Description |
|---|---|
| `start()` | Initializes DB schema, loads completed scan IDs, starts result thread |
| `shutdown()` | Sets stop flag, drains queue, joins thread |
| `insert_scan_task()` | Registers a pending scan record before scan starts |
| `push_result_queue()` | Enqueues a file event from scanner threads |
| `update_scan_finish()` | Updates finish time for a scan (direct DB write) |
| `check_exported()` | Batch lookup: returns completion status for a list of scan IDs |
| `set_scheduler_callback()` | Registers callback invoked with submit root after each scan finalizes |

---

## IndexReader Queries

| Method | Description |
|---|---|
| `find_scan_diff_and_upsert_scandiff` | Reads current scan's entries from `index_history`; correlated subquery fetches previous state/size for each path; writes diff result to `scan_diff_table` |
| `get_top_root_in_index` | Derives common parent of all indexed paths via `MIN(path)` / `MAX(path)` |
| `group_by_folder_in_snd_layer` | Fetches all entries under a root glob; aggregates size by second-level directory in C++ |
| `group_by_extension` | Groups files under a root by extension with count and total size |
| `search` | Full-text path search with `LIKE %keyword%`, paginated |
