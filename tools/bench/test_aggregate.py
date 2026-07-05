#!/usr/bin/env python3
"""Unit tests for aggregate.py. Run: python3 tools/bench/test_aggregate.py"""

import json
import os
import shutil
import tempfile
import unittest

from aggregate import aggregate_reps, build_result


class AggregateRepsTest(unittest.TestCase):
    def test_discards_first_rep_as_warmup(self):
        rows = [{"ops_per_sec": 1}, {"ops_per_sec": 100}, {"ops_per_sec": 200}]
        result = aggregate_reps(rows)
        self.assertEqual(result["ops_per_sec"], 150)  # median of [100, 200]

    def test_single_rep_not_discarded(self):
        rows = [{"ops_per_sec": 42}]
        result = aggregate_reps(rows)
        self.assertEqual(result["ops_per_sec"], 42)


class BuildResultTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_combines_target_workload_files(self):
        with open(os.path.join(self.tmp, "plexdb.write.jsonl"), "w") as f:
            f.write(json.dumps({"ops_per_sec": 1}) + "\n")
            f.write(json.dumps({"ops_per_sec": 1000}) + "\n")
        with open(os.path.join(self.tmp, "cassandra.read.jsonl"), "w") as f:
            f.write(json.dumps({"ops_per_sec": 500}) + "\n")

        result = build_result({"hostname": "bench1"}, self.tmp, "container")
        self.assertEqual(result["mode"], "container")
        self.assertEqual(result["host"]["hostname"], "bench1")
        self.assertEqual(result["targets"]["plexdb"]["write"]["ops_per_sec"], 1000)
        self.assertEqual(result["targets"]["cassandra"]["read"]["ops_per_sec"], 500)


if __name__ == "__main__":
    unittest.main()
