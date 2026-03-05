from fake_http_client import HttpClient
from metrics_sampler import MetricsSampler
from resource_sampler import ResourceSampler
from scenarios import Scenarios
from reporter import build_report, summarize_metrics, summarize_resources
from helpers import as_int, as_float, as_bool, utc_iso
import subprocess
from typing import List, Dict
from pathlib import Path
from datetime import datetime
import json
import argparse
import configparser
import time
import shutil


# ==================
# build fake dataset
# ==================

# made by BFS
# only root & leaf with file

def make_fake_dataset(project_root: Path, cfg: Dict) -> List[str]:
    root_dir = project_root.joinpath(cfg.get("root_dir", "benchmark/data"))
    root_dir.mkdir(parents=True, exist_ok=True)

    root_count = as_int(cfg.get("root_count", 8), 8)
    depth = as_int(cfg.get("depth", 3), 3)
    dirs_per_level = as_int(cfg.get("dirs_per_level", 3), 3)
    files_per_dir = as_int(cfg.get("files_per_dir", 20), 20)
    file_size = as_int(cfg.get("file_size_bytes", 2048), 2048)
    content = ("X" * file_size)


    roots = []
    # create root nodes by cfg
    for i in range(root_count):
        root = root_dir.joinpath(f"root_{i:02d}")
        root.mkdir(parents=True, exist_ok=True)
        roots.append(root)
        # for each root node, create folder tree by BFS
        level = [root]
        for j in range(depth):
            next_level = []
            for parent in level:
                for k in range(dirs_per_level):
                    child = parent.joinpath(f"folder_{j:02d}_{k:02d}")
                    child.mkdir(parents=True, exist_ok=True)
                    next_level.append(child)
            # depth + 1
            level = next_level

        # create file only at root & leaf
        folders = [root] + level
        for node in folders:
            for i in range(files_per_dir):
                file = node.joinpath(f"file_{i:02d}.txt")
                if not file.exists():
                    file.write_text(content, encoding="utf-8")

    return roots


def estimate_entries_per_root(cfg: Dict) -> Dict[str, int]:
    depth = max(0, as_int(cfg.get("depth", 3), 3))
    dirs_per_level = max(0, as_int(cfg.get("dirs_per_level", 3), 3))
    files_per_dir = max(0, as_int(cfg.get("files_per_dir", 20), 20))

    # Geometric sum for generated directories: 1 + b + ... + b^depth
    dirs_per_root = 0
    level_nodes = 1
    for _ in range(depth + 1):
        dirs_per_root += level_nodes
        level_nodes *= dirs_per_level

    leaf_dirs = dirs_per_level ** depth
    files_per_root = (1 + leaf_dirs) * files_per_dir      # files only at root + leaf level
    entries_per_root = dirs_per_root + files_per_root      # scanner records one entry per dir/file node

    return {
        "dirs_per_root": int(dirs_per_root),
        "files_per_root": int(files_per_root),
        "entries_per_root": int(entries_per_root),
    }



# =====================
# run C++ execute file
# =====================

def start_service(cmd: List[str], cwd: Path, stdout_path: Path, stderr_path: Path) -> subprocess.Popen:
    stdout_f = stdout_path.open("w", encoding="utf-8")
    stderr_f = stderr_path.open("w", encoding="utf-8")

    try:
        proc = subprocess.Popen(
            cmd, 
            cwd=str(cwd), 
            stdout=stdout_f, 
            stderr=stderr_f
        ) # open pipe

    except Exception:
        stdout_f.close()
        stderr_f.close()
        raise

    # add attribute
    proc._benchmark_stdout_f = stdout_f
    proc._benchmark_stderr_f = stderr_f

    return proc


def stop_service(client: HttpClient, proc: subprocess.Popen, timeout_sec: float = 120):

    client.post("/shutdown")

    try:
        proc.wait(timeout=timeout_sec)

    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)

    finally:
        if hasattr(proc, "_benchmark_stdout_f"):
            proc._benchmark_stdout_f.close()
        if hasattr(proc, "_benchmark_stderr_f"):
            proc._benchmark_stderr_f.close()


# ==========
# CONFIG
# ==========


# convert command (string/list) to list for subprocess.Popen
def build_service_command(cmd_spec, config_path: Path) -> List[str]:

    if isinstance(cmd_spec, str):
        parts = cmd_spec.strip().split()
    elif isinstance(cmd_spec, list):
        parts = [str(x) for x in cmd_spec]
    else:
        raise ValueError("service_command must be a string or list")

    cmd = []
    for item in parts:
        if "{config}" in item:
            cmd.append(item.replace("{config}", str(config_path)))
        else:
            cmd.append(item)
    if all("{config}" not in x for x in parts):
        cmd.append(str(config_path))

    return cmd


# create formatted config.ini for C++ execution file:
# override httpserver port to run multiple service
# override concurrency related parameter to run different tests with specified config.json 
def create_formatted_cfg(base_cfg_path: Path, out_cfg_path: Path, overrides: Dict[str, str], port: int, log_dir: str):

    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    parser.read(base_cfg_path, encoding="utf-8")

    # override httpserver port
    if "httpserver" not in parser:
        parser["httpserver"] = {}
    parser["httpserver"]["server_port"] = str(port)
    if "asynclogger" not in parser:
        parser["asynclogger"] = {}
    parser["asynclogger"]["log_dir"] = log_dir

    for key, value in overrides.items():
        parts = key.split(".", 1)
        if len(parts) != 2:
            continue
        section, option = parts[0], parts[1]
        if section not in parser:
            parser[section] = {}
        parser[section][option] = str(value)

    with out_cfg_path.open("w", encoding="utf-8") as f:
        parser.write(f)


def read_formatted_config(cfg_path: Path) -> Dict[str, str]:

    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    parser.read(cfg_path, encoding="utf-8")

    def get(section: str, option: str, default: str) -> str:
        if section in parser and option in parser[section]:
            return parser[section][option]
        return default

    return {
        "server_port": get("httpserver", "server_port", ""),
        "fd_pool_num_threads": get("httpserver", "fd_pool_num_threads", ""),
        "fd_queue_max_size": get("httpserver", "fd_queue_max_size", ""),
        "max_concurrent_scan": get("scheduler", "max_concurrent_scan", ""),
        "pending_queue_max_size": get("scheduler", "queue_max_size", get("scheduler", "pending_queue_max_size", "")),
        "scan_pool_num_threads": get("manager", "scan_pool_num_threads", ""),
        "scan_queue_max_size": get("manager", "scan_queue_max_size", ""),
    }






def main():


