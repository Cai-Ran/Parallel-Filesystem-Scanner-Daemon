from typing import Dict, Optional, List
from datetime import datetime, timezone
import json
import time
import math

def now_ts() -> float:
    # Use monotonic clock for all elapsed/latency computations.
    return time.monotonic()

def utc_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_json(text: str) -> Dict:
    try:
        v = json.loads(text)
        return v if isinstance(v, dict) else {}
    except Exception:
        return {}

def parse_scan_id(body: str) -> Optional[int]:
    obj = parse_json(body)
    value = obj.get("id")
    try:
        if value is None:
            return None
        return int(value)
    except Exception:
        return None
    


def as_int(value, default=0) -> int:
    try:
        return int(value)
    except Exception:
        return default

def as_float(value, default=0.0) -> float:
    try:
        return float(value)
    except Exception:
        return default
    
def as_bool(value, default: bool) -> bool:

    if isinstance(value, bool):
        return value
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        text = value.strip().lower()
        if text in ("1", "true", "t", "yes", "y", "on"):
            return True
        if text in ("0", "false", "f", "no", "n", "off"):
            return False
        
    return default
    

def percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    rank = (len(ordered) - 1) * p
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return ordered[low]
    w = rank - low
    return ordered[low] * (1 - w) + ordered[high] * w

