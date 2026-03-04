from typing import Dict, List
from helpers import as_float, as_bool, as_int, percentile
import statistics


U64_WRAP_SUSPECT_THRESHOLD = 1 << 63

def _mark_metric_hit(hit_map: Dict[str, Dict[str, int]], kind: str, key: str):
    if kind not in hit_map:
        hit_map[kind] = {}
    hit_map[kind][key] = hit_map[kind].get(key, 0) + 1


def _metric_u64_sanitized(
    metrics: Dict,
    key: str,
    anomalies: Dict[str, int],
    anomaly_metric_hits: Dict[str, Dict[str, int]],
) -> int:
    raw = as_int(metrics.get(key, 0), 0)
    if raw < 0:
        anomalies["negative_metric"] = anomalies.get("negative_metric", 0) + 1
        _mark_metric_hit(anomaly_metric_hits, "negative_metric", key)
        return 0
    if raw >= U64_WRAP_SUSPECT_THRESHOLD:
        anomalies["u64_wrap_suspect"] = anomalies.get("u64_wrap_suspect", 0) + 1
        _mark_metric_hit(anomaly_metric_hits, "u64_wrap_suspect", key)
        return 0
    return raw


def summarize_resources(samples: List[Dict]) -> Dict:
    cpu_values = []
    rss_values_mb = []
    for s in samples:
        cpu = s.get("cpu_percent")
        rss_bytes = s.get("rss_bytes")
        if cpu is not None:
            cpu_values.append(float(cpu))
        if rss_bytes is not None:
            rss_values_mb.append(float(rss_bytes) / (1024.0 * 1024.0))

    cpu_avg = round(statistics.mean(cpu_values), 3) if cpu_values else 0.0
    cpu_p95 = round(percentile(cpu_values, 0.95), 3) if cpu_values else 0.0
    cpu_peak = round(max(cpu_values), 3) if cpu_values else 0.0

    rss_avg_mb = round(statistics.mean(rss_values_mb), 3) if rss_values_mb else 0.0
    rss_peak_mb = round(max(rss_values_mb), 3) if rss_values_mb else 0.0

    return {
        "cpu_avg_percent": cpu_avg,
        "cpu_p95_percent": cpu_p95,
        "cpu_peak_percent": cpu_peak,
        "rss_avg_mb": rss_avg_mb,
        "rss_peak_mb": rss_peak_mb,
    }


def summarize_metrics(samples: List[Dict], start_metrics: Dict, end_metrics: Dict) -> Dict:
    max_scan_pending = 0
    max_scan_jobs_queued = 0
    max_request_jobs_queued = 0
    max_scan_running = 0
    avg_scan_running = 0.0
    anomalies = {}
    anomaly_metric_hits = {}

    if samples:
        scan_running_values = []
        for s in samples:
            m = s["metrics"]
            scan_pending = _metric_u64_sanitized(m, "scan_pending", anomalies, anomaly_metric_hits)
            scan_jobs_queued = _metric_u64_sanitized(m, "scan_jobs_queued", anomalies, anomaly_metric_hits)
            request_jobs_queued = _metric_u64_sanitized(m, "request_jobs_queued", anomalies, anomaly_metric_hits)
            sr = _metric_u64_sanitized(m, "scan_running", anomalies, anomaly_metric_hits)
            max_scan_pending = max(max_scan_pending, scan_pending)
            max_scan_jobs_queued = max(max_scan_jobs_queued, scan_jobs_queued)
            max_request_jobs_queued = max(max_request_jobs_queued, request_jobs_queued)
            scan_running_values.append(sr)
            max_scan_running = max(max_scan_running, sr)
        avg_scan_running = float(statistics.mean(scan_running_values)) if scan_running_values else 0.0

    def delta(key: str) -> int:
        start_val = _metric_u64_sanitized(start_metrics, key, anomalies, anomaly_metric_hits)
        end_val = _metric_u64_sanitized(end_metrics, key, anomalies, anomaly_metric_hits)
        if end_val < start_val:
            anomalies["delta_negative"] = anomalies.get("delta_negative", 0) + 1
            _mark_metric_hit(anomaly_metric_hits, "delta_negative", key)
            return 0
        return end_val - start_val

    def hits_to_text(kind: str) -> str:
        items = anomaly_metric_hits.get(kind, {})
        if not items:
            return ""
        parts = []
        for k in sorted(items.keys()):
            parts.append(f"{k}:{items[k]}")
        return ",".join(parts)

    return {
        "max_scan_pending": max_scan_pending,
        "max_scan_jobs_queued": max_scan_jobs_queued,
        "max_request_jobs_queued": max_request_jobs_queued,
        "max_scan_running": max_scan_running,
        "avg_scan_running": round(avg_scan_running, 3),
        "delta_scan_jobs_enqueue_reject": delta("scan_jobs_enqueue_reject"),
        "delta_request_jobs_failed": delta("request_jobs_failed"),
        "delta_export_finished": delta("export_finished"),
        "anomaly_u64_wrap_suspect": anomalies.get("u64_wrap_suspect", 0),
        "anomaly_negative_metric": anomalies.get("negative_metric", 0),
        "anomaly_delta_negative": anomalies.get("delta_negative", 0),
        "anomaly_metrics_wrap": hits_to_text("u64_wrap_suspect"),
        "anomaly_metrics_negative": hits_to_text("negative_metric"),
        "anomaly_metrics_delta_negative": hits_to_text("delta_negative"),
        "metrics_anomaly_count": sum(anomalies.values()),
    }



