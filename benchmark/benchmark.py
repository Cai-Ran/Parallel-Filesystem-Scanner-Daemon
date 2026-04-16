from fake_http_client import HttpClient
from metrics_sampler import MetricsSampler
from resource_sampler import ResourceSampler
from scenarios import Scenarios
from reporter import build_report, summarize_metrics, summarize_resources
from helpers import as_int, as_float, utc_iso, now_ts
import subprocess
from typing import List, Dict
from pathlib import Path
from datetime import datetime
import json
import argparse
import configparser


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

    parser = argparse.ArgumentParser(description="Project benchmark runner for concurrency scanner")
    parser.add_argument("--project-root", default=".", help="Project root path (default: current dir)")
    parser.add_argument("--bench-config", default="benchmark/bench_config.json", help="Benchmark config path")
    parser.add_argument("--base-config", default="config.ini", help="Base service config path")
    parser.add_argument("--runs-dir", default="benchmark/runs", help="Benchmark run output directory")
    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()
    bench_cfg_path = (project_root / args.bench_config).resolve()
    base_cfg_path = (project_root / args.base_config).resolve()
    runs_dir = (project_root / args.runs_dir).resolve()
    runs_dir.mkdir(parents=True, exist_ok=True)

    if not bench_cfg_path.exists():
        raise FileNotFoundError(f"Benchmark config not found: {bench_cfg_path}")
    if not base_cfg_path.exists():
        raise FileNotFoundError(f"Base config not found: {base_cfg_path}")
    
    # load Config

    cfg = json.loads(bench_cfg_path.read_text(encoding="utf-8"))

    run_id = datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = runs_dir / run_id
    run_dir.mkdir(parents=True, exist_ok=True)

    dataset_cfg = cfg.get("dataset", {})
    dataset_model = estimate_entries_per_root(dataset_cfg)
    dataset_roots = make_fake_dataset(project_root, dataset_cfg)

    host = cfg.get("host", "127.0.0.1")
    base_port = as_int(cfg.get("base_port", 18080), 18080)
    poll_metrics = as_float(cfg.get("metrics_poll_interval_sec", 1.0), 1.0)
    poll_state = as_float(cfg.get("state_poll_interval_sec", 0.2), 0.2)
    cycle_timeout = as_float(cfg.get("cycle_timeout_sec", 180), 180.0)

    profiles = cfg.get("profiles", [])
    scenario_cfgs = cfg.get("scenarios", [])
    service_command_spec = cfg.get("service_command", ["./service", "{config}"])

    for s in scenario_cfgs:
        if s.get("type") in ("burst", "cancel"):
            req = as_int(s.get("requests", 0), 0)
            if len(dataset_roots) < req:
                print(f"[benchmark] WARNING: scenario '{s.get('name')}' "
                    f"requests={req} > root_count={len(dataset_roots)}, "f"same roots will repeat → root guard 409")

    result_rows = []

    for pidx, profile in enumerate(profiles):

        profile_name = profile.get("name", f"profile_{pidx}")
        profile_desc = profile.get("description", "")
        port = base_port + pidx

        profile_dir = run_dir / profile_name
        profile_dir.mkdir(parents=True, exist_ok=True)
        log_dir = str(profile_dir)

        generated_cfg = profile_dir / "config.generated.ini"
        create_formatted_cfg(base_cfg_path, generated_cfg, profile.get("overrides", {}), port, log_dir)
        effective = read_formatted_config(generated_cfg)

        cmd = build_service_command(service_command_spec, generated_cfg)
        stdout_path = profile_dir / "service.stdout.log"
        stderr_path = profile_dir / "service.stderr.log"

        # Delete DB from previous profile so every profile starts with a fresh DB
        db_ini = configparser.ConfigParser(interpolation=None)
        db_ini.optionxform = str
        db_ini.read(generated_cfg, encoding="utf-8")
        db_path_rel = db_ini.get("db", "db_path", fallback="")
        if db_path_rel:
            db_file = (project_root / db_path_rel).resolve()
            for suffix in ["", "-wal", "-shm"]:
                p = Path(str(db_file) + suffix)
                if p.exists():
                    p.unlink()
                    print(f"[benchmark] deleted {p}")

        print(f"[benchmark] start profile={profile_name} port={port}")
        proc = start_service(cmd, project_root, stdout_path, stderr_path)
        client = HttpClient(host=host, port=port, timeout_sec=3.0)

        ready = client.wait_server_ready(wait_timeout=20)

        if not ready:
            try:
                proc.kill()
            except Exception:
                pass
            proc.wait(timeout=5)
            row = {
                "profile": profile_name,
                "profile_desc": profile_desc,
                "scenario": "_service_start_",
                "error": "server_not_ready",
            }
            row.update(effective)
            result_rows.append(row)
            print(f"[benchmark] profile={profile_name} failed to start")
            continue


        for scenario_cfg in scenario_cfgs:

            sname = scenario_cfg.get("name", scenario_cfg.get("type", "scenario"))
            print(f"[benchmark] run profile={profile_name} scenario={sname}")

            not_timeout = client.wait_metrics_reset(wait_timeout=120, poll_interval_sec=0.2)
            if not not_timeout:
                print(f"[DEBUG] wait_metrics_reset timeout")
            else:
                print(f"[DEBUG] wait_metrics_reset passed")

            metrics_sampler = MetricsSampler(client, poll_metrics)
            resource_sampler = ResourceSampler(proc.pid, min(poll_metrics, 0.1))
            start_metrics = client.get_metrics()
            metrics_sampler.start()
            resource_sampler.start()

            scenario = Scenarios(client=client, roots=dataset_roots, poll_interval=poll_state, timeout_sec=cycle_timeout)
            scenario_result = scenario.run_scenario(scenario_cfg)
            export_idle_ok, _ = client.wait_export_idle(wait_timeout=cycle_timeout, poll_interval_sec=poll_state)
            if export_idle_ok:
                final_metrics = client.get_metrics()
                if final_metrics:
                    metrics_sampler.collected_samples.append({"time": now_ts(), "metrics": final_metrics})

            metrics_sampler.stop()
            resource_sampler.stop()
            end_metrics = client.get_metrics()
            metrics_summary = summarize_metrics(metrics_sampler.collected_samples, start_metrics, end_metrics)
            resource_summary = summarize_resources(resource_sampler.collected_samples)

            row = {
                "profile": profile_name,
                "profile_desc": profile_desc,
                "scenario": sname,
                "scenario_type": scenario_cfg.get("type", ""),
            }
            if scenario_cfg.get("type", "") == "cancel":
                row["cancel_delay_sec"] = scenario_cfg.get("cancel_delay_sec", 0.0002)
            row.update(effective)
            row.update(scenario_result)
            row["scan_requests_total"] = scenario_result.get("scan_requests_total", scenario_result.get("requests_total", 0))
            done_scan_count = as_int(scenario_result.get("done", 0), 0)
            row["done_scan_count"] = done_scan_count
            row["scan_entries_per_root_expected"] = dataset_model["entries_per_root"]
            row["scan_total_expected_entries"] = int(done_scan_count * dataset_model["entries_per_root"])
            row.update(metrics_summary)
            io_incomplete = as_int(row.get("io_incomplete", 0), 0)
            if io_incomplete == 1:
                print(f"[DEBUG] io window incomplete profile={profile_name} scenario={sname}")
            row.pop("io_incomplete", None)
            row.update(resource_summary)
            result_rows.append(row)

        stop_service(client, proc)
        print(f"[benchmark] done profile={profile_name}")

    results_json_path = run_dir / "results.json"
    report_md_path = run_dir / "report.md"

    results_json_path.write_text(json.dumps(result_rows, ensure_ascii=True, indent=2), encoding="utf-8")

    run_meta = {
        "generated_at_utc": utc_iso(),
        "run_dir": str(run_dir),
        "project_root": str(project_root),
        "scan_entries_per_root_expected": dataset_model["entries_per_root"],
        "scan_dirs_per_root_expected": dataset_model["dirs_per_root"],
        "scan_files_per_root_expected": dataset_model["files_per_root"],
    }

    report_md = build_report(run_meta, result_rows)
    report_md_path.write_text(report_md, encoding="utf-8")

    print(f"[benchmark] report: {report_md_path}")
    print(f"[benchmark] json: {results_json_path}")


if __name__ == "__main__":
    main()

