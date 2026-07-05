#!/usr/bin/env python3
"""Unit tests for parse_stress.py. Run: python3 tools/bench/test_parse_stress.py"""

import unittest

from parse_stress import parse

# Captured verbatim from a real `cassandra-stress read` run against plexdb
# (tools/cassandra_stress/run.sh -n 500 -t 4), including the noise that
# precedes the Results block in real output.
REAL_READ_OUTPUT = """
Sleeping 2s...
Warming up READ with 125 iterations...
Connected to cluster: cql, max pending requests per connection 128, max connections per host 8
Datacenter: datacenter1; Host: /127.0.0.1:19100; Rack: rack1
Failed to connect over JMX; not collecting these stats
Running READ with 4 threads for 500 iteration
Failed to connect over JMX; not collecting these stats
type                                               total ops,    op/s,    pk/s,   row/s,    mean,     med,     .95,     .99,    .999,     max,   time,   stderr, errors,  gc: #,  max ms,  sum ms,  sdv ms,      mb
total,                                                   500,     903,     903,     903,     1.0,     0.9,     1.4,     2.3,     4.2,     4.2,    0.6,  0.00000,      0,      0,       0,       0,       0,       0


Results:
Op rate                   :      903 op/s  [READ: 903 op/s]
Partition rate            :      903 pk/s  [READ: 903 pk/s]
Row rate                  :      903 row/s [READ: 903 row/s]
Latency mean              :    1.0 ms [READ: 1.0 ms]
Latency median            :    0.9 ms [READ: 0.9 ms]
Latency 95th percentile   :    1.4 ms [READ: 1.4 ms]
Latency 99th percentile   :    2.3 ms [READ: 2.3 ms]
Latency 99.9th percentile :    4.2 ms [READ: 4.2 ms]
Latency max               :    4.2 ms [READ: 4.2 ms]
Total partitions          :        500 [READ: 500]
Total errors              :          0 [READ: 0]
Total GC count            : 0
Total GC memory           : 0 B
Total GC time             :    0.0 seconds
Avg GC time               :    NaN ms
StdDev GC time            :    0.0 ms
Total operation time      : 00:00:00

END
"""

# op/s and latency figures over 1000 render with thousands separators.
THOUSANDS_OUTPUT = """
Results:
Op rate                   :    1,091 op/s  [WRITE: 1,091 op/s]
Partition rate            :    1,091 pk/s  [WRITE: 1,091 pk/s]
Row rate                  :    1,091 row/s [WRITE: 1,091 row/s]
Latency mean              :    1.4 ms [WRITE: 1.4 ms]
Latency median            :    1.3 ms [WRITE: 1.3 ms]
Latency 95th percentile   :    1.6 ms [WRITE: 1.6 ms]
Latency 99th percentile   :    1.7 ms [WRITE: 1.7 ms]
Latency 99.9th percentile :    2.3 ms [WRITE: 2.3 ms]
Latency max               :    2.3 ms [WRITE: 2.3 ms]
Total errors              :      1,234 [WRITE: 1234]
"""


class ParseStressTest(unittest.TestCase):
    def test_real_captured_output(self):
        metrics = parse(REAL_READ_OUTPUT)
        self.assertEqual(
            metrics,
            {
                "ops_per_sec": 903,
                "mean_latency_ms": 1.0,
                "p50": 0.9,
                "p95": 1.4,
                "p99": 2.3,
                "p999": 4.2,
                "errors": 0,
            },
        )

    def test_thousands_separators_and_nonzero_errors(self):
        metrics = parse(THOUSANDS_OUTPUT)
        self.assertEqual(metrics["ops_per_sec"], 1091)
        self.assertEqual(metrics["errors"], 1234)

    def test_uses_last_results_block(self):
        # a write+read transcript has two Results: blocks; callers pass one
        # workload's output at a time, but if fed both, the last one wins.
        combined = REAL_READ_OUTPUT + THOUSANDS_OUTPUT
        metrics = parse(combined)
        self.assertEqual(metrics["ops_per_sec"], 1091)

    def test_missing_results_block_raises(self):
        with self.assertRaises(ValueError):
            parse("no results here\n")

    def test_truncated_block_raises(self):
        with self.assertRaises(ValueError):
            parse("Results:\nOp rate                   :      903 op/s  [READ: 903 op/s]\n")


if __name__ == "__main__":
    unittest.main()
