from fake_http_client import HttpClient
import threading
from helpers import now_ts

'''
MetricsSampler:
for fake http client to sample Metrics every x seconds 
start -> background thread -> every x interval -> http request get metrics -> collect
'''

class MetricsSampler:
    # constructor
    def __init__(self, client: HttpClient, sample_interval: float):
        self.client = client
        self.interval_sec = sample_interval
        self.collected_samples = []
        self._stop_sig = threading.Event()
        self._thread = None
    
    # core private function
    def _run(self):
        while (not self._stop_sig.is_set()):
            metrics = self.client.get_metrics()
            if (metrics):
                self.collected_samples.append({"time":now_ts(), "metrics": metrics})
            self._stop_sig.wait(self.interval_sec)  # wait for stop signal if no wait for interval_sec

    # public

    def start(self):
        # reset
        self.collected_samples = []     
        self._stop_sig.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)      # worker function = _run(); daemom: detach and kill
        self._thread.start()

    def stop(self):
        self._stop_sig.set()
        if (self._thread):
            self._thread.join(timeout=2)    # return after timeout






