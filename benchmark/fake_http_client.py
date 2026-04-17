from typing import Tuple, Dict, List
from urllib import request, error, parse
from helpers import parse_json, parse_scan_id, now_ts, as_int
import json
import time
import threading

'''
A light weight fake http client.
1. send request with get/post http method 
2. return status code & body 
3. custom url of C++ daemon
'''


class HttpClient:
    FINAL_STATES = {"DONE", "CANCELED", "FAILED", "DROPPED"}

    # constructor
    def __init__(self, host: str, port: int, timeout_sec: float):     
        self.url_base = f"http://{host}:{port}"
        self.timeout = timeout_sec
        self._http_stat_lock = threading.Lock()
        self._http_stats = {
            "requests_total": 0,
            "accepted": 0,
            "status_429": 0,
            "status_400": 0,
            "status_timeout": 0,
            "status_other": 0,
        }

    def _record_http_status(self, status: int):
        with self._http_stat_lock:
            self._http_stats["requests_total"] += 1
            if status == 200:
                self._http_stats["accepted"] += 1
            elif status == 429:
                self._http_stats["status_429"] += 1
            elif status == 400:
                self._http_stats["status_400"] += 1
            elif status == -1:
                self._http_stats["status_timeout"] += 1
            else:
                self._http_stats["status_other"] += 1

    def reset_http_stats(self):
        with self._http_stat_lock:
            for key in self._http_stats.keys():
                self._http_stats[key] = 0

    def get_http_stats(self) -> Dict[str, int]:
        with self._http_stat_lock:
            return dict(self._http_stats)

    # private core function
    def _request(self, method: str, path: str) -> Tuple[int, str]:
        url = self.url_base + path
        req = request.Request(url=url, method=method)

        try:
            with request.urlopen(req, timeout=self.timeout) as resp:
                status = resp.status
                body_bytes = resp.read()
                body = body_bytes.decode("utf-8", errors="replace") # bytes to str
            # resp.close()
            self._record_http_status(int(status))
            return (status, body)
        except error.HTTPError as e:
            if e.fp:    # with body
                try:
                    body_bytes = e.read()
                    body = body_bytes.decode("utf-8", errors="replace")
                except Exception:
                    body = ""
            else:
                body = ""
            code = e.code
            self._record_http_status(int(code))
            return (code, body)
        except Exception:
            self._record_http_status(-1)
            return (-1, "")
        
    # parse response of server: [{"id":42,"state":"CANCELED"}]
    def _parse_state_map(self, body: str) -> Dict[int, str]:
        try:
            payload = json.loads(body)
        except Exception:
            return {}

        state_map = {}
        if isinstance(payload, dict):
            sid = payload.get("id")
            st = payload.get("state")
            try:
                if sid is not None and st is not None:
                    state_map[int(sid)] = str(st)
            except Exception:
                pass
            return state_map

        if isinstance(payload, list):
            for item in payload:
                if not isinstance(item, dict):
                    continue
                sid = item.get("id")
                st = item.get("state")
                if sid is None or st is None:
                    continue
                try:
                    state_map[int(sid)] = str(st)
                except Exception:
                    continue

        return state_map
    
    # public

    def get(self, path: str) -> Tuple[int, str]:
        return self._request("GET", path)
    
    def post(self, path: str) -> Tuple[int, str]:
        return self._request("POST", path)
    
    # custom url of daemon

    def get_metrics(self) -> Dict:
        (status, body) = self.get("/metrics")
        if (status != 200):
            return {}
        obj = parse_json(body)
        return obj if (obj) else {}
    
    def get_state(self, scan_id: int) -> str:
        (status, body) = self.get(f"/state?id={scan_id}")
        if (status != 200):
            return ""
        state_map = self._parse_state_map(body)
        return state_map.get(scan_id, "")

    def post_scan(self, root: str) -> Tuple[int, int, float]:
        encoded = parse.quote(str(root), safe="")
        path = "/scan?root=" + encoded
        t = now_ts()
        (status, body) = self.post(path)
        if (status != 200):     # invalid scan_id = 0
            return (status, 0, t)
        else:
            scan_id = parse_scan_id(body)
        return status, scan_id, t

    def post_cancel(self, scan_id: int) -> int:
        (status, _) = self.post(f"/cancel?id={scan_id}")
        t = now_ts()
        return status, t
    
    # wait metrics reset (happens in system reset)
    def wait_metrics_reset(self, wait_timeout: float, poll_interval_sec: float) -> bool:
        deadline = now_ts() + wait_timeout
        stable_hits = 0     # if detect stable 3 times -> determine stable

        while (now_ts() < deadline):

            metrics = self.get_metrics()
            if (not metrics):
                time.sleep(poll_interval_sec)
                continue

            scan_running =          as_int(metrics.get("scan_running", 0), 0)
            scan_pending =          as_int(metrics.get("scan_pending", 0), 0)
            scan_jobs_unfinished =  as_int(metrics.get("scan_jobs_unfinished", 0), 0)
            scan_jobs_queued =      as_int(metrics.get("scan_jobs_queued", 0), 0)
            request_jobs_queued =   as_int(metrics.get("request_jobs_queued", 0), 0)
            export_pending = as_int(metrics.get("export_pending", 0), 0)
            export_running = as_int(metrics.get("export_running", 0), 0)
            export_finalizing_running = as_int(metrics.get("export_finalizing_running", 0), 0)

            # check metrics is reset 
            idle = (
                    scan_running            == 0
                and scan_pending            == 0
                and scan_jobs_unfinished    == 0
                and scan_jobs_queued        == 0
                and request_jobs_queued     == 0
                and export_pending          == 0
                and export_running          == 0
                and export_finalizing_running == 0
            )

            if idle:
                stable_hits += 1
                if stable_hits >= 3:
                    return True
            else:
                stable_hits = 0

            time.sleep(poll_interval_sec)

        return False

    # wait exporter pipeline to become idle (result + delete)
    def wait_export_idle(self, wait_timeout: float, poll_interval_sec: float):
        deadline = now_ts() + wait_timeout
        stable_hits = 0

        while now_ts() < deadline:
            metrics = self.get_metrics()
            now = now_ts()
            if not metrics:
                time.sleep(poll_interval_sec)
                continue

            export_pending = as_int(metrics.get("export_pending", 0), 0)
            export_running = as_int(metrics.get("export_running", 0), 0)
            export_finalizing_running = as_int(metrics.get("export_finalizing_running", 0), 0)

            idle = (
                export_pending == 0 and export_running == 0 and export_finalizing_running == 0
            )

            if idle:
                stable_hits += 1
                if stable_hits >= 3:
                    return True, now
            else:
                stable_hits = 0

            time.sleep(poll_interval_sec)

        return False, now_ts()
    
    # wait server respond homepage
    def wait_server_ready(self, wait_timeout: float) -> bool:
        deadline = now_ts() + wait_timeout

        while (now_ts() < deadline):

            (status, _) = self.get("/")
            if status == 200:
                return True
            
            time.sleep(0.2)

        return False
    
    # polling get /state to compute scan_cycle_time
    # GET /state?id=1,2,3
    def wait_final_states(self, scan_ids: List[int], poll_interval_sec: float, wait_timeout: float):
        ids_set = set(scan_ids)
        states = {}
        scan_cycle_end_times = {}
        deadline = now_ts() + wait_timeout

        while (ids_set and (now_ts() < deadline)):
            parts = []
            for scan_id in sorted(ids_set):
                parts.append(str(scan_id))
            query_ids = ",".join(parts)

            (status, body) = self.get(f"/state?id={query_ids}")
            if (status != 200):
                time.sleep(poll_interval_sec)
                continue

            state_map = self._parse_state_map(body)
            now = now_ts()
            for scan_id in list(ids_set):
                state = state_map.get(scan_id, "")
                if state in self.FINAL_STATES:
                    states[scan_id] = state
                    scan_cycle_end_times[scan_id] = now
                    ids_set.remove(scan_id)

            time.sleep(poll_interval_sec)

        if ids_set:
            print(f"[DEBUG] wait_final_states timeout, unfinished={list(ids_set)}")
        else:
            print(f"[DEBUG] wait_final_states done")

        return states, scan_cycle_end_times, list(ids_set)