def build_report(run_meta: Dict, rows: List[Dict]) -> str:
    cancel_rows = [r for r in rows if r.get("scenario") == "cancel_flow" or r.get("scenario_type") == "cancel"]
    cancel_delay_values = []
    for r in cancel_rows:
        delay = r.get("cancel_delay_sec")
        if delay is not None:
            cancel_delay_values.append(as_float(delay, 0.0))

    cancel_delay_text = "n/a"
    if cancel_delay_values:
        unique_delays = sorted(set(cancel_delay_values))
        cancel_delay_text = ", ".join(f"{v:g}" for v in unique_delays)

    store_flags = []
    for r in rows:
        if "store" in r:
            store_flags.append(as_bool(r.get("store"), False))

    if store_flags:
        unique_store = sorted(set(store_flags))
        if len(unique_store) == 1:
            store_mode_text = "true" if unique_store[0] else "false"
        else:
            store_mode_text = "mixed(" + ",".join("true" if v else "false" for v in unique_store) + ")"
    else:
        store_mode_text = "unknown"


    entries_per_root = as_int(run_meta.get("scan_entries_per_root_expected", 0), 0)
    dirs_per_root = as_int(run_meta.get("scan_dirs_per_root_expected", 0), 0)
    files_per_root = as_int(run_meta.get("scan_files_per_root_expected", 0), 0)

    lines = []
    lines.append("# Benchmark Report")
    lines.append("")
    lines.append(f"- Generated at (UTC): {run_meta['generated_at_utc']}")
    lines.append(f"- Run directory: `{run_meta['run_dir']}`")
    lines.append(f"- Project root: `{run_meta['project_root']}`")
    lines.append(f"- Store mode in this run: `{store_mode_text}`")
    lines.append("")

    lines.append("## 1. Benchmark Goals")
    lines.append("- Prove concurrency value: faster execution and higher throughput.")
    lines.append("- Prove system control under pressure: backpressure instead of crash.")
    lines.append("- Prove observability: `/metrics` can explain runtime behavior.")
    lines.append("")

    lines.append("## 2. Baseline vs Concurrent Design")
    lines.append("- Baseline (near single-thread): `scan_pool_num_threads=1`, `fd_pool_num_threads=1`, `max_concurrent_scan=1`")
    lines.append("- Concurrent profiles: current config and worker profiles `2/4/8/10`.")
    lines.append("")

    lines.append("## 3. Measured Metrics")
    lines.append("- Latency:")
    lines.append("  - scan scenarios: average and p95 from accepted `POST /scan` to terminal state `DONE`.")
    if cancel_rows:
        lines.append("  - `cancel_flow`: average and p95 from accepted `POST /cancel` to terminal state (`CANCELED` or `DROPPED`).")
    lines.append("- Throughput:")
    lines.append("  - scan scenarios: completed (`DONE`) scans per minute.")
    if cancel_rows:
        lines.append("  - `cancel_flow` scenario: terminal cancels (`CANCELED` + `DROPPED`) per minute.")

    lines.append("- Scan total expected (entries):")
    lines.append("  - Formula: `scan_total_expected_entries = done * scan_entries_per_root_expected`.")
    lines.append(
        "  - In this run, `scan_entries_per_root_expected={entries}` "
        "(`dirs_per_root={dirs}` + `files_per_root={files}`) from fake dataset config.".format(
            entries=entries_per_root, dirs=dirs_per_root, files=files_per_root
        )
    )
    if cancel_rows:
        lines.append("  - `cancel_flow` uses the same formula and therefore is usually `0` because `done=0` under cancel-target scenarios.")
    lines.append("- Backpressure: HTTP 429 count under overload.")
    lines.append("- Timeout(-1): client did not receive response (network/transport level).")
    lines.append("- Resource usage: process CPU (`avg`, `p95`) and RSS memory peak (MB).")
    lines.append("- Store mode (`store`):")
    lines.append("  - `store=true`: keep exported result/index files under each profile `exports/` directory.")
    lines.append("  - `store=false`: benchmark removes `exports/` directory after setup, so export files are not kept and DOES NOT export I/O at all.")
    lines.append("- Per-profile/raw metrics are available in `results.json` for deeper analysis.")
    lines.append("")

    lines.append("## 4. Scenarios")
    lines.append("1. `single_big_scan`: one large root scan.")
    lines.append("2. `burst_submit`: burst submit 20-100 style load.")
    lines.append("3. `overload_queue`: sustained high-rate submit until queues push back.")
    lines.append("4. `cancel_flow`: submit a batch of scans, snapshot only ids still in `PENDING/RUNNING`, wait `cancel_delay_sec`, then `POST /cancel` for that filtered set and wait for terminal convergence.")
    if cancel_rows:
        lines.append(f"- config in this run: `cancel_delay_sec={cancel_delay_text}`.")
        lines.append("- `cancel_delay_sec` is applied after the cancelable scans and before `POST /cancel`, so a larger delay usually increases late-cancel races and `409` responses.")
    lines.append("")

    lines.append("## 5. Directly Demonstrable Project Advantages")
    lines.append("- Multi-level bounded queues with 429 backpressure (no unbounded acceptance).")
    lines.append("- Scheduler enforces max concurrent scan ceiling; scan job queue enforces worker-level backpressure.")
    lines.append("- Graceful cancel: PENDING -> DROPPED, RUNNING -> CANCELED via shared context flag.")
    lines.append("- Drain/shutdown path converges cleanly under active load.")
    lines.append("- Realtime observability: `/metrics` exposes queue depths, running counts, enqueue-reject and request-failed deltas.")
    lines.append("")

    lines.append("## 6. Summary Table")
    lines.append("")
    lines.append(
        "| Profile | Scenario | Config | Latency Avg(s) | Latency P95(s) | Throughput (/min) | Expect_Scanned Size(entries) | Req_Total | Done | Reject (429) (cancel:409) | Timeout (-1) | CPU avg(%) | CPU p95(%) | RSS peak(MB) |"
    )
    lines.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")

    for r in rows:
        scenario = r.get("scenario", "")
        cfg = f"{r.get('scan_pool_num_threads','?')}/{r.get('fd_pool_num_threads','?')}/{r.get('max_concurrent_scan','?')}"
        avg = as_float(r.get("avg_latency_sec", 0), 0.0)
        p95 = as_float(r.get("p95_latency_sec", 0), 0.0)
        tp = as_float(r.get("throughput_per_min", r.get("throughput_scan_per_min", 0)), 0.0)
        req_total = as_int(r.get("requests_total", 0), 0)
        done = as_int(r.get("done", 0), 0)
        scan_total_expected = as_int(r.get("scan_total_expected_entries", 0), 0)
        s429 = as_int(r.get("status_429", 0), 0)
        s409 = as_int(r.get("status_409", 0), 0)
        reject_code = s409 if scenario == "cancel_flow" else s429
        stmo = as_int(r.get("status_timeout", 0), 0)
        cpu_avg = as_float(r.get("cpu_avg_percent", 0), 0.0)
        cpu_p95 = as_float(r.get("cpu_p95_percent", 0), 0.0)
        rss_peak = as_float(r.get("rss_peak_mb", 0), 0.0)

        lines.append(
            "| {profile} | {scenario} | {cfg} | {avg:.4f} | {p95:.4f} | {tp:.3f} | {scan_total_expected} | {req_total} | {done} | {reject_code} | {stmo} | {cpu_avg:.2f} | {cpu_p95:.2f} | {rss_peak:.2f} |".format(
                profile=r.get("profile", ""),
                scenario=scenario,
                cfg=cfg,
                avg=avg,
                p95=p95,
                tp=tp,
                scan_total_expected=scan_total_expected,
                req_total=req_total,
                done=done,
                reject_code=reject_code,
                stmo=stmo,
                cpu_avg=cpu_avg,
                cpu_p95=cpu_p95,
                rss_peak=rss_peak,
            )
        )

    lines.append("")
    lines.append("## Speedup vs Baseline (single_big_scan)")
    baseline = None
    for r in rows:
        if r.get("profile") == "baseline_single" and r.get("scenario") == "single_big_scan":
            baseline = as_float(r.get("elapsed_sec", 0), 0)
            break

    if baseline and baseline > 0:
        lines.append("| Profile | elapsed (s) | Speedup |")
        lines.append("|---|---:|---:|")
        for r in rows:
            if r.get("scenario") != "single_big_scan":
                continue
            elapsed = as_float(r.get("elapsed_sec", 0), 0)
            speedup = (baseline / elapsed) if elapsed > 0 else 0.0
            lines.append(f"| {r.get('profile','')} | {elapsed:.4f} | {speedup:.3f}x |")
    else:
        lines.append("Baseline single_big_scan row not found; speedup table skipped.")

    lines.append("")
    lines.append("## Notes")
    lines.append("- Full JSON: `results.json`.")
    lines.append("- Service stdout/stderr logs are stored per profile in this run directory.")

    return "\n".join(lines) + "\n"

