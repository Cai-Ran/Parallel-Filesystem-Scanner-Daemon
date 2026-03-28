## Export System Architecture

```

┌────────────────────────────────────────────────────────────────┐
│                         ExportManager                          │
│                                                                │
│  ┌─────────────────────┐      ┌──────────────────────────────┐ │
│  │    result_queue     │      │         exported_map         │ │
│  │  JobQueue<FileEvent>│      │  (unordered_set, map_mtx)    │ │
│  │                     │      └──────────────────────────────┘ │
│  │  push_result_queue()│◄── MultiScanner (via Manager)         │
│  │  write_result_to_db │                                       │
│  └──────────┬──────────┘                                       │
│             │  result_thread                                   │
│                                                                │
│  ┌─────────────────────┐                                       │
│  │    delete_queue     │                                       │
│  │ JobQueue<DeleteTask>│◄── Manager (scan complete callback)   │
│  │                     │                                       │
│  │  mark_deleted_in_db │                                       │
│  └──────────┬──────────┘                                       │
│             │  delete_thread                                   │
└─────────────┼──────────────────────────────────────────────────┘
              │
              ▼
   ┌──────────────────────────────────────────────┐
   │                  db_wrapper/                 │
   │                                              │
   │  IndexWriter  ──► index_history (append-only) │
   │               ──► index_current (latest only) │
   │  ScanTable    ──► scan_task_table             │
   │  ScanDiff     ──► scan_diff_table             │
   │  IndexReader  ──► queries served by HTTP API  │
   └──────────────────────────────────────────────┘
```



---

## Thread Safety & Concurrency Model

```
  Producer threads (multiple Scanners)         result_thread (1)
  ────────────────────────────────────         ───────────────────────────────
  push_result_queue(FileEvent)                 write_result_to_db()
       │                                          │
       │  MPSC bounded queue                      │  pop batch
       └─────────────────────────────────────────►│
                                                  │  IndexWriter::upsert()
                                                  │  ScanTable::update_end()
                                                  │  exported_map.insert()  ← map_mtx

  Manager (scan complete callback)             delete_thread (1)
  ──────────────────────────────────           ───────────────────────────────
  push delete root task                        mark_deleted_in_db()
       │                                          │
       │  MPSC bounded queue                      │  pop → IndexWriter::mark_deleted()
       └─────────────────────────────────────────►│

  Reader threads (multiple HttpServer workers)
  ────────────────────────────────────────────
  check_exported()  ── map_mtx ──► exported_map lookup
  IndexReader calls ── DatabaseConnection (per-request, thread-safe via SQLite WAL)
```

---

## Database Schema

| Table | Primary Key | Key Columns |
|---|---|---|
| `index_history` | `(path, last_scan_id)` | `node_type, mtime, msize, state, err_msg, extension` — append-only |
| `index_current` | `path` | `scan_id, node_type, mtime, msize, state, err_msg, extension` — one row per path, latest state |
| `scan_task_table` | `scan_id` | `submit_root, start_time, finish_time, end_state, dir_count, file_count, link_count, total_size` |
| `scan_diff_table` | `(scan_id, path)` | `diff_state, old_size` |

---

## Export Lifecycle

```
  Scanner completes file events
          │
          │  push_result_queue(FileEvent)  [multiple scanner threads]
          ▼
    result_queue  (bounded MPSC)
          │
          │  result_thread drains in batches (batch_size from config)
          ▼
  IndexWriter::upsert()        ──► index_current  (INSERT OR REPLACE, latest state)
                               ──► index_history  (append only, if fingerprint changed)
  ScanTable::upsert()          ──► scan_task_table (record scan)

  Scan root completes
          │
          │  (internal Manager callback)
          ▼
    delete_queue  (bounded MPSC)
          │
          │  delete_thread drains
          ▼
  IndexWriter::mark_deleted()                ──► index_history (INSERT deleted rows)
                                             ──► index_current (DELETE unvisited paths)
  IndexReader::find_scan_diff_and_upsert()   ──► scan_diff_table
  ScanTable::upsert_count()                  ──► scan_task_table (dir/file/link counts)
  exported_map.emplace(scan_id)              ──► GET /exporting returns EXPORTED
```
