#!/usr/bin/env python3

import os
import re
import signal
import subprocess
import sys
from itertools import product
from pathlib import Path
from time import gmtime, strftime


REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
BINARY = BUILD_DIR / "deft_cxl"
RESULT_DIR = REPO_ROOT / "result"
LOG_DIR = REPO_ROOT / "log"


# ============================================================
# CXL placement
# ============================================================
# Recommended for current CXL emulation:
#   CPU threads on NUMA 0
#   data memory on NUMA 1
#   lock memory on NUMA 0
#
# If you want a layout closer to "both lock and data are remote"
# like the original RDMA client view, change LOCK_NUMA to 1.
CPU_NUMA = 0
DATA_NUMA = 1
LOCK_NUMA = 0


# ============================================================
# Fixed benchmark parameters
# ============================================================
DSM_SIZE_GB = 62
OPS_PER_THREAD = 1_000_000
#
# IMPORTANT:
# Keep this as None if you want a "1 thread" run to really use one thread
# end-to-end. The previous fixed value 30 caused the prefill phase to still
# launch 30 threads even when num_bench_threads=1.
NUM_PREFILL_THREADS = None
JOB_TIMEOUT_SEC = 0


# ============================================================
# Sweep variables
# Keep these aligned with script/run_bench.py
# ============================================================
THREADS_CN_ARR = [1, 2, 4, 8, 16, 24, 32]
KEY_SPACE_ARR = [400e6]
READ_RATIO_ARR = [50]
ZIPF_ARR = [0.99]

# threads_CN_arr = [1, 2, 4, 8, 12, 16, 24, 32]
# key_space_arr = [100e5, 500e5, 100e6, 400e6, 800e6, 1200e6]
# read_ratio_arr = [100, 80, 60, 40, 20, 0]
# zipf_arr = [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.99]

USE_HUGEPAGE = False


def get_res_name(prefix: str) -> Path:
    postfix = sys.argv[1] if len(sys.argv) > 1 else ""
    name = f"{prefix}-{postfix}{strftime('-%m-%d-%H-%M', gmtime())}.txt"
    return RESULT_DIR / name


def ensure_dirs() -> None:
    RESULT_DIR.mkdir(exist_ok=True)
    LOG_DIR.mkdir(exist_ok=True)


def ensure_binary() -> None:
    if not BUILD_DIR.exists():
        raise SystemExit(
            f"build dir not found: {BUILD_DIR}\n"
            "Run `mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release ..` first."
        )

    subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "--target", "deft_cxl", "-j"],
        check=True,
        cwd=REPO_ROOT,
    )

    if not BINARY.exists():
        raise SystemExit(f"deft_cxl binary not found after build: {BINARY}")


def extract_float(pattern: str, text: str):
    m = re.search(pattern, text, re.MULTILINE)
    return m.group(1) if m else ""


def parse_metrics(output: str):
    return {
        "loading_tp_mops": extract_float(r"Loading Results: TP ([0-9.]+) Mops/s", output),
        "loading_lat_us": extract_float(r"Loading Results: TP [0-9.]+ Mops/s, Lat ([0-9.]+) us", output),
        "benchmark_tp_mops": extract_float(r"Benchmark TP: ([0-9.]+) Mops/s", output),
        "avg_op_ns": extract_float(r"Avg op latency: ([0-9.]+) ns", output),
        "cache_hit_rate": extract_float(r"Cache hit rate: ([0-9.]+)%", output),
        "avg_lock_ns": extract_float(r"Avg lock latency: ([0-9.]+) ns", output),
        "avg_read_page_ns": extract_float(r"Avg read page latency: ([0-9.]+) ns", output),
        "avg_write_page_ns": extract_float(r"Avg write page latency: ([0-9.]+) ns", output),
        "final_tp_mops": extract_float(r"Final Results: TP ([0-9.]+) Mops/s", output),
        "final_lat_us": extract_float(r"Final Results: TP [0-9.]+ Mops/s, Lat ([0-9.]+) us", output),
    }


def run_and_stream(cmd, cwd: Path, log_path: Path, env=None):
    collected = []
    with log_path.open("w") as log_fp:
        proc = subprocess.Popen(
            cmd,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            universal_newlines=True,
        )
        try:
            assert proc.stdout is not None
            for line in proc.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                log_fp.write(line)
                log_fp.flush()
                collected.append(line)
            returncode = proc.wait(timeout=JOB_TIMEOUT_SEC if JOB_TIMEOUT_SEC > 0 else None)
        except subprocess.TimeoutExpired:
            proc.kill()
            tail = f"\n[run_bench_cxl] timeout after {JOB_TIMEOUT_SEC} seconds\n"
            sys.stdout.write(tail)
            sys.stdout.flush()
            log_fp.write(tail)
            log_fp.flush()
            collected.append(tail)
            returncode = -9
        return returncode, "".join(collected)


