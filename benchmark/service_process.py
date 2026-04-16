import subprocess
from typing import List
from pathlib import Path
from fake_http_client import HttpClient
import time

# =====================
# run C++ execute file
# =====================

class ServiceProcess:

    def __init__(self, stdout_path: Path, stderr_path: Path):
        self.proc = None
        self.stdout_f = stdout_path.open("w", encoding="utf-8")
        self.stderr_f = stderr_path.open("w", encoding="utf-8")


    def start_service(self, cmd: List[str], cwd: Path) -> subprocess.Popen:

        try:
            self.proc = subprocess.Popen(
                cmd, 
                cwd=str(cwd), 
                stdout= self.stdout_f, 
                stderr= self.stderr_f
            ) # open pipe

        except Exception:
            self.stdout_f.close()
            self.stderr_f.close()
            raise



    def stop_service(self, client: HttpClient, timeout_sec: float = 120):

        client.post("/shutdown")

        try:
            self.proc.wait(timeout=timeout_sec)

        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=10)

        finally:
            self.stdout_f.close()
            self.stderr_f.close()
            time.sleep(2)


    def kill_service(self):
        try:
            self.proc.kill()
            self.proc.wait(timeout=5)
        except Exception:
            pass
        finally:
            self.stdout_f.close()
            self.stderr_f.close()
            time.sleep(2)
