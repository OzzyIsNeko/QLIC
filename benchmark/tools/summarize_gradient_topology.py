#!/usr/bin/env python3
"""Reduce QLIC mode-53 gradient-topology traces into a frozen decision record."""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
from collections import defaultdict
from pathlib import Path


FIELD = re.compile(r'([A-Za-z0-9-]+)=("[^"]*"|\S+)')
CLASSES = (1, 4, 8, 16)
WEIGHTS = (1, 2, 3, 4, 5, 6)
BIT_PARTS = ("ZeroIdeal", "ZeroKt", "MagnitudeIdeal", "MagnitudeKt")


def fields(line: str) -> dict[str, str]:
    return {
        match.group(1): match.group(2).strip('"')
        for match in FIELD.finditer(line)
    }


def number(value: str) -> int | float:
    return float(value) if any(mark in value for mark in ".eE") else int(value)


def normalized_path(path: str) -> str:
    return os.path.normcase(os.path.normpath(path))


def source_value(source: dict[str, str], *names: str, default: str = "") -> str:
    for name in names:
        value = source.get(name)
        if value is not None and value != "":
            return value
    return default


def empty_record(source: dict[str, str], index: int) -> dict:
    split = source_value(source, "Split", default="Unspecified").strip().title()
    record: dict[str, int | float | str | list[int] | dict[int, int]] = {
        "Index": index,
        "Stream": source_value(source, "Stream", "Path"),
        "Path": source_value(source, "Path", "Stream"),
        "SourceGroup": source_value(source, "SourceGroup", "Category", default="Unknown"),
        "Category": source_value(source, "Category", "SourceGroup", default="Unknown"),
        "Split": split,
        "ExpectedBytes": int(source_value(source, "ExpectedBytes", "QlicBytes", default="0")),
        "NativeBytes": 0,
        "Planes": 0,
        "Pixels": 0,
        "RangeBytes": 0,
        "ZeroDecisions": 0,
        "MagnitudeDecisions": 0,
        "LiveZeroBits": 0.0,
        "LiveMagnitudeBits": 0.0,
        "SignatureCounts": [0] * 16,
        "_ModelPlanes": {classes: 0 for classes in CLASSES},
        "_SharedPlanes": {
            (classes, weight): 0
            for classes in CLASSES[1:]
            for weight in WEIGHTS
        },
    }
    for classes in CLASSES:
        for part in BIT_PARTS:
            record[f"Class{classes}{part}Bits"] = 0.0
    for classes in CLASSES[1:]:
        for weight in WEIGHTS:
            record[f"Class{classes}Weight{weight}SharedZeroBits"] = 0.0
            record[f"Class{classes}Weight{weight}SharedMagnitudeBits"] = 0.0
    return record


def add_plane(record: dict, data: dict[str, int | float]) -> None:
    record["Planes"] += 1
    record["Pixels"] += int(data["pixels"])
    record["RangeBytes"] += int(data["range-bytes"])
    record["ZeroDecisions"] += int(data["zero-decisions"])
    record["MagnitudeDecisions"] += int(data["magnitude-decisions"])
    record["LiveZeroBits"] += float(data["live-zero-bits"])
    record["LiveMagnitudeBits"] += float(data["live-magnitude-bits"])


def add_model(record: dict, data: dict[str, int | float]) -> None:
    classes = int(data["classes"])
    if classes not in CLASSES:
        raise ValueError(f"unexpected topology class count: {classes}")
    record[f"Class{classes}ZeroIdealBits"] += float(data["zero-ideal-bits"])
    record[f"Class{classes}ZeroKtBits"] += float(data["zero-kt-bits"])
    record[f"Class{classes}MagnitudeIdealBits"] += float(
        data["magnitude-ideal-bits"]
    )
    record[f"Class{classes}MagnitudeKtBits"] += float(
        data["magnitude-kt-bits"]
    )
    record["_ModelPlanes"][classes] += 1


