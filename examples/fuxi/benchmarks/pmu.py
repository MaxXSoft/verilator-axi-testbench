"""Fuxi PMU v1 event ABI, benchmark groups, and interval-local metrics."""

import argparse
from pathlib import Path
import re

# Keep event IDs synchronized with Fuxi consts/PerfEvent.scala. All events are
# single pulses; cycle events may overlap and must not be summed into CPI.
EVENT_NAMES = (
    "disabled", "branch_retired", "branch_miss_retired",
    "branch_direction_miss_retired", "branch_target_miss_retired",
    "jal_retired", "jal_miss_retired", "jalr_retired", "jalr_miss_retired",
    "ras_candidate_retired", "ras_used_retired", "ras_miss_retired",
    "ras_fallback_miss_retired", "other_jalr_retired", "other_jalr_miss_retired",
    "compressed_retired", "load_retired", "store_retired", "frontend_empty",
    "load_use_stall", "mdu_stall", "mem_stall", "csr_stall", "fetch_queue_full",
    "decode_redirect", "context_flush", "ras_overflow", "ras_empty_pop",
    "ras_clear_mispredict", "ras_clear_context", "icache_access", "icache_miss",
    "icache_busy", "icache_walk_access", "icache_walk_miss", "dcache_access",
    "dcache_miss", "dcache_busy", "dcache_writeback", "dcache_walk_access",
    "dcache_walk_miss", "itlb_miss", "itlb_walk_cycle", "dtlb_miss",
    "dtlb_walk_cycle",
)
CYCLE_EVENTS = {18, 19, 20, 21, 22, 23, 32, 37, 42, 44}
GROUPS = {
    1: ("branch", (1, 2, 3, 4, 5, 6, 7, 8)),
    2: ("ras", (9, 10, 11, 12, 13, 14, 28, 29)),
    3: ("stalls", (18, 19, 20, 21, 22, 23, 24, 25)),
    4: ("cache", (30, 31, 32, 35, 36, 37, 38, 15)),
    5: ("translation", (33, 34, 39, 40, 41, 42, 43, 44)),
    6: ("mix", (15, 16, 17, 26, 27, 9, 10, 24)),
}


def parse_pmu(text, cycles, instret, expected_group=None):
    """Reject incomplete/ambiguous snapshots and use only this run's totals."""
    headers = re.findall(r"^BENCH_PMU version=(\d+) group=(\d+) slots=(\d+)$", text, re.M)
    lines = re.findall(r"^BENCH_HPM counter=(\d+) event=(\d+) value=([0-9a-f]{16})$", text, re.M)
    markers = sum(line.startswith(("BENCH_PMU", "BENCH_HPM")) for line in text.splitlines())
    if not markers and expected_group is None:
        return None
    if len(headers) != 1 or len(lines) != 8 or markers != 9:
        raise ValueError("Expected one PMU header and eight complete HPM records")
    version, group, slots = map(int, headers[0])
    if version != 1 or slots != 8 or group not in GROUPS:
        raise ValueError(f"Unsupported PMU snapshot: version={version}, group={group}, slots={slots}")
    if expected_group is not None and group != expected_group:
        raise ValueError(f"Expected PMU group {expected_group}, got {group}")
    if cycles <= 0 or instret <= 0 or instret > cycles:
        raise ValueError("PMU metrics require positive single-issue cycle/instret totals")
    group_name, event_ids = GROUPS[group]
    records = {}
    counts = {}
    for counter_text, event_text, value_text in lines:
        counter, event, value = int(counter_text), int(event_text), int(value_text, 16)
        if counter in records or counter not in range(3, 11):
            raise ValueError(f"Duplicate or invalid HPM counter {counter}")
        if event != event_ids[counter - 3]:
            raise ValueError(f"Unexpected event {event} in HPM counter {counter}")
        if value > cycles or (event <= 17 and value > instret):
            raise ValueError(f"Impossible event count for {EVENT_NAMES[event]}: {value}")
        counts[event] = value
        records[counter] = {
            "counter": counter, "event_id": event, "event": EVENT_NAMES[event],
            "value": value, "unit": "cycles" if event in CYCLE_EVENTS else "occurrences",
            "per_1000_instructions": value * 1000 / instret,
        }
        if event in CYCLE_EVENTS:
            records[counter]["fraction_of_cycles"] = value / cycles

    def ratio(numerator, denominator):
        return numerator / denominator if denominator else None

    if all(event in counts for event in (2, 3, 4)) and counts[2] != counts[3] + counts[4]:
        raise ValueError("Branch misses must equal direction plus target misses")
    for disjoint in ((1, 5, 7), (9, 13), (9, 16, 17)):
        if all(event in counts for event in disjoint) and sum(counts[event] for event in disjoint) > instret:
            raise ValueError("Disjoint retired event counts exceed instret")

    metrics = {}
    for name, total, wrong in (("branch", 1, 2), ("jal", 5, 6), ("jalr", 7, 8),
                               ("other_jalr", 13, 14)):
        if total in counts and wrong in counts:
            if counts[wrong] > counts[total]:
                raise ValueError(f"{name} misses exceed retired {name} count")
            metrics[f"{name}_prediction_accuracy"] = ratio(counts[total] - counts[wrong], counts[total])
            metrics[f"{name}_mpki"] = counts[wrong] * 1000 / instret
    if all(event in counts for event in (2, 6, 8)):
        metrics["control_flow_mpki"] = sum(counts[event] for event in (2, 6, 8)) * 1000 / instret
    if 9 in counts and 10 in counts:
        if counts[10] > counts[9]:
            raise ValueError("RAS use exceeds RAS candidate count")
        metrics["ras_coverage"] = ratio(counts[10], counts[9])
    if all(event in counts for event in (9, 10, 11, 12)):
        fallback = counts[9] - counts[10]
        if counts[11] > counts[10] or counts[12] > fallback:
            raise ValueError("RAS misses exceed their matching prediction count")
        metrics["ras_used_prediction_accuracy"] = ratio(counts[10] - counts[11], counts[10])
        metrics["ras_fallback_prediction_accuracy"] = ratio(fallback - counts[12], fallback)
        metrics["ras_candidate_mpki"] = (counts[11] + counts[12]) * 1000 / instret
    for name, accesses, misses in (("icache", 30, 31), ("dcache", 35, 36),
                                  ("icache_walk", 33, 34), ("dcache_walk", 39, 40)):
        if accesses in counts and misses in counts:
            # Accesses complete, whereas misses start. Boundary transactions
            # can straddle the interval, so this is not a matched-request rate.
            metrics[f"{name}_misses_per_completed_access"] = ratio(counts[misses], counts[accesses])
            metrics[f"{name}_mpki"] = counts[misses] * 1000 / instret
    return {"version": version, "group": group, "group_name": group_name,
            "measurement": "mcountinhibit_frozen", "cycles": cycles, "instret": instret,
            "counters": [records[counter] for counter in sorted(records)], "metrics": metrics}


def write_header(path):
    text = ["/* Generated from pmu.py; Fuxi PMU event ABI version 1. */"]
    for group, (_, events) in GROUPS.items():
        text += [f"#if FUXI_PMU_GROUP == {group}",
                 "#define FUXI_PMU_EVENTS {" + ", ".join(map(str, events)) + "}", "#endif"]
    text += ["#ifndef FUXI_PMU_EVENTS", '#error "Unsupported Fuxi PMU group"', "#endif", ""]
    path.write_text("\n".join(text))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, required=True)
    write_header(parser.parse_args().header)
