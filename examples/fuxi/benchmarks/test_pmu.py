#!/usr/bin/env python3
"""Regression coverage for PMU snapshots and their measurement denominators."""

import unittest

from pmu import GROUPS, parse_pmu


def snapshot(group=1, values=None):
    values = values or [0] * 8
    lines = [f"BENCH_PMU version=1 group={group} slots=8"]
    lines += [f"BENCH_HPM counter={slot} event={event} value={value:016x}"
              for slot, event, value in zip(range(3, 11), GROUPS[group][1], values)]
    return "\n".join(lines) + "\n"


class SnapshotTests(unittest.TestCase):
    def test_legacy_without_pmu(self):
        self.assertIsNone(parse_pmu("BENCH_RESULT\n", 100, 50))
        with self.assertRaises(ValueError):
            parse_pmu("BENCH_RESULT\n", 100, 50, 1)

    def test_branch_metrics_use_current_interval(self):
        result = parse_pmu(snapshot(values=[100, 10, 8, 2, 20, 2, 30, 3]), 2000, 1000, 1)
        self.assertEqual(result["metrics"]["branch_prediction_accuracy"], 0.9)
        self.assertEqual(result["metrics"]["control_flow_mpki"], 15)
        self.assertEqual(result["cycles"], 2000)
        self.assertEqual(result["instret"], 1000)

    def test_ras_and_zero_denominators(self):
        result = parse_pmu(snapshot(2, [100, 80, 4, 3, 0, 0, 7, 0]), 2000, 1000)
        self.assertEqual(result["metrics"]["ras_coverage"], 0.8)
        self.assertEqual(result["metrics"]["ras_used_prediction_accuracy"], 0.95)
        self.assertEqual(result["metrics"]["ras_fallback_prediction_accuracy"], 0.85)
        self.assertIsNone(result["metrics"]["other_jalr_prediction_accuracy"])
        self.assertIsNone(parse_pmu(snapshot(2), 100, 50)["metrics"]["ras_coverage"])

    def test_64bit_and_overlapping_cycle_events(self):
        value = 0x100000007
        result = parse_pmu(snapshot(3, [value] * 8), value, value - 1)
        self.assertEqual(result["counters"][0]["value"], value)
        self.assertEqual(result["counters"][0]["fraction_of_cycles"], 1)
        self.assertNotIn("fraction_of_cycles", result["counters"][6])

    def test_cache_boundary_is_not_matched_request_rate(self):
        result = parse_pmu(snapshot(4, [0, 1, 3, 10, 2, 8, 0, 0]), 100, 50)
        self.assertIsNone(result["metrics"]["icache_misses_per_completed_access"])
        self.assertEqual(result["metrics"]["dcache_mpki"], 40)

    def test_reject_inconsistent_partitions(self):
        for group, values in ((1, [10, 3, 1, 1, 0, 0, 0, 0]),
                              (1, [30, 0, 0, 0, 20, 0, 1, 0]),
                              (2, [30, 0, 0, 0, 21, 0, 0, 0]),
                              (6, [0, 25, 25, 0, 0, 1, 0, 0])):
            with self.subTest(group=group, values=values), self.assertRaises(ValueError):
                parse_pmu(snapshot(group, values), 100, 50)

    def test_reject_malformed_partial_and_duplicate_snapshots(self):
        valid = snapshot()
        for invalid in (valid + valid, "\n".join(valid.splitlines()[:-1]),
                        valid.replace("counter=4", "counter=3"),
                        valid.replace("event=1 ", "event=2 "),
                        valid.replace("version=1", "version=2"),
                        valid.replace("group=1", "group=99"),
                        valid + "BENCH_HPM malformed\n"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                parse_pmu(invalid, 100, 50)
        with self.assertRaises(ValueError):
            parse_pmu(valid, 100, 50, 2)

    def test_reject_impossible_counts_and_ratios(self):
        for group, values in ((1, [1, 2, 0, 0, 0, 0, 0, 0]),
                              (1, [51, 0, 0, 0, 0, 0, 0, 0]),
                              (2, [1, 2, 0, 0, 0, 0, 0, 0]),
                              (2, [3, 2, 0, 2, 0, 0, 0, 0]),
                              (3, [101, 0, 0, 0, 0, 0, 0, 0])):
            with self.subTest(group=group, values=values), self.assertRaises(ValueError):
                parse_pmu(snapshot(group, values), 100, 50)


if __name__ == "__main__":
    unittest.main()