def describe_returncode(returncode: int) -> str:
    if returncode >= 0:
        return f"exit({returncode})"
    sig = -returncode
    try:
        return f"signal({sig}:{signal.Signals(sig).name})"
    except Exception:
        return f"signal({sig})"


def build_command(num_threads: int, key_space: int, read_ratio: int, zipf: float):
    num_prefill_threads = (
        num_threads if NUM_PREFILL_THREADS is None else NUM_PREFILL_THREADS
    )
    return [
        "numactl",
        f"--cpunodebind={CPU_NUMA}",
        f"--membind={CPU_NUMA}",
        str(BINARY),
        f"--dsm_size={DSM_SIZE_GB}",
        f"--data_numa={DATA_NUMA}",
        f"--lock_numa={LOCK_NUMA}",
        f"--num_prefill_threads={num_prefill_threads}",
        f"--num_bench_threads={num_threads}",
        f"--ops_per_thread={OPS_PER_THREAD}",
        f"--key_space={key_space}",
        f"--read_ratio={read_ratio}",
        f"--zipf={zipf}",
    ]


def main() -> None:
    ensure_dirs()
    ensure_binary()

    result_path = get_res_name("bench_cxl")
    product_list = list(product(KEY_SPACE_ARR, READ_RATIO_ARR, ZIPF_ARR, THREADS_CN_ARR))

    print(THREADS_CN_ARR)
    print(KEY_SPACE_ARR)
    print(READ_RATIO_ARR)
    print(ZIPF_ARR)
    print(f"binary: {BINARY}")
    print(f"summary: {result_path}")

    with result_path.open("w") as fp:
        fp.write(
            "job_id\tcpu_numa\tdata_numa\tlock_numa\tdsm_size_gb\tops_per_thread\t"
            "use_hugepage\t"
            "num_prefill_threads\tnum_bench_threads\tkey_space\tread_ratio\tzipf\t"
            "loading_tp_mops\tloading_lat_us\tbenchmark_tp_mops\tavg_op_ns\t"
            "cache_hit_rate\tavg_lock_ns\tavg_read_page_ns\tavg_write_page_ns\t"
            "final_tp_mops\tfinal_lat_us\tstatus\tlog_file\n"
        )
        fp.flush()

        for job_id, (key_space, read_ratio, zipf, num_threads) in enumerate(product_list):
            key_space = int(key_space)
            num_prefill_threads = (
                num_threads if NUM_PREFILL_THREADS is None else NUM_PREFILL_THREADS
            )
            log_path = LOG_DIR / (
                f"cxl_job{job_id}_th{num_threads}_ks{key_space}_rr{read_ratio}_zipf{zipf}.log"
            )

            cmd = build_command(num_threads, key_space, read_ratio, zipf)
            env = os.environ.copy()
            if USE_HUGEPAGE:
                env["DEFT_CXL_USE_HUGEPAGE"] = "1"
            else:
                env.pop("DEFT_CXL_USE_HUGEPAGE", None)
            print(
                f"start job {job_id}: threads={num_threads} prefill_threads={num_prefill_threads} "
                f"key_space={key_space} read_ratio={read_ratio} zipf={zipf}"
            )

            returncode, output = run_and_stream(cmd, BUILD_DIR, log_path, env=env)

            metrics = parse_metrics(output)
            status = "ok" if returncode == 0 else f"fail({returncode})"

            fp.write(
                f"{job_id}\t{CPU_NUMA}\t{DATA_NUMA}\t{LOCK_NUMA}\t{DSM_SIZE_GB}\t{OPS_PER_THREAD}\t{int(USE_HUGEPAGE)}\t"
                f"{num_prefill_threads}\t{num_threads}\t{key_space}\t{read_ratio}\t{zipf}\t"
                f"{metrics['loading_tp_mops']}\t{metrics['loading_lat_us']}\t"
                f"{metrics['benchmark_tp_mops']}\t{metrics['avg_op_ns']}\t"
                f"{metrics['cache_hit_rate']}\t{metrics['avg_lock_ns']}\t"
                f"{metrics['avg_read_page_ns']}\t{metrics['avg_write_page_ns']}\t"
                f"{metrics['final_tp_mops']}\t{metrics['final_lat_us']}\t"
                f"{status}\t{log_path}\n"
            )
            fp.flush()

            if returncode != 0:
                tail = "\n".join(output.splitlines()[-40:])
                print(f"job {job_id} failed with {describe_returncode(returncode)}")
                if tail:
                    print("----- log tail -----")
                    print(tail)
                    print("--------------------")
                raise SystemExit(f"job {job_id} failed, see {log_path}")

            final_line = re.search(r"Final Results:.*", output)
            if final_line:
                print(final_line.group(0))


if __name__ == "__main__":
    main()
