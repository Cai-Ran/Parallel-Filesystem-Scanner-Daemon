# db_wrapper API Time Complexity

## Variables
- `N` = index_history total rows = T × K (T = number of scans done, K ≈ files per scan)
- `K` = alive paths in index_current (total unique files currently alive)
- `M` = scan_diff_table total rows
- `S` = scan_task_table rows = T

---

## IndexWriter

| API | Complexity | Notes |
|---|---|---|
| `upsert()` × K | O(K log K) avg, O(K log N) worst | Fingerprint read from index_current PK O(log K); if unchanged, skip history insert; worst case when all files changed: O(log N) per history insert |
| `mark_deleted()` | O(K) | GLOB index_current + simple `scan_id != ?` condition, no correlated subquery |

## IndexReader

| API | Complexity | Notes |
|---|---|---|
| `find_scan_diff_and_upsert_scandiff()` | O(K log N) | index on last_scan_id finds K rows; each row runs two correlated subqueries on index_history for prev state/size |
| `group_by_folder_in_snd_layer()` | O(K) | GLOB scan on index_current; second-layer grouping done in C++ |
| `group_by_extension()` | O(K) | GLOB scan on index_current; GROUP BY done in SQL |
| `search()` | O(K) | `LIKE %keyword%` leading-wildcard — full table scan on index_current, no index used; paginated with LIMIT/OFFSET |
| `get_top_root_in_index()` | O(log K) | MIN/MAX on path PK |

## ScanDiff

| API | Complexity | Notes |
|---|---|---|
| `upsert()` × D | O(D log M) | INSERT per diff row |
| `get_scan_diff_count()` | O(log M + D) | PK (scan_id, path) prefix scan |
| `get_scan_diff_detail()` | O(D log N) per page | JOIN index_history on PK (path, last_scan_id), O(log N) per row; paginated with LIMIT/OFFSET |

## ScanTable

| API | Complexity | Notes |
|---|---|---|
| `upsert()` / `get_data()` / `get_max_id()` | O(log S) | PK lookup |
| `upsert_count()` | O(K) | Full scan index_current WHERE scan_id = ? |
| `get_all()` | O(S) | Full scan scan_task_table |

---

## Improvements vs Previous Design

| API | Before | After |
|---|---|---|
| `upsert()` fingerprint compare | O(log N) per file (correlated subquery on index_history) | O(log K) per file (PK lookup on index_current) |
| `mark_deleted()` | O(K log N) | O(K) |
| `group_by_folder/extension/search` | O(K log N) | O(K) |
| `get_top_root_in_index()` | O(N) | O(log K) |

## Remaining Bottleneck

`find_scan_diff_and_upsert_scandiff()` still O(K log N) — acceptable because it runs in background, not on HTTP critical path.

`group_by_folder_in_snd_layer()` and `group_by_extension()` are O(K) but K = 1.5M paths → full disk I/O scan → practical HTTP response ~9s. Complexity class improved but wall-clock bottleneck remains.
