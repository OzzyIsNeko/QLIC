#!/usr/bin/env python3
"""Summarize the QLIC causal context-cache replay."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path


FIELD = re.compile(r'([A-Za-z0-9-]+)=("[^"]*"|\S+)')
CANDIDATES = ("dictionary", "cache512", "cache1024")
DECISIONS = ("zero", "magnitude", "sign")


def fields(line: str) -> dict[str, str]:
    return {
        match.group(1): match.group(2).strip('"')
        for match in FIELD.finditer(line)
    }


def number(value: str) -> int | float:
    return float(value) if any(mark in value for mark in ".eE") else int(value)


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
        "Planes": 0,
        "BaselineBits": 0.0,
        "PacketBytes": 0,
        "EntryBytes": 0,
        **{f"{candidate}Bits": 0.0 for candidate in CANDIDATES},
        **{
            f"{model}{decision.title()}Bits": 0.0
            for model in ("baseline", *CANDIDATES)
            for decision in DECISIONS
        },
        **{
            f"{cache}{metric.title()}": 0
            for cache in ("cache512", "cache1024")
            for metric in (
                "hits",
                "fallbacks",
                "first",
                "second",
                "conflicts",
                "tag-collisions",
            )
        },
    }


def read_trace(trace_path: Path, source_rows: list[dict[str, str]]) -> list[dict]:
    records: dict[int, dict] = {}
    current: dict | None = None
    ended: set[int] = set()
    with trace_path.open("r", encoding="utf-8-sig", errors="strict") as trace:
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
            elif line.startswith("context-cache plane="):
                if current is None:
                    raise ValueError("plane appeared before file-begin")
                data = {key: number(value) for key, value in fields(line).items()}
                current["Planes"] += 1
                current["RangeBytes"] += int(data["range-bytes"])
                current["BaselineBits"] += float(data["baseline-bits"])
                current["PacketBytes"] = int(data["packet-bytes"])
                current["EntryBytes"] = int(data["entry-bytes"])
                for candidate in CANDIDATES:
                    current[f"{candidate}Bits"] += float(data[f"{candidate}-bits"])
            elif line.startswith("context-cache-decisions "):
                if current is None:
                    raise ValueError("decision costs appeared before file-begin")
                data = {key: number(value) for key, value in fields(line).items()}
                for model in ("baseline", *CANDIDATES):
                    for decision in DECISIONS:
                        current[f"{model}{decision.title()}Bits"] += float(
                            data[f"{model}-{decision}"]
                        )
            elif line.startswith("context-cache-state "):
                if current is None:
                    raise ValueError("cache state appeared before file-begin")
                data = {key: number(value) for key, value in fields(line).items()}
                for cache in ("cache512", "cache1024"):
                    for metric in (
                        "hits",
                        "fallbacks",
                        "first",
                        "second",
                        "conflicts",
                        "tag-collisions",
                    ):
                        current[f"{cache}{metric.title()}"] += int(
                            data[f"{cache}-{metric}"]
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
    output = [records[index] for index in sorted(records)]
    for record in output:
        for candidate in CANDIDATES:
            delta_bits = record[f"{candidate}Bits"] - record["BaselineBits"]
            record[f"{candidate}DeltaBytes"] = delta_bits / 8.0
            record[f"{candidate}RangePercent"] = (
                100.0 * delta_bits / (record["RangeBytes"] * 8.0)
                if record["RangeBytes"]
                else 0.0
            )
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
        "PacketBytes": max(record["PacketBytes"] for record in records),
        "EntryBytes": max(record["EntryBytes"] for record in records),
    }
    for candidate in CANDIDATES:
        bits = sum(record[f"{candidate}Bits"] for record in records)
        delta_bits = bits - result["BaselineBits"]
        result[f"{candidate}Bits"] = bits
        result[f"{candidate}DeltaBytes"] = delta_bits / 8.0
        result[f"{candidate}RangePercent"] = (
            100.0 * delta_bits / (result["RangeBytes"] * 8.0)
            if result["RangeBytes"]
            else 0.0
        )
        result[f"{candidate}Wins"] = sum(
            record[f"{candidate}Bits"] < record["BaselineBits"]
            for record in records
        )
        result[f"{candidate}Losses"] = sum(
            record[f"{candidate}Bits"] > record["BaselineBits"]
            for record in records
        )
    for model in ("baseline", *CANDIDATES):
        for decision in DECISIONS:
            result[f"{model}{decision.title()}Bits"] = sum(
                record[f"{model}{decision.title()}Bits"] for record in records
            )
    for cache in ("cache512", "cache1024"):
        for metric in (
            "hits",
            "fallbacks",
            "first",
            "second",
            "conflicts",
            "tag-collisions",
        ):
            result[f"{cache}{metric.title()}"] = sum(
                record[f"{cache}{metric.title()}"] for record in records
            )
        observations = result[f"{cache}Hits"] + result[f"{cache}Fallbacks"]
        result[f"{cache}HitPercent"] = (
            100.0 * result[f"{cache}Hits"] / observations if observations else 0.0
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
    source_summaries = [
        aggregate(grouped[source], source) for source in sorted(grouped)
    ]
    overall = aggregate(records, "ALL")
    write_csv(args.out / "sources.csv", [*source_summaries, overall])

    required_percent = 100.0 * args.full_gap_bytes / args.full_mode52_bytes
    concentration = {}
    for candidate in CANDIDATES:
        savings = sorted(
            (
                max(0.0, -record[f"{candidate}DeltaBytes"])
                for record in records
            ),
            reverse=True,
        )
        concentration[candidate] = {
            "positive_savings_bytes": sum(savings),
            "best_1_savings_bytes": sum(savings[:1]),
            "best_10_savings_bytes": sum(savings[:10]),
            "best_100_savings_bytes": sum(savings[:100]),
        }
    summary = {
        "qualification_gap_bytes": args.full_gap_bytes,
        "full_mode52_bytes": args.full_mode52_bytes,
        "required_mode52_reduction_percent": required_percent,
        "acceptance_headroom_percent": 1.0,
        "discovery": overall,
        "sources": source_summaries,
        "positive_savings_concentration": concentration,
        "trace": str(args.trace.resolve()),
        "files": str(args.files.resolve()),
    }
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )

    cache_delta = overall["cache1024RangePercent"]
    dictionary_delta = overall["dictionaryRangePercent"]
    verdict = (
        "advance"
        if dictionary_delta <= -1.0 and cache_delta <= -1.0
        else "reject"
    )
    lines = [
        "# Causal context cache",
        "",
        f"Verdict: **{verdict}**.",
        "",
        (
            f"The frozen discovery slice contains {overall['Files']} mode-52 "
            f"files from {len(source_summaries)} source families. QLIC must save "
            f"{required_percent:.6f}% of all mode-52 bytes to erase the current "
            f"{args.full_gap_bytes:,}-byte qualification gap. The pre-wire "
            "acceptance line is 1.0% so the exact coder has room to lose."
        ),
        "",
        (
            f"The unlimited dictionary changed the measured plane ranges by "
            f"{dictionary_delta:+.6f}%. The 512-entry cache changed them by "
            f"{overall['cache512RangePercent']:+.6f}%, and the 1,024-entry cache "
            f"changed them by {cache_delta:+.6f}%. Negative is smaller."
        ),
        "",
        (
            f"The packet is {overall['PacketBytes']} bytes. The trace entry is "
            f"{overall['EntryBytes']} bytes because it also keeps the full "
            "signature to detect silent tag collisions; a production entry "
            "would not retain that diagnostic field."
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
