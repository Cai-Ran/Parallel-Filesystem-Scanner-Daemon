import threading
from helpers import now_ts
from typing import Optional
import os

'''
ResourceSampler results:

{
"time": now_ts(), 
"cpu_percent":  process CPU usage %, 
"rss_bytes":    process memory usage (Resident Set Size bytes)
}

# d_process      = (utime1 + stime1) - (utime0 + stime0)    # process busy
# d_cpu_total    = total_cpu_jiffies1 - total_cpu_jiffies0  # cpu: busy + idle
# process CPU usage % = d_proc / d_total * 100 * N_cpu

'''

class ResourceSampler:
    # constructor
    def __init__(self, pid: int, sample_interval: float):
        self.pid = pid
        self.interval_sec = sample_interval
        self.collected_samples = []
        self._stop_sig = threading.Event()
        self._thread = None

    # core private function
    def _run(self):
        count = os.cpu_count() 
        cpu_count = count if (count) else 1

        prev_total = self._read_total_cpu_jiffies()
        prev_proc = self._read_proc_cpu_jiffies(self.pid)

        while (not self._stop_sig.is_set()):
            curr_total = self._read_total_cpu_jiffies()
            curr_proc = self._read_proc_cpu_jiffies(self.pid)
            rss_bytes = self._read_proc_rss_bytes(self.pid)

            cpu_percent = None

            if ((prev_total is not None) and (prev_proc is not None) and (curr_total is not None) and (curr_proc is not None)
                and (curr_total > prev_total) and (curr_proc >= prev_proc)):
                cpu_percent = ((curr_proc - prev_proc) / (curr_total - prev_total)) * 100.0 * cpu_count

            self.collected_samples.append(
                {"time": now_ts(), "cpu_percent": cpu_percent, "rss_bytes": rss_bytes}
            )

            # update prev
            prev_total = curr_total
            prev_proc = curr_proc
            self._stop_sig.wait(self.interval_sec)


    # d_process      = (utime1 + stime1) - (utime0 + stime0)    # process busy
    # d_cpu_total    = total_cpu_jiffies1 - total_cpu_jiffies0  # cpu: busy + idle
    # process CPU usage % = d_proc / d_total * 100 * N_cpu

    # d_cpu_total: read linux kernel /proc/stat file
    def _read_total_cpu_jiffies(self) -> Optional[int]:
        try:
            # open linux file
            with open("/proc/stat", "r", encoding="utf-8") as f:
                # cpu 84946 236 11306 5321631 421 0 1721 0 0 0      cpu accumulation usage in different state
                line = f.readline().strip()
            if not line.startswith("cpu "):