def add_shared(record: dict, data: dict[str, int | float]) -> None:
    classes = int(data["classes"])
    weight = int(data["weight"])
    if classes not in CLASSES[1:] or weight not in WEIGHTS:
        raise ValueError(
            f"unexpected shared topology candidate: classes={classes}, weight={weight}"
        )
    record[f"Class{classes}Weight{weight}SharedZeroBits"] += float(
        data["zero-bits"]
    )
    record[f"Class{classes}Weight{weight}SharedMagnitudeBits"] += float(
        data["magnitude-bits"]
    )
    record["_SharedPlanes"][(classes, weight)] += 1


def finish_record(record: dict) -> None:
    planes = int(record["Planes"])
    for classes in CLASSES:
        if record["_ModelPlanes"][classes] != planes:
            raise ValueError(
                f"file {record['Index']} has {record['_ModelPlanes'][classes]} "
                f"class-{classes} models for {planes} planes"
            )
        ideal = float(record[f"Class{classes}ZeroIdealBits"]) + float(
            record[f"Class{classes}MagnitudeIdealBits"]
        )
        kt = float(record[f"Class{classes}ZeroKtBits"]) + float(
            record[f"Class{classes}MagnitudeKtBits"]
        )
        record[f"Class{classes}IdealBits"] = ideal
        record[f"Class{classes}KtBits"] = kt
    for classes in CLASSES[1:]:
        for weight in WEIGHTS:
            if record["_SharedPlanes"][(classes, weight)] != planes:
                raise ValueError(
                    f"file {record['Index']} has "
                    f"{record['_SharedPlanes'][(classes, weight)]} class-{classes} "
                    f"weight-{weight} shared models for {planes} planes"
                )
            record[f"Class{classes}Weight{weight}SharedBits"] = float(
                record[f"Class{classes}Weight{weight}SharedZeroBits"]
            ) + float(
                record[f"Class{classes}Weight{weight}SharedMagnitudeBits"]
            )
    if sum(record["SignatureCounts"]) != int(record["Pixels"]):
        raise ValueError(
            f"file {record['Index']} signature count does not equal pixels"
        )
    add_savings(record)


def add_savings(record: dict) -> None:
    baseline_ideal = float(record["Class1IdealBits"])
    baseline_kt = float(record["Class1KtBits"])
    range_bits = int(record["RangeBytes"]) * 8.0
    live_bits = float(record["LiveZeroBits"]) + float(record["LiveMagnitudeBits"])
    record["LiveBits"] = live_bits
    for classes in CLASSES[1:]:
        ideal_saving = baseline_ideal - float(record[f"Class{classes}IdealBits"])
        kt_saving = baseline_kt - float(record[f"Class{classes}KtBits"])
        record[f"Class{classes}IdealSavingBits"] = ideal_saving
        record[f"Class{classes}KtSavingBits"] = kt_saving
        record[f"Class{classes}IdealDecisionSavingPercent"] = (
            100.0 * ideal_saving / baseline_ideal if baseline_ideal else 0.0
        )
        record[f"Class{classes}KtDecisionSavingPercent"] = (
            100.0 * kt_saving / baseline_kt if baseline_kt else 0.0
        )
        record[f"Class{classes}IdealRangeSavingPercent"] = (
            100.0 * ideal_saving / range_bits if range_bits else 0.0
        )
        record[f"Class{classes}KtRangeSavingPercent"] = (
            100.0 * kt_saving / range_bits if range_bits else 0.0
        )
        for weight in WEIGHTS:
            key = f"Class{classes}Weight{weight}Shared"
            shared_saving = live_bits - float(record[f"{key}Bits"])
            record[f"{key}SavingBits"] = shared_saving
            record[f"{key}DecisionSavingPercent"] = (
                100.0 * shared_saving / live_bits if live_bits else 0.0
            )
            record[f"{key}RangeSavingPercent"] = (
                100.0 * shared_saving / range_bits if range_bits else 0.0
            )


