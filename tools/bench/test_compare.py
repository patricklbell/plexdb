#!/usr/bin/env python3
"""Unit tests for compare.py. Run: python3 tools/bench/test_compare.py"""

import unittest

from compare import compare_result, compare_self_check, slug_of

HOST_A = {"hostname": "bench1", "cpu": {"model": "AMD Ryzen 5 9600X 6-Core Processor", "logical_cpus": 12}}
HOST_A_OTHER_HOST = {"hostname": "bench2", "cpu": {"model": "AMD Ryzen 5 9600X 6-Core Processor", "logical_cpus": 12}}
HOST_B = {"hostname": "bench3", "cpu": {"model": "Intel Core i9-13900K", "logical_cpus": 32}}


def _metrics(ops, mean, p50, p95, p99, p999, errors=0):
    return {
        "ops_per_sec": ops,
        "mean_latency_ms": mean,
        "p50": p50,
        "p95": p95,
        "p99": p99,
        "p999": p999,
        "errors": errors,
    }


def _result(host, ops, mean):
    return {
        "host": host,
        "targets": {
            "plexdb": {"write": _metrics(ops, mean, mean, mean * 1.5, mean * 2, mean * 3)},
        },
    }


def _baseline(host, ops, mean):
    return {
        "slug": slug_of(host),
        "hostname": host["hostname"],
        "targets": {
            "plexdb": {"write": _metrics(ops, mean, mean, mean * 1.5, mean * 2, mean * 3)},
        },
    }


class SlugTest(unittest.TestCase):
    def test_stable_and_distinct(self):
        self.assertEqual(slug_of(HOST_A), slug_of(HOST_A_OTHER_HOST))
        self.assertNotEqual(slug_of(HOST_A), slug_of(HOST_B))


class CompareResultTest(unittest.TestCase):
    def test_no_regression_within_tolerance(self):
        result = _result(HOST_A, ops=1000, mean=1.0)
        baseline = _baseline(HOST_A, ops=950, mean=1.05)  # ~5% worse, within 10% default
        rows, regressions, warnings = compare_result(result, baseline, tolerance=10.0)
        self.assertEqual(regressions, [])
        self.assertEqual(warnings, [])

    def test_throughput_regression_detected(self):
        result = _result(HOST_A, ops=700, mean=1.0)  # 30% drop in ops/sec
        baseline = _baseline(HOST_A, ops=1000, mean=1.0)
        rows, regressions, warnings = compare_result(result, baseline, tolerance=10.0)
        metrics_flagged = {m for _, m, *_ in regressions}
        self.assertIn("ops_per_sec", metrics_flagged)

    def test_latency_regression_detected(self):
        result = _result(HOST_A, ops=1000, mean=2.0)  # latency doubled
        baseline = _baseline(HOST_A, ops=1000, mean=1.0)
        rows, regressions, warnings = compare_result(result, baseline, tolerance=10.0)
        metrics_flagged = {m for _, m, *_ in regressions}
        self.assertIn("mean_latency_ms", metrics_flagged)

    def test_hostname_mismatch_within_slug_warns_not_fails(self):
        result = _result(HOST_A, ops=1000, mean=1.0)
        baseline = _baseline(HOST_A_OTHER_HOST, ops=1000, mean=1.0)
        rows, regressions, warnings = compare_result(result, baseline, tolerance=10.0)
        self.assertEqual(regressions, [])
        self.assertTrue(any("different host" in w for w in warnings))

    def test_cross_hardware_slug_mismatch_refuses(self):
        result = _result(HOST_A, ops=1000, mean=1.0)
        baseline = _baseline(HOST_B, ops=1000, mean=1.0)
        with self.assertRaises(SystemExit):
            compare_result(result, baseline, tolerance=10.0)

    def test_new_errors_flagged_even_within_tolerance(self):
        result = _result(HOST_A, ops=1000, mean=1.0)
        result["targets"]["plexdb"]["write"]["errors"] = 5
        baseline = _baseline(HOST_A, ops=1000, mean=1.0)  # baseline errors=0
        rows, regressions, warnings = compare_result(result, baseline, tolerance=10.0)
        metrics_flagged = {m for _, m, *_ in regressions}
        self.assertIn("errors", metrics_flagged)


class SelfCheckTest(unittest.TestCase):
    def test_no_divergence_within_tolerance(self):
        a = _result(HOST_A, ops=1000, mean=1.0)
        b = _result(HOST_A, ops=980, mean=1.02)
        rows, divergences = compare_self_check(a, b, tolerance=5.0)
        self.assertEqual(divergences, [])

    def test_divergence_flagged_either_direction(self):
        a = _result(HOST_A, ops=1000, mean=1.0)
        b = _result(HOST_A, ops=1300, mean=1.0)  # b is 30% faster than a
        rows, divergences = compare_self_check(a, b, tolerance=5.0)
        metrics_flagged = {m for _, m, *_ in divergences}
        self.assertIn("ops_per_sec", metrics_flagged)


if __name__ == "__main__":
    unittest.main()
