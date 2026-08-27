#!/usr/bin/env python3
"""Summarize the QLIC forward-only probability calibration audit."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path


FIELD = re.compile(r'([A-Za-z0-9-]+)=("[^"]*"|\S+)')
FACTORS = (
    "activity",
    "predictor",
    "history",
    "cross-channel",
    "history+cross",
    "all-factors",
)
BASE_FACTORS = FACTORS[:4]
DECISIONS = (
    "zero",
    "magnitude-head",
    "magnitude-second",
    "sign-small",
    "sign-medium",
    "sign-large",
)


def fields(line: str) -> dict[str, str]:
    return {
        match.group(1): match.group(2).strip('"')
        for match in FIELD.finditer(line)
    }


def empty_record(source: dict[str, str], index: int) -> dict:
    return {
        "Index": index,
        "Path": source["Path"],
        "SourceGroup": source["SourceGroup"],
        "Category": source["Category"],
        "QlicBytes": int(source["QlicBytes"]),
        "Jxl9Bytes": int(source["Jxl9Bytes"]),
        "CurrentDeltaBytes": int(source["DeltaBytes"]),
        "RangeBytes": 0,
        "BaselineBits": 0.0,
        "Planes": 0,
        "factors": {
            factor: {
                "Decisions": 0,
                "Scored": 0,
                "SavingBits": 0.0,
                "planes": defaultdict(float),
                "decisions": defaultdict(float),
            }
            for factor in FACTORS
        },
    }


def read_trace(trace_path: Path, source_rows: list[dict[str, str]]) -> list[dict]:
    records: dict[int, dict] = {}
    current: dict | None = None
    ended: set[int] = set()
    with trace_path.open("r", encoding="utf-8-sig") as trace:
        for raw in trace:
            line = raw.strip()
            if line.startswith("context-cache-file-begin "):
                data = fields(line)
                index = int(data["index"])
                if index < 1 or index > len(source_rows):
                    raise ValueError(f"trace index {index} is outside the file table")
                current = empty_record(source_rows[index - 1], index)
                current["NativeBytes"] = int(data["bytes"])
                records[index] = current
            elif line.startswith("calibration plane="):
                if current is None:
                    raise ValueError("calibration plane appeared before file-begin")
                data = fields(line)
                current["Planes"] += 1
                current["RangeBytes"] += int(data["range-bytes"])
                current["BaselineBits"] += float(data["baseline-bits"])
                current["BlockRows"] = int(data["block-rows"])
                current["MinimumCurvature"] = float(data["minimum-curvature"])
                current["CorrectionLimit"] = float(data["correction-limit"])
            elif line.startswith("calibration-factor "):
                if current is None:
                    raise ValueError("calibration factor appeared before file-begin")
                data = fields(line)
                factor = data["factor"]
                if factor not in FACTORS:
                    raise ValueError(f"unknown factor: {factor}")
                result = current["factors"][factor]
                result["Decisions"] += int(data["decisions"])
                result["Scored"] += int(data["scored"])
                saving = float(data["forward-saving-bits"])
                result["SavingBits"] += saving
                result["planes"][int(data["plane"])] += saving
            elif line.startswith("calibration-decision "):
                if current is None:
                    raise ValueError("calibration decision appeared before file-begin")
                data = fields(line)
                factor = data["factor"]
                decision = data["decision"]
                if factor not in FACTORS or decision not in DECISIONS:
                    raise ValueError(f"unknown calibration cell: {factor}/{decision}")
                current["factors"][factor]["decisions"][decision] += float(
                    data["forward-saving-bits"]
                )
            elif line.startswith("context-cache-file-end "):
                data = fields(line)
                index = int(data["index"])
                if data.get("status") != "ok":
                    raise ValueError(f"file {index} ended with {data.get('status')}")
                ended.add(index)
                current = None

    expected = set(range(1, len(source_rows) + 1))
    if set(records) != expected or ended != expected:
        raise ValueError(
            f"incomplete trace: began={len(records)}, ended={len(ended)}, "
            f"expected={len(source_rows)}"
        )
    return [records[index] for index in sorted(records)]


def aggregate(records: list[dict], factor: str, source: str) -> dict:
    qlic_bytes = sum(record["QlicBytes"] for record in records)
    range_bytes = sum(record["RangeBytes"] for record in records)
    saving_bits = sum(record["factors"][factor]["SavingBits"] for record in records)
    decisions = sum(record["factors"][factor]["Decisions"] for record in records)
    scored = sum(record["factors"][factor]["Scored"] for record in records)
    return {
        "SourceGroup": source,
        "Factor": factor,
        "Files": len(records),
        "QlicBytes": qlic_bytes,
        "RangeBytes": range_bytes,
        "Decisions": decisions,
        "Scored": scored,
        "ScoredPercent": 100.0 * scored / decisions if decisions else 0.0,
        "ForwardSavingBits": saving_bits,
        "ForwardSavingBytes": saving_bits / 8.0,
        "QlicSavingPercent": 100.0 * saving_bits / (qlic_bytes * 8.0),
        "RangeSavingPercent": 100.0 * saving_bits / (range_bytes * 8.0),
        "Wins": sum(record["factors"][factor]["SavingBits"] > 0 for record in records),
        "Losses": sum(record["factors"][factor]["SavingBits"] < 0 for record in records),
    }


def write_csv(path: Path, rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--full-mode52-bytes", type=int, required=True)
    parser.add_argument("--full-gap-bytes", type=int, required=True)
    args = parser.parse_args()

    with args.files.open("r", newline="", encoding="utf-8-sig") as source:
        source_rows = list(csv.DictReader(source))
    records = read_trace(args.trace, source_rows)
    args.out.mkdir(parents=True, exist_ok=True)

    file_rows = []
    for record in records:
        for factor in FACTORS:
            result = record["factors"][factor]
            file_rows.append(
                {
                    "Index": record["Index"],
                    "Path": record["Path"],
                    "SourceGroup": record["SourceGroup"],
                    "Factor": factor,
                    "QlicBytes": record["QlicBytes"],
                    "RangeBytes": record["RangeBytes"],
                    "Decisions": result["Decisions"],
                    "Scored": result["Scored"],
                    "ForwardSavingBits": result["SavingBits"],
                    "QlicSavingPercent": 100.0 * result["SavingBits"] /
                    (record["QlicBytes"] * 8.0),
                }
            )
    write_csv(args.out / "files.csv", file_rows)

    grouped: dict[str, list[dict]] = defaultdict(list)
    for record in records:
        grouped[record["SourceGroup"]].append(record)
    source_results = []
    overall = []
    for factor in FACTORS:
        for source in sorted(grouped):
            source_results.append(aggregate(grouped[source], factor, source))
        overall.append(aggregate(records, factor, "ALL"))
    write_csv(args.out / "sources.csv", [*source_results, *overall])

    decisions = []
    planes = []
    for factor in FACTORS:
        for decision in DECISIONS:
            saving = sum(
                record["factors"][factor]["decisions"][decision]
                for record in records
            )
            decisions.append(
                {
                    "Factor": factor,
                    "Decision": decision,
                    "ForwardSavingBits": saving,
                    "ForwardSavingBytes": saving / 8.0,
                    "QlicSavingPercent": 100.0 * saving /
                    (sum(record["QlicBytes"] for record in records) * 8.0),
                }
            )
        for plane in range(4):
            saving = sum(
                record["factors"][factor]["planes"].get(plane, 0.0)
                for record in records
            )
            planes.append(
                {
                    "Factor": factor,
                    "Plane": plane,
                    "ForwardSavingBits": saving,
                    "ForwardSavingBytes": saving / 8.0,
                    "QlicSavingPercent": 100.0 * saving /
                    (sum(record["QlicBytes"] for record in records) * 8.0),
                }
            )
    write_csv(args.out / "decisions.csv", decisions)
    write_csv(args.out / "planes.csv", planes)

    required = 100.0 * args.full_gap_bytes / args.full_mode52_bytes
    sources_by_factor = {
        factor: [row for row in source_results if row["Factor"] == factor]
        for factor in FACTORS
    }
    best = max(overall, key=lambda row: row["QlicSavingPercent"])
    independent_sum = sum(
        row["QlicSavingPercent"]
        for row in overall
        if row["Factor"] in BASE_FACTORS
    )
    stable = all(
        row["ForwardSavingBits"] > 0 for row in sources_by_factor[best["Factor"]]
    )
    advance = best["QlicSavingPercent"] >= 1.0 and stable
    summary = {
        "qualification_gap_bytes": args.full_gap_bytes,
        "full_mode52_bytes": args.full_mode52_bytes,
        "required_mode52_reduction_percent": required,
        "audit_acceptance_percent": 1.0,
        "forward_only": True,
        "score": "second-order log-odds gain",
        "best_factor": best,
        "independent_factor_sum_percent": independent_sum,
        "best_factor_all_sources_positive": stable,
        "verdict": "advance" if advance else "reject",
        "factors": overall,
        "sources": source_results,
        "trace": str(args.trace.resolve()),
        "files": str(args.files.resolve()),
    }
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )

    lines = [
        "# Forward calibration audit",
        "",
        f"Verdict: **{summary['verdict']}**.",
        "",
        (
            f"The best tested correction was {best['Factor']} at "
            f"{best['QlicSavingPercent']:.6f}% modeled forward saving across "
            f"{best['Files']} frozen files. It won {best['Wins']} and lost "
            f"{best['Losses']}."
        ),
        "",
        (
            f"QLIC needs {required:.6f}% of all mode-52 bytes to erase the "
            "qualification gap. This audit requires 1.0% with every source "
            "family positive before an exact coder trial."
        ),
        "",
        (
            f"The naive sum of the four independent factors is "
            f"{independent_sum:.6f}%. "
            "The explicit additive rows are the meaningful combined tests; "
            "the sum itself is not a bound because the factors overlap."
        ),
        "",
        (
            "Each correction was estimated from completed 16-row blocks and "
            "scored only on later blocks. QLIC probabilities were treated as "
            "P(bit=0). Corrections were withheld below curvature 16 and "
            "clipped to plus or minus 0.5 log-odds."
        ),
        "",
        "No wire mode was created. The sealed reserve was not opened.",
        "",
    ]
    (args.out / "RESULTS.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