def read_trace(trace_path: Path, source_rows: list[dict[str, str]]) -> list[dict]:
    records: dict[int, dict] = {}
    ended: set[int] = set()
    current: dict | None = None
    with trace_path.open("r", encoding="utf-8-sig", errors="strict") as trace:
        for raw in trace:
            line = raw.strip()
            if line.startswith("gradient-topology-file-begin "):
                data = fields(line)
                index = int(data["index"])
                if index < 1 or index > len(source_rows):
                    raise ValueError(f"trace index {index} is outside the file table")
                current = empty_record(source_rows[index - 1], index)
                current["NativeBytes"] = int(data["bytes"])
                trace_path_value = data.get("path", "")
                expected_path = str(current["Stream"])
                if expected_path and normalized_path(trace_path_value) != normalized_path(
                    expected_path
                ):
                    raise ValueError(
                        f"file {index} path differs: {trace_path_value} != {expected_path}"
                    )
                records[index] = current
            elif line.startswith("gradient-topology plane="):
                if current is None:
                    raise ValueError("plane appeared before file-begin")
                add_plane(
                    current,
                    {key: number(value) for key, value in fields(line).items()},
                )
            elif line.startswith("gradient-topology-model "):
                if current is None:
                    raise ValueError("model appeared before file-begin")
                add_model(
                    current,
                    {key: number(value) for key, value in fields(line).items()},
                )
            elif line.startswith("gradient-topology-shared "):
                if current is None:
                    raise ValueError("shared model appeared before file-begin")
                add_shared(
                    current,
                    {key: number(value) for key, value in fields(line).items()},
                )
            elif line.startswith("gradient-topology-signatures "):
                if current is None:
                    raise ValueError("signature counts appeared before file-begin")
                values = [int(value) for value in fields(line)["counts"].split(",")]
                if len(values) != 16:
                    raise ValueError("signature line must contain 16 counts")
                current["SignatureCounts"] = [
                    old + new
                    for old, new in zip(current["SignatureCounts"], values)
                ]
            elif line.startswith("gradient-topology-file-end "):
                data = fields(line)
                index = int(data["index"])
                if data.get("status") != "ok":
                    raise ValueError(f"file {index} ended with {data.get('status')}")
                if current is None or int(current["Index"]) != index:
                    raise ValueError(f"file {index} ended out of order")
                finish_record(current)
                ended.add(index)
                current = None

    expected = set(range(1, len(source_rows) + 1))
    if set(records) != expected or ended != expected or current is not None:
        raise ValueError(
            f"incomplete trace: began={len(records)}, ended={len(ended)}, "
            f"expected={len(source_rows)}"
        )
    return [records[index] for index in sorted(records)]


def aggregate(records: list[dict], split: str, source_group: str) -> dict:
    result: dict[str, int | float | str] = {
        "Split": split,
        "SourceGroup": source_group,
        "Files": len(records),
    }
    sum_fields = (
        "ExpectedBytes",
        "NativeBytes",
        "Planes",
        "Pixels",
        "RangeBytes",
        "ZeroDecisions",
        "MagnitudeDecisions",
        "LiveZeroBits",
        "LiveMagnitudeBits",
    )
    for key in sum_fields:
        result[key] = sum(record[key] for record in records)
    for classes in CLASSES:
        for part in (*BIT_PARTS, "Ideal", "Kt"):
            key = f"Class{classes}{part}Bits"
            result[key] = sum(float(record[key]) for record in records)
    for classes in CLASSES[1:]:
        for weight in WEIGHTS:
            for part in ("Zero", "Magnitude", ""):
                key = f"Class{classes}Weight{weight}Shared{part}Bits"
                result[key] = sum(float(record[key]) for record in records)
    add_savings(result)
    for classes in CLASSES[1:]:
        result[f"Class{classes}Wins"] = sum(
            float(record[f"Class{classes}KtSavingBits"]) > 0.0
            for record in records
        )
        result[f"Class{classes}Losses"] = sum(
            float(record[f"Class{classes}KtSavingBits"]) < 0.0
            for record in records
        )
        for weight in WEIGHTS:
            key = f"Class{classes}Weight{weight}Shared"
            result[f"{key}Wins"] = sum(
                float(record[f"{key}SavingBits"]) > 0.0 for record in records
            )
            result[f"{key}Losses"] = sum(
                float(record[f"{key}SavingBits"]) < 0.0 for record in records
            )
    return result


