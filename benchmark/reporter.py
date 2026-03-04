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

