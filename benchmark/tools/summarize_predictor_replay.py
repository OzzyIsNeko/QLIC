#!/usr/bin/env python3
"""Reduce exact fixed-tile mode-52 QLIC replays into one decision record."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import defaultdict
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def total(rows: list[dict[str, str]], field: str) -> int:
    return sum(int(row[field]) for row in rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--stream-root", type=Path)
    args = parser.parse_args()

    possible_inputs = {
        "same_settings": args.directory / "same-settings.csv",
        "tile_16": args.directory / "tile-16.csv",
        "tile_8": args.directory / "tile-8.csv",
        "tile_4": args.directory / "tile-4.csv",
    }
    inputs = {name: path for name, path in possible_inputs.items() if path.is_file()}
    configuration_path = args.directory / "configuration.json"
    configuration = (
        json.loads(configuration_path.read_text(encoding="utf-8-sig"))
        if configuration_path.is_file()
        else {}
    )
    required = {"tile_16"}
    if not required.issubset(inputs):
        raise SystemExit(f"missing replay tables: {sorted(required - set(inputs))}")
    rows = {name: read_rows(path) for name, path in inputs.items()}
    counts = {len(value) for value in rows.values()}
    if len(counts) != 1 or not next(iter(counts)):
        raise SystemExit(f"replay table counts differ: {sorted(counts)}")
    file_count = next(iter(counts))
    paths = [{row["Path"] for row in value} for value in rows.values()]
    if any(value != paths[0] for value in paths[1:]):
        raise SystemExit("replay tables do not contain the same files")

    source_by_stream = {}
    for row in read_rows(args.files):
        stream = row.get("Stream") or row.get("StreamPath")
        if not stream:
            continue
        stream_path = Path(stream)
        if not stream_path.is_absolute() and args.stream_root is not None:
            if row.get("EncodeShard", "") != "":
                stream_path = (
                    args.stream_root / f"shard-{row['EncodeShard']}" / stream_path
                )
            else:
                stream_path = args.stream_root / stream_path
        source_by_stream[str(stream_path.resolve())] = row["SourceGroup"]
    reference_name = "same_settings" if "same_settings" in rows else "tile_16"
    baseline = total(rows[reference_name], "BaselineBytes")
    same = (
        total(rows["same_settings"], "CandidateBytes")
        if "same_settings" in rows
        else None
    )
    if same is not None and same != baseline:
        raise SystemExit(f"same-setting replay changed bytes: {baseline} -> {same}")

    by_path = {
        name: {row["Path"]: row for row in value}
        for name, value in rows.items()
    }
    best_8_or_16 = 0
    best_switches = {"tile_16": 0, "tile_8": 0, "unchanged": 0}
    for path in sorted(paths[0]):
        base = int(by_path[reference_name][path]["BaselineBytes"])
        sixteen = int(by_path["tile_16"][path]["CandidateBytes"])
        eight = (
            int(by_path["tile_8"][path]["CandidateBytes"])
            if "tile_8" in by_path
            else base
        )
        best = min(base, sixteen, eight)
        best_8_or_16 += best
        if best == base:
            best_switches["unchanged"] += 1
        elif sixteen <= eight:
            best_switches["tile_16"] += 1
        else:
            best_switches["tile_8"] += 1

    variants = {}
    for name, value in rows.items():
        candidate = total(value, "CandidateBytes")
        first = value[0]
        keep_timing = not str(configuration.get("timingUse", "")).startswith(
            "discard"
        )
        timing_batches = []
        seen_batches = set()
        for row in value:
            batch = row.get("TimingBatch")
            key = (
                ("batch", batch)
                if batch not in (None, "")
                else (
                    "times",
                    row["BaselineSeconds"],
                    row["CandidateSeconds"],
                )
            )
            if key not in seen_batches:
                seen_batches.add(key)
                timing_batches.append(row)
        baseline_seconds = sum(
            float(row["BaselineSeconds"]) for row in timing_batches
        )
        candidate_seconds = sum(
            float(row["CandidateSeconds"]) for row in timing_batches
        )
        variants[name] = {
            "bytes": candidate,
            "delta_bytes": candidate - baseline,
            "delta_percent": 100.0 * (candidate / baseline - 1.0),
            "exact_files": sum(int(row["Exact"]) for row in value),
            "runs": int(first["Runs"]),
            "baseline_decode_seconds": (
                baseline_seconds if keep_timing else None
            ),
            "candidate_decode_seconds": (
                candidate_seconds if keep_timing else None
            ),
            "decode_delta_percent": (
                100.0 * (candidate_seconds - baseline_seconds) / baseline_seconds
                if keep_timing else None
            ),
            "candidate_encode_seconds": sum(
                float(row["CandidateEncodeSeconds"]) for row in value
            ),
        }

    by_source = {}
    grouped: dict[str, list[str]] = defaultdict(list)
    for path in paths[0]:
        grouped[source_by_stream[path]].append(path)
    for source, source_paths in sorted(grouped.items()):
        source_base = sum(
            int(by_path[reference_name][path]["BaselineBytes"])
            for path in source_paths
        )
        by_source[source] = {"files": len(source_paths), "baseline_bytes": source_base}
        for name in ("tile_16", "tile_8", "tile_4"):
            if name not in by_path:
                continue
            candidate = sum(
                int(by_path[name][path]["CandidateBytes"])
                for path in source_paths
            )
            by_source[source][name] = {
                "bytes": candidate,
                "delta_bytes": candidate - source_base,
                "delta_percent": 100.0 * (candidate / source_base - 1.0),
            }

    summary = {
        "schema": 1,
        "files": file_count,
        "baseline_native_bytes": baseline,
        "all_exact": all(
            int(row["Exact"]) == 1
            for value in rows.values()
            for row in value
        ),
        "source_replay_exact_bytes": same == baseline if same is not None else None,
        "variants": variants,
        "free_best_of_tile_8_and_16": {
            "bytes": best_8_or_16,
            "delta_bytes": best_8_or_16 - baseline,
            "delta_percent": 100.0 * (best_8_or_16 / baseline - 1.0),
            "selection_cost_bytes": 0,
            "switches": best_switches,
            "meaning": (
                "strict best of the retained baseline and the available exact "
                "8/16-tile replays; selection needs no new wire syntax"
            ),
        },
        "by_source": by_source,
        "inputs": {
            name: {"path": str(path), "sha256": sha256(path)}
            for name, path in inputs.items()
        },
        "discovery_files": {
            "path": str(args.files),
            "sha256": sha256(args.files),
        },
        "binary": {"path": str(args.binary), "sha256": sha256(args.binary)},
        "limits": {
            "decode_timing_rounds": int(
                configuration.get("runs", variants[reference_name]["runs"])
            ),
            "timing_use": configuration.get(
                "timingUse", "directional only; repeat any promoted candidate"
            ),
            "map_syntax": "current exact predictor-map range syntax",
            "entropy_replay": "full causal encoder replay including cross-plane state",
            "sampled_streams": "retained and re-encoded with original common sample grid",
        },
    }
    (args.directory / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