def source_summaries(records: list[dict]) -> list[dict]:
    grouped: dict[tuple[str, str], list[dict]] = defaultdict(list)
    by_split: dict[str, list[dict]] = defaultdict(list)
    for record in records:
        split = str(record["Split"])
        source_group = str(record["SourceGroup"])
        grouped[(split, source_group)].append(record)
        by_split[split].append(record)
    rows = [
        aggregate(grouped[key], key[0], key[1])
        for key in sorted(grouped)
    ]
    rows.extend(
        aggregate(by_split[split], split, "ALL") for split in sorted(by_split)
    )
    return rows


def evaluate(
    summaries: list[dict], minimum_oracle_percent: float
) -> tuple[str, tuple[int, int] | None, list[dict]]:
    overall = {
        str(row["Split"]): row
        for row in summaries
        if row["SourceGroup"] == "ALL"
    }
    source_rows = [row for row in summaries if row["SourceGroup"] != "ALL"]
    split_names = ("Discovery", "Holdout")
    complete = all(split in overall for split in split_names)
    candidates: list[dict] = []
    for classes in CLASSES[1:]:
        for weight in WEIGHTS:
            key = f"Class{classes}Weight{weight}Shared"
            discovery_oracle = complete and float(
                overall["Discovery"][f"Class{classes}IdealDecisionSavingPercent"]
            ) >= minimum_oracle_percent
            holdout_oracle = complete and float(
                overall["Holdout"][f"Class{classes}IdealDecisionSavingPercent"]
            ) >= minimum_oracle_percent
            discovery_causal = complete and float(
                overall["Discovery"][f"{key}SavingBits"]
            ) > 0.0
            holdout_causal = complete and float(
                overall["Holdout"][f"{key}SavingBits"]
            ) > 0.0
            discovery_family = complete and all(
                float(row[f"{key}SavingBits"]) >= 0.0
                for row in source_rows
                if row["Split"] == "Discovery"
            )
            holdout_family = complete and all(
                float(row[f"{key}SavingBits"]) >= 0.0
                for row in source_rows
                if row["Split"] == "Holdout"
            )
            candidates.append(
                {
                    "classes": classes,
                    "weight_eighths": weight,
                    "discovery_oracle_gate": discovery_oracle,
                    "holdout_oracle_gate": holdout_oracle,
                    "discovery_shared_gate": discovery_causal,
                    "holdout_shared_gate": holdout_causal,
                    "discovery_source_family_gate": discovery_family,
                    "holdout_source_family_gate": holdout_family,
                    "discovery_shared_range_saving_percent": (
                        float(overall["Discovery"][f"{key}RangeSavingPercent"])
                        if complete
                        else 0.0
                    ),
                    "holdout_shared_range_saving_percent": (
                        float(overall["Holdout"][f"{key}RangeSavingPercent"])
                        if complete
                        else 0.0
                    ),
                    "discovery_eligible": (
                        discovery_oracle and discovery_causal and discovery_family
                    ),
                }
            )
    if not complete:
        return "incomplete-split", None, candidates
    eligible = [candidate for candidate in candidates if candidate["discovery_eligible"]]
    if not eligible:
        return "reject", None, candidates
    selected = max(
        eligible,
        key=lambda candidate: candidate["discovery_shared_range_saving_percent"],
    )
    holdout_passes = (
        selected["holdout_oracle_gate"]
        and selected["holdout_shared_gate"]
        and selected["holdout_source_family_gate"]
    )
    choice = (int(selected["classes"]), int(selected["weight_eighths"]))
    return (
        "advance-to-lab-mode" if holdout_passes else "reject-on-holdout",
        choice,
        candidates,
    )


