"""Fuxi grouped PMU events, benchmark groups, and interval-local metrics."""

import argparse
from enum import IntEnum
from pathlib import Path
import re

# Keep event IDs synchronized with Fuxi consts/PerfEvent.scala. All events are
# single pulses; cycle events may overlap and must not be summed into CPI.
class Event(IntEnum):
    BRANCH = 0x01000000
    BRANCH_MISS = 0x01000001
    BRANCH_DIRECTION_MISS = 0x01000002
    BRANCH_TARGET_MISS = 0x01000003
    JAL = 0x01000004
    JAL_MISS = 0x01000005
    JALR = 0x01000006
    JALR_MISS = 0x01000007
    RAS_CANDIDATE = 0x01000008
    RAS_USED = 0x01000009
    RAS_MISS = 0x0100000a
    RAS_FALLBACK_MISS = 0x0100000b
    OTHER_JALR = 0x0100000c
    OTHER_JALR_MISS = 0x0100000d
    COMPRESSED = 0x0100000e
    LOAD = 0x0100000f
    STORE = 0x01000010
    FRONTEND_EMPTY = 0x02000000
    LOAD_USE_STALL = 0x02000001
    MDU_STALL = 0x02000002
    MEM_STALL = 0x02000003
    CSR_STALL = 0x02000004
    FETCH_QUEUE_FULL = 0x02000005
    DECODE_REDIRECT = 0x02000006
    CONTEXT_FLUSH = 0x02000007
    RAS_OVERFLOW = 0x02000008
    RAS_EMPTY_POP = 0x02000009
    RAS_CLEAR_MISPREDICT = 0x0200000a
    RAS_CLEAR_CONTEXT = 0x0200000b
    ICACHE_ACCESS = 0x03000000
    ICACHE_MISS = 0x03000001
    ICACHE_BUSY = 0x03000002
    ICACHE_WALK_ACCESS = 0x03000003
    ICACHE_WALK_MISS = 0x03000004
    DCACHE_ACCESS = 0x03000005
    DCACHE_MISS = 0x03000006
    DCACHE_BUSY = 0x03000007
    DCACHE_WRITEBACK = 0x03000008
    DCACHE_WALK_ACCESS = 0x03000009
    DCACHE_WALK_MISS = 0x0300000a
    ITLB_MISS = 0x0300000b
    ITLB_WALK_CYCLE = 0x0300000c
    DTLB_MISS = 0x0300000d
    DTLB_WALK_CYCLE = 0x0300000e


