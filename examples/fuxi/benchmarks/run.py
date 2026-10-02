#!/usr/bin/env python3
"""Run a Fuxi benchmark, validate its results, and report workload counters."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

from pmu import GROUPS, parse_pmu


def validate(text, name, iterations):
    if name == "dhrystone":
        expected = {
            "Int_Glob": "5", "Bool_Glob": "1", "Ch_1_Glob": "A",
            "Ch_2_Glob": "B", "Arr_1_Glob[8]": "7",
            "Arr_2_Glob[8][7]": str(iterations + 10),
            "Int_1_Loc": "5", "Int_2_Loc": "13", "Int_3_Loc": "7",
            "Enum_Loc": "1", "Str_1_Loc": "DHRYSTONE PROGRAM, 1'ST STRING",
            "Str_2_Loc": "DHRYSTONE PROGRAM, 2'ND STRING",
        }
        for key, value in expected.items():
            if not re.search(rf"^{re.escape(key)}:\s*{re.escape(value)}\s*$", text, re.M):
                raise ValueError(f"Dhrystone result mismatch: {key} expected {value}")
    elif name == "coremark":
        for key, value in (("seedcrc", "e9f5"), ("[0]crclist", "e714"),
                           ("[0]crcmatrix", "1fd7"), ("[0]crcstate", "8e3a")):
            if not re.search(rf"^{re.escape(key)}\s*:\s*0x{value}\s*$", text, re.M):
                raise ValueError(f"CoreMark CRC mismatch: {key} expected {value}")
        for line in text.splitlines():
            if "ERROR" in line and "Must execute for at least 10 secs" not in line:
                raise ValueError(f"CoreMark validation failed: {line}")
    else:
        raise ValueError(f"Unknown benchmark {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-cycles", type=int, default=2000000000)
    parser.add_argument("--stall-probability", type=float, default=0)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--pmu-group", type=int, choices=GROUPS,
                        help="Require a complete frozen snapshot of this PMU group")
    args = parser.parse_args()
    command = [str(args.simulator.resolve()), "--elf", str(args.elf.resolve()),
               "--max-cycles", str(args.max_cycles), "--stall-probability",
               str(args.stall_probability), "--seed", str(args.seed), "--stats"]
    started = time.monotonic()
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    elapsed = time.monotonic() - started
    args.output.parent.mkdir(parents=True, exist_ok=True)
    log = args.output.with_suffix(".log")
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Simulator failed with {result.returncode}; see {log}")
    matches = re.findall(r"BENCH_RESULT name=(\w+) iterations=(\d+) "
                         r"cycles=([0-9a-f]{16}) instret=([0-9a-f]{16})", result.stdout)
    if len(matches) != 1:
        raise ValueError(f"Expected one benchmark result, found {len(matches)}; see {log}")
    name, iterations, cycles, instret = matches[0]
    iterations, cycles, instret = int(iterations), int(cycles, 16), int(instret, 16)
    if cycles <= 0 or instret <= 0 or instret > cycles:
        raise ValueError(f"Invalid single-issue counter result: cycles={cycles}, instret={instret}")
    validate(result.stdout, name, iterations)
    report = {
        "benchmark": name, "iterations": iterations, "cycles": cycles, "instret": instret,
        "ipc": instret / cycles, "cycles_per_iteration": cycles / iterations,
        "iterations_per_second_at_50mhz": iterations * 50000000 / cycles,
        "simulated_seconds_at_50mhz": cycles / 50000000,
        "host_wall_seconds": elapsed, "validation": "passed",
        "command": command, "elf_sha256": hashlib.sha256(args.elf.read_bytes()).hexdigest(),
        "simulator_sha256": hashlib.sha256(args.simulator.read_bytes()).hexdigest(),
    }
    pmu = parse_pmu(result.stdout, cycles, instret, args.pmu_group)
    if pmu is not None:
        report["pmu"] = pmu
    report["full_run_axi_reads"] = [
        {"port": int(port), "requests": int(requests),
         "bytes_requested": int(size), "beats_received": int(beats)}
        for port, requests, size, beats in re.findall(
            r"AXI_STATS port=(\d+) read_requests=(\d+) read_bytes_requested=(\d+) "
            r"read_beats_received=(\d+)", result.stderr)
    ]
    if len(report["full_run_axi_reads"]) != 3:
        raise ValueError("Expected full-run AXI read statistics for all three Fuxi ports")
    if name == "dhrystone":
        report["dmips_per_mhz"] = iterations * 1000000 / (cycles * 1757)
        report["score_note"] = "Short simulated run; normalized Dhrystone 2.1 estimate"
    else:
        report["coremark_per_mhz"] = iterations * 1000000 / cycles
        report["score_note"] = "Normalized simulation estimate; not an official CoreMark score"
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
