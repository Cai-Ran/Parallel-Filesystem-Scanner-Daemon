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
            "status_other": status_other,   
            "done": done,                   
            "canceled": canceled,          
            "failed": failed,              
            "dropped": dropped,             
            "custom_timeout": custom_timeout + len(scan_cycle_unclosed),    
            "avg_latency_sec": round(statistics.mean(latencies), 4) if latencies else 0.0,  
            "p95_latency_sec": round(percentile(latencies, 0.95), 4) if latencies else 0.0,
            "elapsed_sec": round(elapsed, 4),
            "throughput_per_min": round((end_state / elapsed) * 60.0, 3),
        }
           

    def scenario_single(self, cfg: Dict) -> Dict:

        # pick one root in cfg roots
        ridx = as_int(cfg.get("root_index", 0), 0)
        root = self.roots[ridx % len(self.roots)]

        started = now_ts()
        status, scan_id, submit_ts = self.client.post_scan(root)

        return self._collect_result(started, 1, [(status, scan_id, submit_ts)], "scan")


    def scenario_burst(self, cfg: Dict) -> Dict:
        request_count = as_int(cfg.get("requests", 40), 40)
        workers = as_int(cfg.get("submit_workers", 8), 8)

        started = now_ts()
        response_records = self._post_batch(request_count, workers, "scan", [])

        return self._collect_result(started, request_count, response_records, "scan")


    # scenario_overload is time-window based model
    # while _post_batch is fixed-count based model
    def scenario_overload(self, cfg: Dict) -> Dict:
        test_duration = as_float(cfg.get("duration_sec", 20), 20.0)
        workers = as_int(cfg.get("submit_workers", 20), 20)                
        workers = max(1, workers)     # avoid 0 worker error of ThreadPoolExecutor
        send_interval = as_float(cfg.get("sleep_between_submit_sec", 0.0), 0.0)

        started = now_ts()
        stop_ts = started + test_duration
        lock = threading.Lock()
        response_records = []

        def worker(seed: int):
            rnd = random.Random(seed)
            while (now_ts() < stop_ts):
                root = self.roots[rnd.randrange(0, len(self.roots))]    # randomly pick one root
                status, sid, ts = self.client.post_scan(root)
                with lock:      # protect response_records with lock  mutex
                    response_records.append((status, sid, ts))
                if send_interval > 0:
                    time.sleep(send_interval)

        threads = []
        for i in range(workers):
            thread = threading.Thread(target=worker, args=(i + 17,), daemon=True)
            thread.start()
            threads.append(thread)
        for thread in threads:
            thread.join()

        # in this scenario, sent requeust as many as possible in duration, so request_count = len(response_records)
        return self._collect_result(started, len(response_records), response_records, "scan")
    

    def scenario_cancel(self, cfg: Dict) -> Dict:
        count = as_int(cfg.get("requests", 30), 30)
        workers = as_int(cfg.get("submit_workers", 8), 8)
        cancel_ratio = min(max(as_float(cfg.get("cancel_ratio", 1.0), 1.0), 0.0), 1.0)
        cancel_delay = as_float(cfg.get("cancel_delay_sec", 0.0002), 0.0002)

        # 1. post scan
        started = now_ts()
        records = self._post_batch(count, workers, "scan", [])

        submit_map = {}
        scan_submit_status_429 = 0
        scan_submit_status_400 = 0
        scan_submit_status_timeout = 0
        scan_submit_status_other = 0
        # First classify submit responses (including 429 / -1), then evaluate final states.
        for status, sid, ts in records:
            if status == 200 and sid:
                submit_map[sid] = ts
            elif status == 429:
                scan_submit_status_429 += 1
            elif status == 400:
                scan_submit_status_400 += 1
            elif status == -1:
                scan_submit_status_timeout += 1
            else:
                scan_submit_status_other += 1

        # 2. collect non-finished scan_ids
        valid_cancel_ids = []
        for sid in sorted(submit_map.keys()):
            st = self.client.get_state(sid)
            if st in {"PENDING", "RUNNING"}:
                valid_cancel_ids.append(sid)

        # simulate user operation
        if cancel_delay > 0:
            time.sleep(cancel_delay)

        # Cancel only for runnable scans. Non PENDING/RUNNING scans are skipped.
        valid_cancel_req = int(len(valid_cancel_ids) * cancel_ratio)
        cancel_ids = valid_cancel_ids[:valid_cancel_req]

        # 3. post cancel for non-finished scan_ids
        response_records = self._post_batch(len(cancel_ids), workers, "cancel", cancel_ids)

        result = self._collect_result(started, len(cancel_ids), response_records, "cancel")
        # Keep pre-cancel POST /scan submit stats for expected scan-workload analysis.
        result["scan_requests_total"] = count
        result["scan_submit_requests_total"] = count
        result["scan_submit_accepted"] = len(submit_map)
        result["scan_submit_status_429"] = scan_submit_status_429
        result["scan_submit_status_400"] = scan_submit_status_400
        result["scan_submit_status_timeout"] = scan_submit_status_timeout
        result["scan_submit_status_other"] = scan_submit_status_other
        return result


    def run_scenario(self, scenario: Dict):
        stype = scenario.get("type", "")
        if stype == "single":
            return self.scenario_single(scenario)
        if stype == "burst":
            return self.scenario_burst(scenario)
        if stype == "overload":
            return self.scenario_overload(scenario)
        if stype == "cancel":
            return self.scenario_cancel(scenario)

        raise ValueError(f"Unsupported scenario type: {stype}")
