#!/usr/bin/env python3
"""Tests for summarize_gradient_topology.py."""

from __future__ import annotations

import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import summarize_gradient_topology as summary


def trace_file(index: int, path: str) -> str:
    models = {
        1: (60.0, 64.0, 40.0, 46.0),
        4: (54.0, 58.0, 36.0, 42.0),
        8: (51.0, 66.0, 34.0, 49.0),
        16: (48.0, 69.0, 32.0, 51.0),
    }
    lines = [
        f'gradient-topology-file-begin index={index} bytes=100 width=10 '
        f'height=1 channels=3 path="{path}"',
        "gradient-topology plane=0 pixels=10 range-bytes=100 zero-contexts=4 "
        "magnitude-contexts=4 zero-decisions=10 magnitude-decisions=5 "
        "live-zero-bits=65.0 live-magnitude-bits=45.0",
    ]
    for classes, values in models.items():
        lines.append(
            "gradient-topology-model plane=0 classes={} zero-ideal-bits={} "
            "zero-kt-bits={} magnitude-ideal-bits={} magnitude-kt-bits={} "
            "total-ideal-bits={} total-kt-bits={}".format(
                classes,
                *values,
                values[0] + values[2],
                values[1] + values[3],
            )
        )
    shared_totals = {
        4: (105.0, 100.0, 102.0, 104.0, 106.0, 108.0),
        8: (111.0, 112.0, 113.0, 114.0, 115.0, 116.0),
        16: (112.0, 114.0, 116.0, 118.0, 120.0, 122.0),
    }
    for classes, totals in shared_totals.items():
        for weight, total in enumerate(totals, 1):
            lines.append(
                "gradient-topology-shared plane=0 classes={} weight={} "
                "zero-bits={} magnitude-bits={} total-bits={}".format(
                    classes, weight, total * 0.6, total * 0.4, total
                )
            )
    lines.extend(
        [
            "gradient-topology-signatures plane=0 counts=1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0",
            f'gradient-topology-file-end index={index} status=ok error=0 path="{path}"',
        ]
    )
    return "\n".join(lines)


class GradientTopologySummaryTest(unittest.TestCase):
    def test_complete_discovery_holdout_selects_penalized_model(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rows = [
                {
                    "Stream": "discovery.qlic",
                    "Path": "discovery.png",
                    "Category": "photo",
                    "Split": "Discovery",
                    "ExpectedBytes": "100",
                },
                {
                    "Stream": "holdout.qlic",
                    "Path": "holdout.png",
                    "Category": "photo",
                    "Split": "Holdout",
                    "ExpectedBytes": "100",
                },
            ]
            table = root / "files.csv"
            with table.open("w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            trace = root / "trace.txt"
            trace.write_text(
                trace_file(1, "discovery.qlic")
                + "\n"
                + trace_file(2, "holdout.qlic")
                + "\ngradient-topology-replay files=2 passed=2 failed=0\n",
                encoding="utf-8",
            )

            records = summary.read_trace(trace, rows)
            summaries = summary.source_summaries(records)
            verdict, selected, candidates = summary.evaluate(summaries, 2.0)

            self.assertEqual(verdict, "advance-to-lab-mode")
            self.assertEqual(selected, (4, 2))
            by_candidate = {
                (candidate["classes"], candidate["weight_eighths"]): candidate
                for candidate in candidates
            }
            self.assertTrue(by_candidate[(4, 2)]["discovery_eligible"])
            self.assertFalse(by_candidate[(8, 1)]["discovery_shared_gate"])
            self.assertFalse(by_candidate[(16, 1)]["discovery_shared_gate"])

    def test_incomplete_signature_counts_are_rejected(self) -> None:
        rows = [
            {
                "Stream": "bad.qlic",
                "Path": "bad.png",
                "Category": "photo",
                "Split": "Discovery",
                "ExpectedBytes": "100",
            }
        ]
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "trace.txt"
            trace.write_text(
                trace_file(1, "bad.qlic").replace(
                    "1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0",
                    "1,1,1",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "16 counts"):
                summary.read_trace(trace, rows)


if __name__ == "__main__":
    unittest.main()
