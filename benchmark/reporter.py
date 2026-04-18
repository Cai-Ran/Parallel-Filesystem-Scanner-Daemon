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
    io_elapsed_sec = 0.0
    io_incomplete = 0
    io_started = 0
    io_finished = 1
    anomalies = {}
    anomaly_metric_hits = {}

    if samples:
        scan_running_values = []
        sample_times = []
        sample_active = []
        for s in samples:
            m = s["metrics"]
            scan_pending = _metric_u64_sanitized(m, "scan_pending", anomalies, anomaly_metric_hits)
            scan_jobs_queued = _metric_u64_sanitized(m, "scan_jobs_queued", anomalies, anomaly_metric_hits)
            request_jobs_queued = _metric_u64_sanitized(m, "request_jobs_queued", anomalies, anomaly_metric_hits)
            sr = _metric_u64_sanitized(m, "scan_running", anomalies, anomaly_metric_hits)
            export_pending = _metric_u64_sanitized(m, "export_pending", anomalies, anomaly_metric_hits)
            export_running = _metric_u64_sanitized(m, "export_running", anomalies, anomaly_metric_hits)
            export_finalizing_running = _metric_u64_sanitized(m, "export_finalizing_running", anomalies, anomaly_metric_hits)
            max_scan_pending = max(max_scan_pending, scan_pending)
            max_scan_jobs_queued = max(max_scan_jobs_queued, scan_jobs_queued)
            max_request_jobs_queued = max(max_request_jobs_queued, request_jobs_queued)
            scan_running_values.append(sr)
            max_scan_running = max(max_scan_running, sr)
            sample_times.append(as_float(s.get("time", 0.0), 0.0))
            sample_active.append(
                (export_pending > 0) or (export_running > 0) or (export_finalizing_running > 0)
            )
        avg_scan_running = float(statistics.mean(scan_running_values)) if scan_running_values else 0.0

        first_active_idx = -1
        for i, active in enumerate(sample_active):
            if active:
                first_active_idx = i
                break

        if first_active_idx != -1:
            io_started = 1
            io_finished = 0
            start_t = sample_times[first_active_idx]
            last_active_idx = first_active_idx
            for i in range(len(sample_active) - 1, first_active_idx - 1, -1):
                if sample_active[i]:
                    last_active_idx = i
                    break

            end_idx = -1
            for i in range(last_active_idx + 1, len(sample_active)):
                if not sample_active[i]:
                    end_idx = i
                    break

            if end_idx != -1:
                io_finished = 1
                io_elapsed_sec = max(sample_times[end_idx] - start_t, 0.0)
            else:
                io_incomplete = 1
                io_elapsed_sec = max(sample_times[-1] - start_t, 0.0)

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

    delta_export_finished = delta("export_finished")
    io_rows_finished = delta_export_finished
    io_throughput_rows_per_sec_K = 0.0
    if io_elapsed_sec > 0.0 and io_incomplete == 0:
        io_throughput_rows_per_sec_K = io_rows_finished / io_elapsed_sec / 1000

    return {
        "max_scan_pending": max_scan_pending,
        "max_scan_jobs_queued": max_scan_jobs_queued,
        "max_request_jobs_queued": max_request_jobs_queued,
        "max_scan_running": max_scan_running,
        "avg_scan_running": round(avg_scan_running, 3),
        "delta_scan_jobs_enqueue_reject": delta("scan_jobs_enqueue_reject"),
        "delta_request_jobs_failed": delta("request_jobs_failed"),
        "delta_export_finished": delta("export_finished"),
        "io_rows_finished": io_rows_finished,
        "io_throughput_rows_per_sec_K": round(io_throughput_rows_per_sec_K, 3),
        "io_elapsed_sec": round(io_elapsed_sec, 4),
        "io_started": io_started,
        "io_finished": io_finished,
        "io_incomplete": io_incomplete,
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


    entries_per_root = as_int(run_meta.get("scan_entries_per_root_expected", 0), 0)
    dirs_per_root = as_int(run_meta.get("scan_dirs_per_root_expected", 0), 0)
    files_per_root = as_int(run_meta.get("scan_files_per_root_expected", 0), 0)

    lines = []
    lines.append("# Benchmark Report")
    lines.append("")
    lines.append(f"- Generated at (UTC): {run_meta['generated_at_utc']}")
    lines.append(f"- Run directory: `{run_meta['run_dir']}`")
    lines.append(f"- Project root: `{run_meta['project_root']}`")
    lines.append("")

    lines.append("## 1. Benchmark Goals")
    lines.append("- Prove concurrency value: faster execution and higher throughput.")
    lines.append("- Prove system control under pressure: backpressure instead of crash.")
    lines.append("- Prove observability: `/metrics` can explain runtime behavior.")
    lines.append("")

    lines.append("## 2. Baseline vs Concurrent Design")
    lines.append("- Baseline (near single-thread):  `max_concurrent_scan=1`, `scan_pool_num_threads=1`, `fd_pool_num_threads=1`")
    lines.append("- Concurrent profiles: current config and worker profiles .")
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
    lines.append("  - IO window: measured from first exporter-active sample to first exporter-idle sample after activity.")
    lines.append("  - IO throughput formula: `io_throughput_rows_per_sec_K = (export_finished) / io_elapsed_sec` / 1000.")

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
    lines.append("- IO elapsed: `io_elapsed_sec` starts at first exporter-active sample and ends at first exporter-idle sample after activity.")
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
    lines.append("- Realtime observability: `/metrics` exposes queue depths, running counts, enqueue-reject and request-failed deltas.")
    
    lines.append("")

    def profile_sort_key(r: Dict):
        profile = str(r.get("profile", ""))
        if profile == "baseline_single":
            return (0, 0, profile)
        prefix = "concurrent_"
        if profile.startswith(prefix):
            suffix = profile[len(prefix):]
            if suffix.isdigit():
                return (1, as_int(suffix, 0), profile)
        if profile == "concurrent_current":
            return (2, 0, profile)
        return (3, 0, profile)

    def scenario_rows(scenario_name: str) -> List[Dict]:
        if scenario_name == "cancel_flow":
            filtered = [r for r in rows if r.get("scenario") == "cancel_flow" or r.get("scenario_type") == "cancel"]
        else:
            filtered = [r for r in rows if r.get("scenario") == scenario_name]
        return sorted(filtered, key=profile_sort_key)



    lines.append("## 6. Results")
    lines.append("")
    lines.append(
        "| Profile | Scenario | Config | Latency Avg(s) | Latency P95(s) | Throughput Scan (/min) | IO Elapsed(s) | IO Throughput (K entry/sec) | Expect_Scanned Size(entries) | Req_Total | Done | Reject (429) (cancel:409) | Timeout (-1) | CPU avg(%) | CPU p95(%) | RSS peak(MB) |"
    )
    lines.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")

    for r in rows:
        scenario = r.get("scenario", "")
        cfg = f"{r.get('max_concurrent_scan','?')}/{r.get('scan_pool_num_threads','?')}/{r.get('fd_pool_num_threads','?')}"
        avg = as_float(r.get("avg_latency_sec", 0), 0.0)
        p95 = as_float(r.get("p95_latency_sec", 0), 0.0)
        tp = as_float(r.get("throughput_per_min", r.get("throughput_scan_per_min", 0)), 0.0)
        io_elapsed = as_float(r.get("io_elapsed_sec", 0.0), 0.0)
        io_tp_text = "n/a"
        if "io_throughput_rows_per_sec_K" in r:
            io_tp_text = f"{as_float(r.get('io_throughput_rows_per_sec_K', 0.0), 0.0):.3f}"
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
            "| {profile} | {scenario} | {cfg} | {avg:.4f} | {p95:.4f} | {tp:.3f} | {io_elapsed:.4f} | {io_tp} | {scan_total_expected} | {req_total} | {done} | {reject_code} | {stmo} | {cpu_avg:.2f} | {cpu_p95:.2f} | {rss_peak:.2f} |".format(
                profile=r.get("profile", ""),
                scenario=scenario,
                cfg=cfg,
                avg=avg,
                p95=p95,
                tp=tp,
                io_elapsed=io_elapsed,
                io_tp=io_tp_text,
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

    lines.append("## 7. Key Metric Comparisons by Scenario")
    lines.append("")

    single_rows = scenario_rows("single_big_scan")
    baseline_latency = 0.0
    for r in single_rows:
        if r.get("profile") == "baseline_single":
            baseline_latency = as_float(r.get("avg_latency_sec", 0), 0.0)
            break

    lines.append("### single_big_scan — Speedup vs Baseline")
    lines.append("")
    lines.append("Scenario goal: measure scan-path parallelization efficiency under fixed work by comparing effective scan rate against the single-thread baseline.")
    lines.append("")
    lines.append("| Profile | latency (s) | Speedup |")
    lines.append("|---|---:|---:|")
    for r in single_rows:
        latency = as_float(r.get("avg_latency_sec", 0), 0.0)
        speedup_text = "n/a"
        if baseline_latency > 0 and latency > 0:
            speedup_text = f"{(baseline_latency / latency):.3f}x"
        lines.append(f"| {r.get('profile','')} | {latency:.4f} | {speedup_text} |")

    lines.append("")
    lines.append("")

    burst_rows = scenario_rows("burst_submit")
    burst_total = max([as_int(r.get("requests_total", 0), 0) for r in burst_rows], default=0)
    burst_total_suffix = f", {burst_total} requests" if burst_total > 0 else ""
    lines.append(f"#### burst_submit — Done Rate (Effective Capacity Under Burst{burst_total_suffix})")
    lines.append("")
    lines.append("Scenario goal: test effective capacity and protection behavior under sudden burst traffic (fixed-count concurrent submit).")
    lines.append("")
    lines.append("| Profile | Req Total | Reject 429 | Done | Done % | Throughput Scan (/min) | IO Elapsed(s) | IO Throughput (K entry/sec) | Latency Avg (s) | Latency P95 (s) | CPU avg (%) | RSS peak (MB) |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in burst_rows:
        req_total = as_int(r.get("requests_total", 0), 0)
        reject_429 = as_int(r.get("status_429", 0), 0)
        done = as_int(r.get("done", 0), 0)
        done_pct = (done * 100.0 / req_total) if req_total > 0 else 0.0
        tp = as_float(r.get("throughput_per_min", 0), 0.0)
        io_elapsed = as_float(r.get("io_elapsed_sec", 0.0), 0.0)
        io_tp_text = "n/a"
        if "io_throughput_rows_per_sec_K" in r:
            io_tp_text = f"{as_float(r.get('io_throughput_rows_per_sec_K', 0.0), 0.0):.3f}"
        avg = as_float(r.get("avg_latency_sec", 0), 0.0)
        p95 = as_float(r.get("p95_latency_sec", 0), 0.0)
        cpu_avg = as_float(r.get("cpu_avg_percent", 0), 0.0)
        rss_peak = as_float(r.get("rss_peak_mb", 0), 0.0)
        lines.append(
            f"| {r.get('profile','')} | {req_total} | {reject_429} | {done} | {done_pct:.1f}% | {tp:.3f} | {io_elapsed:.4f} | {io_tp_text} | {avg:.4f} | {p95:.4f} | {cpu_avg:.2f} | {rss_peak:.2f} |"
        )

    lines.append("")
    lines.append("")

    overload_rows = scenario_rows("overload_queue")
    lines.append("#### overload_queue — Throughput vs Resource Cost")
    lines.append("")
    lines.append("Scenario goal: test stability under sustained pressure (time-window (20s) continuous submit), including whether the system rejects excess load instead of stalling. Overlapped path requests will be rejected with 409.")
    lines.append("")
    lines.append("| Profile | Req Total | Done | Reject 429 | Reject 409 | Timeout (-1) | Throughput Scan (/min) | IO Elapsed(s) | IO Throughput (K entry/sec) | CPU avg (%) | CPU p95 (%) | RSS Peak (MB) |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in overload_rows:
        req_total = as_int(r.get("requests_total", 0), 0)
        done = as_int(r.get("done", 0), 0)
        reject_429 = as_int(r.get("status_429", 0), 0)
        reject_409 = as_int(r.get("status_409", 0), 0)
        timeout_count = as_int(r.get("status_timeout", 0), 0)
        tp = as_float(r.get("throughput_per_min", 0), 0.0)
        io_elapsed = as_float(r.get("io_elapsed_sec", 0.0), 0.0)
        io_tp_text = "n/a"
        if "io_throughput_rows_per_sec_K" in r:
            io_tp_text = f"{as_float(r.get('io_throughput_rows_per_sec_K', 0.0), 0.0):.3f}"
        cpu_avg = as_float(r.get("cpu_avg_percent", 0), 0.0)
        cpu_p95 = as_float(r.get("cpu_p95_percent", 0), 0.0)
        rss_peak = as_float(r.get("rss_peak_mb", 0), 0.0)
        lines.append(
            f"| {r.get('profile','')} | {req_total} | {done} | {reject_429} | {reject_409} | {timeout_count} | {tp:.3f} | {io_elapsed:.4f} | {io_tp_text} | {cpu_avg:.2f} | {cpu_p95:.2f} | {rss_peak:.2f} |"
        )

    lines.append("")
    lines.append("")
    lines.append("---")
    lines.append("")

    cancel_table_rows = scenario_rows("cancel_flow")
    lines.append("#### cancel_flow — Cancel Latency & Conflict Rate")
    lines.append("")
    lines.append("Scenario method: two-phase model. Phase 1 submits a batch of scans concurrently, then snapshots IDs still in `PENDING/RUNNING`; Phase 2 waits `cancel_delay_sec` and sends `POST /cancel` for that snapshot. This measures cancel latency and late-cancel conflict (`409`) when targets have already finished or left cancellable states before `POST /cancel`.")
    lines.append("")
    lines.append("| Profile | Req Total | 409 | DROPPED | CANCELED | DONE | Throughput (/min) | Latency Avg (ms) | Latency P95 (ms) |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in cancel_table_rows:
        req_total = as_int(r.get("requests_total", 0), 0)
        status_409 = as_int(r.get("status_409", 0), 0)
        dropped = as_int(r.get("dropped", 0), 0)
        canceled = as_int(r.get("canceled", 0), 0)
        done = as_int(r.get("done", 0), 0)
        tp = as_float(r.get("throughput_per_min", 0), 0.0)
        avg_ms = as_float(r.get("avg_latency_sec", 0), 0.0) * 1000.0
        p95_ms = as_float(r.get("p95_latency_sec", 0), 0.0) * 1000.0
        lines.append(
            f"| {r.get('profile','')} | {req_total} | {status_409} | {dropped} | {canceled} | {done} | {tp:.3f} | {avg_ms:.1f} | {p95_ms:.1f} |"
        )

    lines.append("")
    lines.append("## Notes")
    lines.append("- Full JSON: `results.json`.")
    lines.append("- Service stdout/stderr logs are stored per profile in this run directory.")

    return "\n".join(lines) + "\n"
