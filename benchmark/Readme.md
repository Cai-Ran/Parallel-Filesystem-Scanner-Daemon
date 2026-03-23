# Benchmark

## Goals
- Prove concurrency value: faster execution and higher throughput.
- Prove system control under pressure: backpressure instead of crash.
- Prove observability: `/metrics` can explain runtime behavior.


---

## Quick Start

```bash
make bench-full
```

This command builds `./service` first, then runs `benchmark/benchmark.py` with `benchmark/bench_config.json`.

Run artifacts are written to:

```text
benchmark/runs/<YYYYMMDD_HHMMSS>/
|- report.md
|- results.json
|- <profile>/config.generated.ini
|- <profile>/service.stdout.log
|- <profile>/service.stderr.log
`- <profile>/*.log
```

---

## Run Manually

```bash
python3 benchmark/benchmark.py \
  --project-root . \
  --bench-config benchmark/bench_config.json \
  --base-config config.ini \
  --runs-dir benchmark/runs
```

Key flags:
- `--bench-config`: benchmark profiles/scenarios definition
- `--runs-dir`: output root for benchmark runs

---

## Bench Config at a Glance

`benchmark/bench_config.json` mainly controls:
- `service_command`, `host`, `base_port`
- `dataset` (generated filesystem size/shape)
- `profiles` (thread/concurrency settings)
- `scenarios` (traffic patterns)
- `metrics_poll_interval_sec`, `cycle_timeout_sec`

---

## Baseline vs Concurrent Design

Each profile is described by three parameters: `max_concurrent_scan / scan_pool_num_threads / fd_pool_num_threads`.

- **Baseline** (`1/1/1`): near single-threaded — one scan worker, one fd worker, one scan allowed at a time. Used as the reference point for speedup calculation.
- **`concurrent_current`**: the production-style config being tested, with asymmetric thread/concurrency settings.
- **`concurrent_2/3/4/8`**: symmetric scaling profiles where all scan parameters are set to the same value (e.g. `concurrent_2` = `2/2/16`). Used to observe how throughput and resource usage scale with worker count.

---

## Scenarios

- `single_big_scan`: one large scan, used for baseline latency and speedup.
- `burst_submit`: burst submit 20-100 style load.
- `overload_queue`: sustained high-rate submit until queues push back.
- `cancel_flow`: submit a batch of scans, snapshot only ids still in `PENDING/RUNNING`, wait `cancel_delay_sec`, then `POST /cancel` for that filtered set and wait for terminal convergence.
  - `cancel_delay_sec` is applied after the cancelable scans and before `POST /cancel`, so a larger delay usually increases late-cancel races and `409` responses.

---

## Measured Metrics

- **Latency**:
  - scan scenarios: average and p95 from accepted `POST /scan` to terminal state `DONE`.
  - `cancel_flow`: average and p95 from accepted `POST /cancel` to terminal state (`CANCELED` or `DROPPED`).
- **Throughput**:
  - scan scenarios: completed (`DONE`) scans per minute.
  - `cancel_flow` scenario: terminal cancels (`CANCELED` + `DROPPED`) per minute.
- **Scan total expected (entries)**:
  - Formula: `scan_total_expected_entries = done * scan_entries_per_root_expected`.
  - `cancel_flow` uses the same formula and is usually `0` because `done=0` under cancel-target scenarios.
- **Backpressure**: HTTP 429 count under overload.
- **Timeout (-1)**: client did not receive response (network/transport level).
- **Resource usage**: process CPU (`avg`, `p95`) and RSS memory peak (MB).
- Per-profile/raw metrics are available in `results.json` for deeper analysis.

---

## Directly Demonstrable Project Advantages

- Multi-level bounded queues with 429 backpressure (no unbounded acceptance).
- Scheduler enforces max concurrent scan ceiling; scan job queue enforces worker-level backpressure.
- Graceful cancel: `PENDING` -> `DROPPED`, `RUNNING` -> `CANCELED` via shared context flag.
- Drain/shutdown path converges cleanly under active load.
- Realtime observability: `/metrics` exposes queue depths, running counts, enqueue-reject and request-failed deltas.

---

## Reading Results

- `report.md`: human-readable summary table
- `results.json`: full raw metrics per `(profile, scenario)`

Main signals:
- Latency (`avg`, `p95`)
- Throughput (`/min`)
- Backpressure/conflicts (`429`, `409`)
- CPU (`avg`, `p95`) and memory (`RSS peak`)

Speedup is derived from `single_big_scan`:

```text
speedup = baseline_elapsed_sec / profile_elapsed_sec
```