EVENT_NAMES = {
    event: event.name.lower() + ("_retired" if event >> 24 == 0x01 else "")
    for event in Event
}
CYCLE_EVENTS = {
    Event.FRONTEND_EMPTY, Event.LOAD_USE_STALL, Event.MDU_STALL, Event.MEM_STALL,
    Event.CSR_STALL, Event.FETCH_QUEUE_FULL, Event.ICACHE_BUSY, Event.DCACHE_BUSY,
    Event.ITLB_WALK_CYCLE, Event.DTLB_WALK_CYCLE,
}
# Benchmark measurement sets can mix hardware event categories. These group
# numbers are not the high-byte group IDs in mhpmeventN.
GROUPS = {
    1: ("branch", (Event.BRANCH, Event.BRANCH_MISS, Event.BRANCH_DIRECTION_MISS,
                   Event.BRANCH_TARGET_MISS, Event.JAL, Event.JAL_MISS,
                   Event.JALR, Event.JALR_MISS)),
    2: ("ras", (Event.RAS_CANDIDATE, Event.RAS_USED, Event.RAS_MISS,
                Event.RAS_FALLBACK_MISS, Event.OTHER_JALR, Event.OTHER_JALR_MISS,
                Event.RAS_CLEAR_MISPREDICT, Event.RAS_CLEAR_CONTEXT)),
    3: ("stalls", (Event.FRONTEND_EMPTY, Event.LOAD_USE_STALL, Event.MDU_STALL,
                   Event.MEM_STALL, Event.CSR_STALL, Event.FETCH_QUEUE_FULL,
                   Event.DECODE_REDIRECT, Event.CONTEXT_FLUSH)),
    4: ("cache", (Event.ICACHE_ACCESS, Event.ICACHE_MISS, Event.ICACHE_BUSY,
                  Event.DCACHE_ACCESS, Event.DCACHE_MISS, Event.DCACHE_BUSY,
                  Event.DCACHE_WRITEBACK, Event.COMPRESSED)),
    5: ("translation", (Event.ICACHE_WALK_ACCESS, Event.ICACHE_WALK_MISS,
                        Event.DCACHE_WALK_ACCESS, Event.DCACHE_WALK_MISS,
                        Event.ITLB_MISS, Event.ITLB_WALK_CYCLE,
                        Event.DTLB_MISS, Event.DTLB_WALK_CYCLE)),
    6: ("mix", (Event.COMPRESSED, Event.LOAD, Event.STORE, Event.RAS_OVERFLOW,
                Event.RAS_EMPTY_POP, Event.RAS_CANDIDATE, Event.RAS_USED,
                Event.DECODE_REDIRECT)),
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
        if value > cycles or (event >> 24 == 0x01 and value > instret):
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

    branch_errors = (Event.BRANCH_MISS, Event.BRANCH_DIRECTION_MISS, Event.BRANCH_TARGET_MISS)
    if all(event in counts for event in branch_errors):
        if counts[Event.BRANCH_MISS] != (counts[Event.BRANCH_DIRECTION_MISS] +
                                        counts[Event.BRANCH_TARGET_MISS]):
            raise ValueError("Branch misses must equal direction plus target misses")
    for disjoint in ((Event.BRANCH, Event.JAL, Event.JALR),
                     (Event.RAS_CANDIDATE, Event.OTHER_JALR),
                     (Event.RAS_CANDIDATE, Event.LOAD, Event.STORE)):
        if all(event in counts for event in disjoint) and sum(counts[event] for event in disjoint) > instret:
            raise ValueError("Disjoint retired event counts exceed instret")

    metrics = {}
    for name, total, wrong in (("branch", Event.BRANCH, Event.BRANCH_MISS),
                               ("jal", Event.JAL, Event.JAL_MISS),
                               ("jalr", Event.JALR, Event.JALR_MISS),
                               ("other_jalr", Event.OTHER_JALR, Event.OTHER_JALR_MISS)):
        if total in counts and wrong in counts:
            if counts[wrong] > counts[total]:
                raise ValueError(f"{name} misses exceed retired {name} count")
            metrics[f"{name}_prediction_accuracy"] = ratio(counts[total] - counts[wrong], counts[total])
            metrics[f"{name}_mpki"] = counts[wrong] * 1000 / instret
    control_flow_misses = (Event.BRANCH_MISS, Event.JAL_MISS,
                           Event.JALR_MISS)
    if all(event in counts for event in control_flow_misses):
        metrics["control_flow_mpki"] = sum(counts[event] for event in control_flow_misses) * 1000 / instret
    if Event.RAS_CANDIDATE in counts and Event.RAS_USED in counts:
        if counts[Event.RAS_USED] > counts[Event.RAS_CANDIDATE]:
            raise ValueError("RAS use exceeds RAS candidate count")
        metrics["ras_coverage"] = ratio(counts[Event.RAS_USED], counts[Event.RAS_CANDIDATE])
    if all(event in counts for event in (Event.RAS_CANDIDATE, Event.RAS_USED,
                                        Event.RAS_MISS, Event.RAS_FALLBACK_MISS)):
        fallback = counts[Event.RAS_CANDIDATE] - counts[Event.RAS_USED]
        if counts[Event.RAS_MISS] > counts[Event.RAS_USED] or counts[Event.RAS_FALLBACK_MISS] > fallback:
            raise ValueError("RAS misses exceed their matching prediction count")
        metrics["ras_used_prediction_accuracy"] = ratio(
            counts[Event.RAS_USED] - counts[Event.RAS_MISS], counts[Event.RAS_USED])
        metrics["ras_fallback_prediction_accuracy"] = ratio(fallback - counts[Event.RAS_FALLBACK_MISS], fallback)
        metrics["ras_candidate_mpki"] = (counts[Event.RAS_MISS] + counts[Event.RAS_FALLBACK_MISS]) * 1000 / instret
    for name, accesses, misses in (("icache", Event.ICACHE_ACCESS, Event.ICACHE_MISS),
                                  ("dcache", Event.DCACHE_ACCESS, Event.DCACHE_MISS),
                                  ("icache_walk", Event.ICACHE_WALK_ACCESS, Event.ICACHE_WALK_MISS),
                                  ("dcache_walk", Event.DCACHE_WALK_ACCESS, Event.DCACHE_WALK_MISS)):
        if accesses in counts and misses in counts:
            # Accesses complete, whereas misses start. Boundary transactions
            # can straddle the interval, so this is not a matched-request rate.
            metrics[f"{name}_misses_per_completed_access"] = ratio(counts[misses], counts[accesses])
            metrics[f"{name}_mpki"] = counts[misses] * 1000 / instret
    return {"version": version, "group": group, "group_name": group_name,
            "measurement": "mcountinhibit_frozen", "cycles": cycles, "instret": instret,
            "counters": [records[counter] for counter in sorted(records)], "metrics": metrics}


def write_header(path):
    text = ["/* Generated from pmu.py; Fuxi grouped event encodings (group[31:24], event[23:0]). */"]
    for group, (_, events) in GROUPS.items():
        text += [f"#if FUXI_PMU_GROUP == {group}",
                 "#define FUXI_PMU_EVENTS {" + ", ".join(f"0x{event:08x}u" for event in events) + "}", "#endif"]
    text += ["#ifndef FUXI_PMU_EVENTS", '#error "Unsupported Fuxi PMU group"', "#endif", ""]
    path.write_text("\n".join(text))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, required=True)
    write_header(parser.parse_args().header)
