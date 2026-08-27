#!/usr/bin/env python3
"""Summarize the QLIC causal hierarchy-gate replay."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path


FIELD = re.compile(r'([A-Za-z0-9-]+)=("[^"]*"|\S+)')
KINDS = ("zero", "magnitude-head", "magnitude-tail", "mantissa", "sign")


def fields(line: str) -> dict[str, str]:
    return {
        match.group(1): match.group(2).strip('"')
        for match in FIELD.finditer(line)
    }


def empty_record(source: dict[str, str], index: int) -> dict:
    record = {
        "Index": index,
        "Path": source["Path"],
        "SourceGroup": source["SourceGroup"],
        "Category": source["Category"],
        "QlicBytes": int(source["QlicBytes"]),
        "Jxl9Bytes": int(source["Jxl9Bytes"]),
        "CurrentDeltaBytes": int(source["DeltaBytes"]),
        "RangeBytes": 0,
        "Planes": 0,
        "BaselineBits": 0.0,
        "CandidateBits": 0.0,
        "FixedMismatches": 0,
        "FixedError": 0,
        "Increases": 0,
        "Decreases": 0,
        "Unchanged": 0,
        "LowerSaturations": 0,
        "UpperSaturations": 0,
    }
    for kind in KINDS:
        label = kind.replace("-", " ").title().replace(" ", "")
        record[f"{label}Decisions"] = 0
        record[f"{label}BaselineBits"] = 0.0
        record[f"{label}CandidateBits"] = 0.0
    return record


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
            elif line.startswith("hierarchy-gate plane="):
                if current is None:
                    raise ValueError("gate plane appeared before file-begin")
                data = fields(line)
                current["Planes"] += 1
                current["RangeBytes"] += int(data["range-bytes"])
                current["BaselineBits"] += float(data["baseline-bits"])
                current["CandidateBits"] += float(data["candidate-bits"])
                for source, target in (
                    ("fixed-mismatches", "FixedMismatches"),
                    ("fixed-error", "FixedError"),
                    ("increases", "Increases"),
                    ("decreases", "Decreases"),
                    ("unchanged", "Unchanged"),
                    ("lower-saturations", "LowerSaturations"),
                    ("upper-saturations", "UpperSaturations"),
                ):
                    current[target] += int(data[source])
            elif line.startswith("hierarchy-gate-decision "):
                if current is None:
                    raise ValueError("gate decision appeared before file-begin")
                data = fields(line)
                kind = data["kind"]
                if kind not in KINDS:
                    raise ValueError(f"unknown gate decision kind: {kind}")
                label = kind.replace("-", " ").title().replace(" ", "")
                current[f"{label}Decisions"] += int(data["decisions"])
                current[f"{label}BaselineBits"] += float(data["baseline-bits"])
                current[f"{label}CandidateBits"] += float(data["candidate-bits"])
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
    output = [records[index] for index in sorted(records)]
    for record in output:
        delta_bits = record["CandidateBits"] - record["BaselineBits"]
        record["DeltaBytes"] = delta_bits / 8.0
        record["RangePercent"] = (
            100.0 * delta_bits / (record["RangeBytes"] * 8.0)
            if record["RangeBytes"]
            else 0.0
        )
        for kind in KINDS:
            label = kind.replace("-", " ").title().replace(" ", "")
            record[f"{label}DeltaBytes"] = (
                record[f"{label}CandidateBits"]
                - record[f"{label}BaselineBits"]
            ) / 8.0
    return output


def aggregate(records: list[dict], label: str) -> dict:
    result: dict[str, int | float | str] = {
        "SourceGroup": label,
        "Files": len(records),
        "QlicBytes": sum(record["QlicBytes"] for record in records),
        "Jxl9Bytes": sum(record["Jxl9Bytes"] for record in records),
        "CurrentDeltaBytes": sum(record["CurrentDeltaBytes"] for record in records),
        "RangeBytes": sum(record["RangeBytes"] for record in records),
        "Planes": sum(record["Planes"] for record in records),
        "BaselineBits": sum(record["BaselineBits"] for record in records),
        "CandidateBits": sum(record["CandidateBits"] for record in records),
        "FixedMismatches": sum(record["FixedMismatches"] for record in records),
        "FixedError": sum(record["FixedError"] for record in records),
        "Increases": sum(record["Increases"] for record in records),
        "Decreases": sum(record["Decreases"] for record in records),
        "Unchanged": sum(record["Unchanged"] for record in records),
        "LowerSaturations": sum(
            record["LowerSaturations"] for record in records
        ),
        "UpperSaturations": sum(
            record["UpperSaturations"] for record in records
        ),
    }
    delta_bits = result["CandidateBits"] - result["BaselineBits"]
    result["DeltaBytes"] = delta_bits / 8.0
    result["RangePercent"] = (
        100.0 * delta_bits / (result["RangeBytes"] * 8.0)
        if result["RangeBytes"]
        else 0.0
    )
    result["Wins"] = sum(
        record["CandidateBits"] < record["BaselineBits"] for record in records
    )
    result["Losses"] = sum(
        record["CandidateBits"] > record["BaselineBits"] for record in records
    )
    for kind in KINDS:
        kind_label = kind.replace("-", " ").title().replace(" ", "")
        baseline = sum(record[f"{kind_label}BaselineBits"] for record in records)
        candidate = sum(
            record[f"{kind_label}CandidateBits"] for record in records
        )
        kind_delta = candidate - baseline
        result[f"{kind_label}Decisions"] = sum(
            record[f"{kind_label}Decisions"] for record in records
        )
        result[f"{kind_label}BaselineBits"] = baseline
        result[f"{kind_label}CandidateBits"] = candidate
        result[f"{kind_label}DeltaBytes"] = kind_delta / 8.0
        result[f"{kind_label}RangePercent"] = (
            100.0 * kind_delta / (result["RangeBytes"] * 8.0)
            if result["RangeBytes"]
            else 0.0
        )
        result[f"{kind_label}Wins"] = sum(
            record[f"{kind_label}CandidateBits"]
            < record[f"{kind_label}BaselineBits"]
            for record in records
        )
        result[f"{kind_label}Losses"] = sum(
            record[f"{kind_label}CandidateBits"]
            > record[f"{kind_label}BaselineBits"]
            for record in records
        )
    return result


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
    write_csv(args.out / "files.csv", records)

    grouped: dict[str, list[dict]] = defaultdict(list)
    for record in records:
        grouped[record["SourceGroup"]].append(record)
    sources = [aggregate(grouped[name], name) for name in sorted(grouped)]
    overall = aggregate(records, "ALL")
    write_csv(args.out / "sources.csv", [*sources, overall])

    required = 100.0 * args.full_gap_bytes / args.full_mode52_bytes
    summary = {
        "qualification_gap_bytes": args.full_gap_bytes,
        "full_mode52_bytes": args.full_mode52_bytes,
        "required_mode52_reduction_percent": required,
        "acceptance_headroom_percent": 1.0,
        "discovery": overall,
        "sources": sources,
        "trace": str(args.trace.resolve()),
        "files": str(args.files.resolve()),
    }
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )

    kinds = []
    for kind in KINDS:
        label = kind.replace("-", " ").title().replace(" ", "")
        kinds.append((kind, overall[f"{label}RangePercent"]))
    best_kind, best_percent = min(kinds, key=lambda item: item[1])
    verdict = "advance" if overall["RangePercent"] <= -1.0 else "reject"
    lines = [
        "# Adaptive hierarchy gate",
        "",
        f"Verdict: **{verdict}**.",
        "",
        (
            f"The causal one-byte outer gate changed measured mode-52 ranges by "
            f"{overall['RangePercent']:+.6f}% across {overall['Files']} frozen "
            f"discovery files. Negative is smaller. It won {overall['Wins']} "
            f"files and lost {overall['Losses']}."
        ),
        "",
        (
            f"The best isolated decision class was {best_kind} at "
            f"{best_percent:+.6f}% of range bytes. QLIC needs "
            f"{required:.6f}% of all mode-52 bytes to erase the qualification "
            "gap, and this lab requires 1.0% before a wire implementation."
        ),
        "",
        (
            f"The trace reconstructed every current fixed mixture with "
            f"{overall['FixedMismatches']} mismatches and total probability "
            f"error {overall['FixedError']}."
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
