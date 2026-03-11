## Export System Architecture

```

┌─────────────────────────────────────────────────────────┐
│                      ExportManager                      │
│                                                         │
│  ┌─────────────────┐        ┌────────────────────────┐  │
│  │  result_queue   │        │        Maps            │  │
│  │  (MPSC queue)   │        │  export_map (scan_id)  │  │
│  │                 │        │  index_map  (version)  │  │
│  │  push_queue() ◄─┼─────── │  current_index         │  │
│  │  run() ─────────┼──┐     └────────────────────────┘  │
│  └─────────────────┘  │                                 │
└───────────────────────┼─────────────────────────────────┘
                        │ (single IO thread)
           ┌────────────┴─────────────┐
           │                          │
           ▼                          ▼
   ┌──────────────┐          ┌────────────────┐
   │   Exporter   │          │ IndexReporter  │
   │              │          │                │
   │  scan detail │          │  index detail  │
   │  scan summary│          │  index summary │
   └──────┬───────┘          └───────┬────────┘
          │ update                   │ read
          └──────────┬───────────────┘
                     ▼
             ┌───────────────┐
             │ MetadataIndex │
             │  path → Entry │
             │  version_cnt  │
             └───────────────┘
```



---

## Thread Safety & Concurrency Model

```
  Producer threads (multiple Scanners)         Consumer thread (1 IO thread)
  ────────────────────────────────────         ─────────────────────────────
  push_queue(ExportData)                       run()  ← blocking wait
       │                                          │
       │  queue_mtx + notify_one()                │  wait(cv) → pop
       └─────────────────────────────────────────►│
                                                  │
                                          Exporter writes to MetadataIndex
                                                  │
                                      map_mtx  ◄──┤
                                                  │  export_map.emplace()
                                                  │  index_map.emplace()
                                                  │
  Reader threads (multiple HttpServer)            │
  ────────────────────────────────────            │
  check_exported()   ── map_mtx ─────────────────►│ (concurrent reads)
  get_scan_result()  ── map_mtx ─────────────────►│
  get_index_result() ── map_mtx ─────────────────►│
  get_newest_index() ── map_mtx ─────────────────►│

  MetadataIndex internal:
    iterate_entry()     (IndexReporter / Exporter::check_deleted()) ── mtx ──► read
    update() / erase()  (Exporter::update_index())                  ── mtx ──► write
```

---

## Output JSON File Naming Convention

| Type | Pattern | Producer |
|------|---------|----------|
| Scan detail | `scan_result_ID_{scan_id}_T_{time}.json` | `Exporter::export_result()` |
| Scan summary | `scan_summary_ID_{scan_id}_T_{time}.json` | `Exporter::export_summary()` |
| Index detail | `index_detail_V_{version}_T_{time}.json` | `IndexReporter::export_index_detail()` |
| Index summary | `index_summary_V_{version}_T_{time}.json` | `IndexReporter::export_index_summary()` |

> **Crash-safe writes**: all files are first written to a `.tmp` path, then atomically `rename()` to the final path — readers always see a complete file, never a partial one.
 files are first written to a `.tmp` path, then atomically `rename()` to the final path — readers always see a complete file, never a partial one.
