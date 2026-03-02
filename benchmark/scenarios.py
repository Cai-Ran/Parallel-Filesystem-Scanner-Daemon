from fake_http_client import HttpClient
from helpers import as_int, as_float, now_ts, percentile
from typing import List, Dict
from concurrent.futures import ThreadPoolExecutor, wait, ALL_COMPLETED
import threading
import statistics
import random
import time

'''
Scenario results:

"requests_total":               # post request count
"accepted":                     # post responsed 200
"status_429":                   # post responsed 429
"status_400":                   # post responsed 400
"status_timeout":               # post network timeout / transport error (status = -1)
"status_other":                 # post responsed other code
"done":                         # scan final state
"canceled":                     # scan final state
"failed":                       # scan final state
"dropped":                      # scan final state
"state_timeout":                # accepted scans that did not reach terminal state before wait timeout
                                # latency: end_state time - post_scan time
"avg_latency_sec": round(statistics.mean(latencies), 4) if latencies else 0.0,   
"p95_latency_sec": round(percentile(latencies, 0.95), 4) if latencies else 0.0,
                                # total elapsed time in this scenario (from first post_scan to last scan_final_state)
"elapsed_sec": round(elapsed, 4),   
                                 # use elapsed time to compute throughput: scan done number / elapsed time sec * 60 (done_scan/min) 
"throughput_per_min": round((end_state / elapsed) * 60.0, 3),                    

'''




class Scenarios:

    # constructor
    def __init__(self, client: HttpClient, roots: List[str], poll_interval: float, timeout_sec: float):
        self.client = client
        self.roots = roots
        self.poll_interval = poll_interval
        self.timeout_sec = timeout_sec


    # private funciton                                                         # ids for post_cancel
    def _post_batch(self, request_count: int, num_thread: int, post_type: str, ids: list):

        num_thread = max(1, num_thread)     # avoid 0 worker error of ThreadPoolExecutor

        lock = threading.Lock()
        response_records = []

        def task(i: int):
            # cycle through roots for each POST scan request
            if (post_type == "scan"):
                if not self.roots:
                    raise ValueError("roots is empty")
                root = self.roots[i % len(self.roots)]
                status, sid, ts = self.client.post_scan(root)
                with lock:      # protect record: lock with mutex
                    response_records.append((status, sid, ts))
            # iterate through roots for each POST cancel request
            elif (post_type == "cancel"):
                status, ts = self.client.post_cancel(ids[i])
                with lock:
                    response_records.append((status, ids[i], ts))

        with ThreadPoolExecutor(max_workers=num_thread) as ex:
            futures = []
            for i in range(request_count):
                future = ex.submit(task, i)
                futures.append(future)
            # all task is scheduled
            wait(futures, return_when=ALL_COMPLETED)

        return response_records
    
    
    def _collect_result(self, start_time: float, request_count: int, response_records: list, post_type: str):

        post_map = {}
        status_429 = status_409 = status_400 = status_other = status_timeout = 0
        for (status, scan_id, ts) in response_records:
            if status == 200 and scan_id:
                post_map[scan_id] = ts   # accepted
            elif status == 429:
                status_429 += 1
            elif status == 409:
                status_409 += 1
            elif status == 400:
                status_400 += 1
            elif status == -1:
                status_timeout += 1
            else:
                status_other += 1

        states, end_times, scan_cycle_unclosed = self.client.wait_final_states(list(post_map.keys()), self.poll_interval, self.timeout_sec)
        finished = now_ts()

        done = canceled = failed = dropped = custom_timeout = 0
        latencies = []

        for sid, post_ts in post_map.items():
            st = states.get(sid)
            if st is None:
                custom_timeout += 1
                continue
            latency = end_times[sid] - post_ts

            if ((post_type == "scan" and st in {"DONE"}) or 
                (post_type == "cancel" and st in {"DROPPED", "CANCELED"})):
                latencies.append(latency)

            if st == "DONE":
                done += 1
            elif st == "CANCELED":
                canceled += 1
            elif st == "FAILED":
                failed += 1
            elif st == "DROPPED":
                dropped += 1

        elapsed = max(finished - start_time, 1e-9)

        end_state = 0
        if (post_type == "scan"):
            end_state = done
        elif (post_type == "cancel"):
            end_state = canceled + dropped

        return {
            "requests_total": request_count,
            "scan_requests_total": request_count,
            "accepted": len(post_map),
            "status_429": status_429,   
            "status_409": status_409,     
            "status_400": status_400,      
            "status_timeout": status_timeout,

