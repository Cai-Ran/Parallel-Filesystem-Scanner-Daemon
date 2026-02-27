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

