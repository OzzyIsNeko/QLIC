#!/usr/bin/env python3
"""Summarize the QLIC causal residual-alphabet replay."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path


FIELD = re.compile(r'([A-Za-z0-9-]+)=("[^"]*"|\S+)')
VARIANTS = ("center-k1", "center-k3", "center-k7")
COUNTERS = (
    "admitted",
    "sets0",
    "sets1",
    "sets2",
    "hits",
    "exact-center-hits",
    "alias-hits",
    "source-west",
    "source-north",
    "source-both",
    "levels-removed",
    "boundary-misses",
    "decision-saving",
    "hit-decision-saving",
    "ordinary-decision-saving",
)
BIT_FIELDS = (
    "promotion-saving-bits",
    "ordinary-saving-bits",
    "displacement-loss-bits",
)


def fields(line: str) -> dict[str, str]:
    return {
        match.group(1): match.group(2).strip('"')
        for match in FIELD.finditer(line)
    }


def number(value: str) -> int | float:
    return float(value) if any(mark in value for mark in ".eE") else int(value)


def empty_file(source: dict[str, str], index: int) -> dict:
    row: dict[str, int | float | str] = {
        "Index": index,
        "Path": source["Path"],
        "SourceGroup": source["SourceGroup"],
        "Category": source["Category"],
        "QlicBytes": int(source["QlicBytes"]),
        "Jxl9Bytes": int(source["Jxl9Bytes"]),
        "CurrentDeltaBytes": int(source["DeltaBytes"]),
        "RangeBytes": 0,
        "RangeDecisions": 0,
        "Planes": 0,
        "Pixels": 0,
        "Nonzero": 0,
        "LiveMagnitudeBits": 0.0,
        "BaselineMagnitudeBits": 0.0,
        "LiveMagnitudeDecisions": 0,
        "BaselineMagnitudeDecisions": 0,
        "ProbabilityMismatches": 0,
        "ProbabilityError": 0,
        "BitMismatches": 0,
        "CountMismatches": 0,
    }
    for variant in VARIANTS:
        prefix = variant.replace("-", "_")
        row[f"{prefix}_candidate_bits"] = 0.0
        row[f"{prefix}_modeled_bytes"] = 0.0
        for key in COUNTERS:
            row[f"{prefix}_{key.replace('-', '_')}"] = 0
        for key in BIT_FIELDS:
            row[f"{prefix}_{key.replace('-', '_')}"] = 0.0
    return row


def add_plane(row: dict, data: dict[str, int | float]) -> None:
    row["RangeBytes"] += int(data["range-bytes"])
    row["RangeDecisions"] += int(data["range-decisions"])
    row["Planes"] += 1
    row["Pixels"] += int(data["pixels"])
    row["Nonzero"] += int(data["nonzero"])
    row["LiveMagnitudeBits"] += float(data["live-magnitude-bits"])
    row["BaselineMagnitudeBits"] += float(data["baseline-magnitude-bits"])
    row["LiveMagnitudeDecisions"] += int(data["live-magnitude-decisions"])
    row["BaselineMagnitudeDecisions"] += int(
        data["baseline-magnitude-decisions"]
    )
    row["ProbabilityMismatches"] += int(data["probability-mismatches"])
    row["ProbabilityError"] += int(data["probability-error"])
    row["BitMismatches"] += int(data["bit-mismatches"])
    row["CountMismatches"] += int(data["count-mismatches"])


def add_variant(row: dict, data: dict[str, int | float]) -> None:
    prefix = str(data["variant"]).replace("-", "_")
    row[f"{prefix}_candidate_bits"] += float(data["candidate-magnitude-bits"])
    row[f"{prefix}_modeled_bytes"] += float(data["modeled-bytes"])
    for key in COUNTERS:
        row[f"{prefix}_{key.replace('-', '_')}"] += int(data[key])
    for key in BIT_FIELDS:
        row[f"{prefix}_{key.replace('-', '_')}"] += float(data[key])


def finish_file(row: dict) -> None:
    for variant in VARIANTS:
        prefix = variant.replace("-", "_")
        saving_bits = (
            float(row["BaselineMagnitudeBits"])
            - float(row[f"{prefix}_candidate_bits"])
        )
        row[f"{prefix}_saving_bytes"] = saving_bits / 8.0
        row[f"{prefix}_saving_percent"] = (
            100.0 * saving_bits / (int(row["RangeBytes"]) * 8.0)
            if row["RangeBytes"]
            else 0.0
        )


def read_trace(
    trace_path: Path, source_rows: list[dict[str, str]]
) -> tuple[list[dict], list[dict], list[dict]]:
    records: dict[int, dict] = {}
    current: dict | None = None
    current_plane: dict | None = None
    planes: list[dict] = []
    hit_classes: list[dict] = []
    ended: set[int] = set()
    with trace_path.open("r", encoding="utf-8-sig", errors="strict") as trace:
        for raw in trace:
            line = raw.strip()
            if line.startswith("context-cache-file-begin "):
                data = fields(line)
                index = int(data["index"])
                if index < 1 or index > len(source_rows):
                    raise ValueError(f"trace index {index} is outside the file table")
                current = empty_file(source_rows[index - 1], index)
                current["NativeBytes"] = int(data["bytes"])
                records[index] = current
                current_plane = None
            elif line.startswith("residual-grammar plane="):
                if current is None:
                    raise ValueError("plane appeared before file-begin")
                data = {key: number(value) for key, value in fields(line).items()}
                add_plane(current, data)
                current_plane = {
                    "Index": current["Index"],
                    "SourceGroup": current["SourceGroup"],
                    "Plane": int(data["plane"]),
                    **data,
                }
                planes.append(current_plane)
            elif line.startswith("residual-grammar-variant "):
                if current is None or current_plane is None:
                    raise ValueError("variant appeared before plane")
                raw_data = fields(line)
                data = {
                    key: value if key == "variant" else number(value)
                    for key, value in raw_data.items()
                }
                add_variant(current, data)
                variant = str(data["variant"]).replace("-", "_")
                for key, value in data.items():
                    if key not in ("plane", "variant"):
                        current_plane[f"{variant}_{key.replace('-', '_')}"] = value
            elif line.startswith("residual-grammar-hit-class "):
                if current is None:
                    raise ValueError("hit class appeared before file-begin")
                data = fields(line)
                hit_classes.append(
                    {
                        "Index": current["Index"],
                        "SourceGroup": current["SourceGroup"],
                        "Plane": int(data["plane"]),
                        "Variant": data["variant"],
                        "Class": int(data["class"]),
                        "Hits": int(data["hits"]),
                    }
                )
            elif line.startswith("context-cache-file-end "):
                data = fields(line)
                index = int(data["index"])
                if data.get("status") != "ok":
                    raise ValueError(f"file {index} ended with {data.get('status')}")
                finish_file(records[index])
                ended.add(index)
                current = None
                current_plane = None

    expected = set(range(1, len(source_rows) + 1))
    if set(records) != expected or ended != expected:
        raise ValueError(
            f"incomplete trace: began={len(records)}, ended={len(ended)}, "
            f"expected={len(source_rows)}"
        )
    return [records[index] for index in sorted(records)], planes, hit_classes


def aggregate(records: list[dict], label: str) -> dict:
    result: dict[str, int | float | str] = {
        "SourceGroup": label,
        "Files": len(records),
    }
    common = (
        "QlicBytes",
        "Jxl9Bytes",
        "CurrentDeltaBytes",
        "RangeBytes",
        "RangeDecisions",
        "Planes",
        "Pixels",
        "Nonzero",
        "LiveMagnitudeBits",
        "BaselineMagnitudeBits",
        "LiveMagnitudeDecisions",
        "BaselineMagnitudeDecisions",
        "ProbabilityMismatches",
        "ProbabilityError",
        "BitMismatches",
        "CountMismatches",
    )
    for key in common:
        result[key] = sum(record[key] for record in records)
    for variant in VARIANTS:
        prefix = variant.replace("-", "_")
        candidate_bits = sum(
            record[f"{prefix}_candidate_bits"] for record in records
        )
        saving_bits = float(result["BaselineMagnitudeBits"]) - candidate_bits
        result[f"{prefix}_candidate_bits"] = candidate_bits
        result[f"{prefix}_saving_bytes"] = saving_bits / 8.0
        result[f"{prefix}_saving_percent"] = (
            100.0 * saving_bits / (int(result["RangeBytes"]) * 8.0)
            if result["RangeBytes"]
            else 0.0
        )
        result[f"{prefix}_wins"] = sum(
            record[f"{prefix}_saving_bytes"] > 0.0 for record in records
        )
        result[f"{prefix}_losses"] = sum(
            record[f"{prefix}_saving_bytes"] < 0.0 for record in records
        )
        result[f"{prefix}_ties"] = len(records) - int(
            result[f"{prefix}_wins"]
        ) - int(result[f"{prefix}_losses"])
        for key in COUNTERS:
            name = f"{prefix}_{key.replace('-', '_')}"
            result[name] = sum(record[name] for record in records)
        for key in BIT_FIELDS:
            name = f"{prefix}_{key.replace('-', '_')}"
            result[name] = sum(record[name] for record in records)
    return result


def aggregate_planes(rows: list[dict], label: str) -> dict:
    result: dict[str, int | float | str] = {
        "Plane": label,
        "Rows": len(rows),
        "RangeBytes": sum(int(row["range-bytes"]) for row in rows),
        "RangeDecisions": sum(int(row["range-decisions"]) for row in rows),
        "Pixels": sum(int(row["pixels"]) for row in rows),
        "Nonzero": sum(int(row["nonzero"]) for row in rows),
        "BaselineMagnitudeBits": sum(
            float(row["baseline-magnitude-bits"]) for row in rows
        ),
    }
    for variant in VARIANTS:
        prefix = variant.replace("-", "_")
        candidate = sum(
            float(row[f"{prefix}_candidate_magnitude_bits"]) for row in rows
        )
        saving = float(result["BaselineMagnitudeBits"]) - candidate
        result[f"{prefix}_candidate_bits"] = candidate
        result[f"{prefix}_saving_bytes"] = saving / 8.0
        result[f"{prefix}_saving_percent"] = (
            100.0 * saving / (int(result["RangeBytes"]) * 8.0)
            if result["RangeBytes"]
            else 0.0
        )
        for key in (
            "decision_saving",
            "hits",
            "levels_removed",
            "boundary_misses",
            "promotion_saving_bits",
            "ordinary_saving_bits",
            "displacement_loss_bits",
        ):
            result[f"{prefix}_{key}"] = sum(
                row.get(f"{prefix}_{key}", 0) for row in rows
            )
    return result


def aggregate_hit_classes(rows: list[dict]) -> list[dict]:
    counts: dict[tuple[int, str, int], int] = defaultdict(int)
    for row in rows:
        key = (int(row["Plane"]), str(row["Variant"]), int(row["Class"]))
        counts[key] += int(row["Hits"])
    return [
        {"Plane": plane, "Variant": variant, "Class": magnitude, "Hits": hits}
        for (plane, variant, magnitude), hits in sorted(counts.items())
    ]


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    keys: list[str] = []
    seen: set[str] = set()
    for row in rows:
        for key in row:
            if key not in seen:
                seen.add(key)
                keys.append(key)
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--full-mode52-bytes", type=int, required=True)
    parser.add_argument("--full-gap-bytes", type=int, required=True)
    parser.add_argument("--gate-percent", type=float, default=1.1)
    args = parser.parse_args()

    with args.files.open("r", newline="", encoding="utf-8-sig") as source:
        source_rows = list(csv.DictReader(source))
    records, planes, hit_classes = read_trace(args.trace, source_rows)
    args.out.mkdir(parents=True, exist_ok=True)
    write_csv(args.out / "files.csv", records)
    write_csv(args.out / "planes.csv", planes)
    write_csv(args.out / "hit-classes.csv", hit_classes)
    plane_groups: dict[int, list[dict]] = defaultdict(list)
    for row in planes:
        plane_groups[int(row["Plane"])].append(row)
    plane_summaries = [
        aggregate_planes(plane_groups[plane], str(plane))
        for plane in sorted(plane_groups)
    ]
    write_csv(args.out / "plane-roles.csv", plane_summaries)
    write_csv(
        args.out / "hit-class-summary.csv", aggregate_hit_classes(hit_classes)
    )

    grouped: dict[str, list[dict]] = defaultdict(list)
    for record in records:
        grouped[str(record["SourceGroup"])].append(record)
    source_rows_out = [
        aggregate(grouped[source], source) for source in sorted(grouped)
    ]
    overall = aggregate(records, "ALL")
    write_csv(args.out / "sources.csv", [*source_rows_out, overall])

    concentration: dict[str, dict[str, float]] = {}
    leave_one_out: dict[str, dict[str, float]] = {}
    for variant in VARIANTS:
        prefix = variant.replace("-", "_")
        positive = sorted(
            (max(0.0, float(row[f"{prefix}_saving_bytes"])) for row in records),
            reverse=True,
        )
        concentration[variant] = {
            "positive_savings_bytes": sum(positive),
            "best_1_savings_bytes": sum(positive[:1]),
            "best_10_savings_bytes": sum(positive[:10]),
            "best_100_savings_bytes": sum(positive[:100]),
        }
        leave_one_out[variant] = {}
        for source in sorted(grouped):
            subset = [row for row in records if row["SourceGroup"] != source]
            leave_one_out[variant][source] = float(
                aggregate(subset, f"without-{source}")[f"{prefix}_saving_percent"]
            )

    identity_ok = not any(
        int(overall[key])
        for key in (
            "ProbabilityMismatches",
            "ProbabilityError",
            "BitMismatches",
            "CountMismatches",
        )
    ) and overall["LiveMagnitudeDecisions"] == overall[
        "BaselineMagnitudeDecisions"
    ]
    best_variant = max(
        VARIANTS,
        key=lambda value: float(
            overall[f"{value.replace('-', '_')}_saving_percent"]
        ),
    )
    best_prefix = best_variant.replace("-", "_")
    best_percent = float(overall[f"{best_prefix}_saving_percent"])
    best_decisions = int(overall[f"{best_prefix}_decision_saving"])
    verdict = (
        "advance"
        if identity_ok
        and best_percent >= args.gate_percent
        and best_decisions > 0
        else "reject"
    )
    summary = {
        "verdict": verdict,
        "best_variant": best_variant,
        "best_saving_percent": best_percent,
        "best_decision_saving": best_decisions,
        "identity_ok": identity_ok,
        "gate_percent": args.gate_percent,
        "qualification_gap_bytes": args.full_gap_bytes,
        "full_mode52_bytes": args.full_mode52_bytes,
        "required_mode52_reduction_percent": (
            100.0 * args.full_gap_bytes / args.full_mode52_bytes
        ),
        "discovery": overall,
        "sources": source_rows_out,
        "planes": plane_summaries,
        "positive_savings_concentration": concentration,
        "leave_one_source_out_percent": leave_one_out,
        "trace": str(args.trace.resolve()),
        "files": str(args.files.resolve()),
        "candidate_b": "ineligible: exact west/north residuals are not retained",
        "candidate_c": "ineligible: exact previous-plane residuals are not retained",
    }
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )

    lines = [
        "# Residual alphabet trial",
        "",
        f"Verdict: **{verdict}**.",
        "",
        (
            f"The identity replay matched every live probability, bit, and "
            f"magnitude decision across {overall['Files']} files."
            if identity_ok
            else "The identity replay did not match. No candidate result is valid."
        ),
        "",
    ]
    for variant in VARIANTS:
        prefix = variant.replace("-", "_")
        lines.append(
            f"{variant}: {float(overall[f'{prefix}_saving_percent']):+.6f}% "
            f"of mode-52 plane ranges, "
            f"{int(overall[f'{prefix}_decision_saving']):+,} decisions, "
            f"{int(overall[f'{prefix}_wins'])} wins / "
            f"{int(overall[f'{prefix}_losses'])} losses."
        )
    lines.extend(
        [
            "",
            (
                "Candidate B was not run because exact west and north residuals "
                "are not in the decoder's retained hot state. Candidate C was "
                "not run for the same reason across planes."
            ),
            "",
            (
                f"The pre-wire line was {args.gate_percent:.1f}%. No wire mode "
                "was created and the sealed reserve was not opened."
            ),
            "",
        ]
    )
    (args.out / "RESULTS.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