def csv_record(record: dict) -> dict:
    return {
        key: ",".join(str(value) for value in item)
        if key == "SignatureCounts"
        else item
        for key, item in record.items()
        if not key.startswith("_")
    }


def write_csv(path: Path, rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def write_results(
    path: Path, summary: dict, overall: dict[str, dict]
) -> None:
    lines = [
        "# Gradient-topology context replay",
        "",
        f"Verdict: **{summary['verdict']}**.",
        "",
        (
            "The experiment changes no wire syntax and no production decoder. "
            "Ideal entropy is an upper bound; the causal KT replay charges each "
            "new context for sparse observations. The shared replay measures a "
            "small probability bank blended with QLIC's live model. Negative "
            "savings mean larger."
        ),
        "",
        "| Classes | Weight | Discovery oracle | Holdout oracle | Discovery shared/range | Holdout shared/range | Result |",
        "|---:|---:|---:|---:|---:|---:|:---|",
    ]
    for classes in CLASSES[1:]:
        candidate = max(
            (
                item
                for item in summary["candidates"]
                if item["classes"] == classes
            ),
            key=lambda item: item["discovery_shared_range_saving_percent"],
        )
        weight = int(candidate["weight_eighths"])
        key = f"Class{classes}Weight{weight}Shared"
        discovery = overall.get("Discovery", {})
        holdout = overall.get("Holdout", {})
        selected = (
            summary["selected_classes"] == classes
            and summary["selected_weight_eighths"] == weight
        )
        lines.append(
            "| {classes} | {weight}/8 | {di:+.4f}% | {hi:+.4f}% | "
            "{ds:+.4f}% | {hs:+.4f}% | {result} |".format(
                classes=classes,
                weight=weight,
                di=float(discovery.get(f"Class{classes}IdealDecisionSavingPercent", 0.0)),
                hi=float(holdout.get(f"Class{classes}IdealDecisionSavingPercent", 0.0)),
                ds=float(discovery.get(f"{key}RangeSavingPercent", 0.0)),
                hs=float(holdout.get(f"{key}RangeSavingPercent", 0.0)),
                result="selected" if selected else "not selected",
            )
        )
    lines.extend(
        [
            "",
            (
                f"The predeclared oracle threshold is "
                f"{summary['minimum_oracle_decision_saving_percent']:.2f}% on "
                "both discovery and holdout. Every represented source family "
                "must also be nonnegative under the shared causal replay. The "
                "blend weight is selected on discovery only."
            ),
            "",
            "A passing result permits a bounded decoder prototype; it is not production approval.",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def run(args: argparse.Namespace) -> dict:
    with args.files.open("r", newline="", encoding="utf-8-sig") as source:
        source_rows = list(csv.DictReader(source))
    if not source_rows:
        raise ValueError("file table is empty")
    records = read_trace(args.trace, source_rows)
    summaries = source_summaries(records)
    verdict, selected, candidates = evaluate(
        summaries, args.minimum_oracle_percent
    )
    overall = {
        str(row["Split"]): row
        for row in summaries
        if row["SourceGroup"] == "ALL"
    }
    summary = {
        "schema": 1,
        "experiment": "bounded-gradient-topology-context",
        "trace": str(args.trace.resolve()),
        "files": str(args.files.resolve()),
        "minimum_oracle_decision_saving_percent": args.minimum_oracle_percent,
        "verdict": verdict,
        "selected_classes": selected[0] if selected else None,
        "selected_weight_eighths": selected[1] if selected else None,
        "production_ready": False,
        "decoder_hot_loop_gate": "not-yet-measured",
        "candidates": candidates,
        "splits": overall,
        "sources": [row for row in summaries if row["SourceGroup"] != "ALL"],
    }
    args.out.mkdir(parents=True, exist_ok=True)
    write_csv(args.out / "files.csv", [csv_record(record) for record in records])
    write_csv(args.out / "sources.csv", summaries)
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    write_results(args.out / "RESULTS.md", summary, overall)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--minimum-oracle-percent", type=float, default=2.0)
    args = parser.parse_args()
    print(json.dumps(run(args), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
