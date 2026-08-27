#!/usr/bin/env python3
"""Summarize the QLIC fixed-residual existing-predictor oracle trace."""

from __future__ import annotations

import argparse
import base64
import csv
import json
import math
import re
from collections import defaultdict
from pathlib import Path


FILE_RE = re.compile(r"predictor-oracle-file index=(\d+) path64=(\S+)")
PLANE_RE = re.compile(
    r"predictor-oracle-plane plane=(\d+) pixels=(\d+) depth=(\d+) tile=(\d+) "
    r"current-proxy=(\d+) pixel-proxy=(\d+) current-regret=(\d+) "
    r"current-equals-pixel-id=(\d+) current-equals-pixel-cost=(\d+) "
    r"purity-majority=(\d+) purity-total=(\d+)"
)
RANK_RE = re.compile(r"predictor-oracle-rank plane=(\d+) counts=([0-9,]+)")
SCALE_RE = re.compile(
    r"predictor-oracle-scale plane=(\d+) scale=(\S+) size=(\d+) "
    r"regions=(\d+) proxy=(\d+) selector-entropy-bits=([0-9.]+) "
    r"second-gap=(\d+) horizontal-transitions=(\d+) "
    r"horizontal-pairs=(\d+) vertical-transitions=(\d+) "
    r"vertical-pairs=(\d+) runs=(\d+)"
)
CHOICES_RE = re.compile(
    r"predictor-oracle-choices plane=(\d+) scale=(\S+) counts=([0-9,]+)"
)
QST_PLANE_RE = re.compile(
    r"qst2-plane plane=(\d+) pixels=(\d+) depth=(\d+) tile=(\d+) "
    r"range-bytes=(\d+) map-bytes=(\d+)"
)
QST_COST_RE = re.compile(
    r"qst2-cost plane=(\d+) map-decisions=(\d+) map-renormalizations=(\d+) "
    r"zero-decisions=(\d+) zero-renormalizations=(\d+) "
    r"magnitude-decisions=(\d+) magnitude-renormalizations=(\d+) "
    r"tail-decisions=(\d+) tail-renormalizations=(\d+) "
    r"sign-decisions=(\d+) sign-renormalizations=(\d+)"
)


def read_csv(path: Path):
    with path.open(newline="", encoding="utf-8-sig") as handle:
        yield from csv.DictReader(handle)


def write_csv(path: Path, fields: list[str], rows):
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def pearson(left: list[int], right: list[int]) -> float | None:
    if len(left) != len(right) or len(left) < 2:
        return None
    left_mean = sum(left) / len(left)
    right_mean = sum(right) / len(right)
    numerator = sum(
        (a - left_mean) * (b - right_mean) for a, b in zip(left, right)
    )
    left_size = sum((value - left_mean) ** 2 for value in left)
    right_size = sum((value - right_mean) ** 2 for value in right)
    if not left_size or not right_size:
        return None
    return numerator / math.sqrt(left_size * right_size)


def image_size(pixels: int) -> str:
    if pixels < 65536:
        return "small-under-64k"
    if pixels < 1048576:
        return "medium-64k-to-1m"
    return "large-at-least-1m"


def parse_trace(path: Path):
    records = []
    current = None
    with path.open(encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            match = FILE_RE.fullmatch(line)
            if match:
                current = {
                    "index": int(match.group(1)),
                    "path": base64.b64decode(match.group(2)).decode("utf-8"),
                    "planes": {},
                    "scales": {},
                }
                records.append(current)
                continue
            if current is None:
                continue
            match = PLANE_RE.fullmatch(line)
            if match:
                values = [int(value) for value in match.groups()]
                plane = values[0]
                current["planes"].setdefault(plane, {}).update({
                    "plane": plane,
                    "pixels": values[1],
                    "depth": values[2],
                    "tile_log": values[3],
                    "current_proxy": values[4],
                    "pixel_proxy": values[5],
                    "current_regret": values[6],
                    "current_equals_pixel_id": values[7],
                    "current_equals_pixel_cost": values[8],
                    "purity_majority": values[9],
                    "purity_total": values[10],
                })
                continue
            match = RANK_RE.fullmatch(line)
            if match:
                plane = int(match.group(1))
                current["planes"].setdefault(plane, {})["rank_counts"] = match.group(2)
                continue
            match = SCALE_RE.fullmatch(line)
            if match:
                plane = int(match.group(1))
                scale = match.group(2)
                current["scales"][(plane, scale)] = {
                    "plane": plane,
                    "scale": scale,
                    "size": int(match.group(3)),
                    "regions": int(match.group(4)),
                    "proxy": int(match.group(5)),
                    "selector_entropy_bits": float(match.group(6)),
                    "second_gap": int(match.group(7)),
                    "horizontal_transitions": int(match.group(8)),
                    "horizontal_pairs": int(match.group(9)),
                    "vertical_transitions": int(match.group(10)),
                    "vertical_pairs": int(match.group(11)),
                    "runs": int(match.group(12)),
                }
                continue
            match = CHOICES_RE.fullmatch(line)
            if match:
                key = (int(match.group(1)), match.group(2))
                current["scales"].setdefault(key, {})["choice_counts"] = match.group(3)
                continue
            match = QST_PLANE_RE.fullmatch(line)
            if match:
                values = [int(value) for value in match.groups()]
                plane = values[0]
                current["planes"].setdefault(plane, {}).update({
                    "range_bytes": values[4],
                    "map_bytes": values[5],
                })
                continue
            match = QST_COST_RE.fullmatch(line)
            if match:
                values = [int(value) for value in match.groups()]
                plane = values[0]
                names = [
                    "map_decisions", "map_renormalizations", "zero_decisions",
                    "zero_renormalizations", "magnitude_decisions",
                    "magnitude_renormalizations", "tail_decisions",
                    "tail_renormalizations", "sign_decisions",
                    "sign_renormalizations",
                ]
                current["planes"].setdefault(plane, {}).update(
                    dict(zip(names, values[1:]))
                )
    return records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--files", required=True, type=Path)
    parser.add_argument("--modes", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--comparison", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    files = {row["Path"]: row for row in read_csv(args.files)}
    modes = {row["Path"]: row for row in read_csv(args.modes)}
    comparison = (
        {row["image_id"]: row for row in read_csv(args.comparison)}
        if args.comparison is not None
        else {}
    )
    records = parse_trace(args.trace)
    plane_rows = []
    scale_rows = []
    for record in records:
        path = record["path"]
        source = files[path]
        mode = modes[path]
        for plane, row in sorted(record["planes"].items()):
            if "range_bytes" not in row or "rank_counts" not in row:
                raise ValueError(f"incomplete plane {plane} for {path}")
            merged = {
                "path": path,
                "source_family": source["SourceGroup"],
                "category": source["Category"],
                "transform": mode["NativeTransform"],
                "qlic_bytes": source["QlicBytes"],
                "jxl9_bytes": source["Jxl9Bytes"],
                "delta_to_jxl9": source["DeltaBytes"],
                "image_size": image_size(int(source["Pixels"])),
                **row,
            }
            merged["exact_entropy_bits"] = row["range_bytes"] * 8
            merged["residual_range_bytes"] = row["range_bytes"] - row["map_bytes"]
            merged["current_equals_pixel_id_rate"] = row["current_equals_pixel_id"] / row["pixels"]
            merged["current_equals_pixel_cost_rate"] = row["current_equals_pixel_cost"] / row["pixels"]
            merged["predictor_purity"] = row["purity_majority"] / row["purity_total"]
            merged["regret_proxy_per_pixel"] = row["current_regret"] / row["pixels"]
            merged["range_decisions"] = sum(
                row[name] for name in (
                    "map_decisions", "zero_decisions", "magnitude_decisions",
                    "tail_decisions", "sign_decisions",
                )
            )
            merged["range_renormalizations"] = sum(
                row[name] for name in (
                    "map_renormalizations", "zero_renormalizations",
                    "magnitude_renormalizations", "tail_renormalizations",
                    "sign_renormalizations",
                )
            )
            plane_rows.append(merged)
        for (plane, scale), row in sorted(record["scales"].items()):
            if "choice_counts" not in row:
                raise ValueError(f"missing choices for {path} plane {plane} {scale}")
            baseline = record["planes"][plane]
            merged = {
                "path": path,
                "source_family": source["SourceGroup"],
                "category": source["Category"],
                "transform": mode["NativeTransform"],
                "qlic_bytes": source["QlicBytes"],
                "jxl9_bytes": source["Jxl9Bytes"],
                "delta_to_jxl9": source["DeltaBytes"],
                "image_size": image_size(int(source["Pixels"])),
                "current_proxy": baseline["current_proxy"],
                "current_map_bits": baseline["map_bytes"] * 8,
                **row,
            }
            merged["free_selector_saving_proxy"] = baseline["current_proxy"] - row["proxy"]
            merged["free_selector_saving_percent"] = (
                100.0 * merged["free_selector_saving_proxy"] / baseline["current_proxy"]
            )
            merged["selector_bits_per_region"] = row["selector_entropy_bits"] / row["regions"]
            merged["horizontal_transition_rate"] = (
                row["horizontal_transitions"] / row["horizontal_pairs"]
                if row["horizontal_pairs"] else 0.0
            )
            merged["vertical_transition_rate"] = (
                row["vertical_transitions"] / row["vertical_pairs"]
                if row["vertical_pairs"] else 0.0
            )
            merged["mean_horizontal_run"] = row["regions"] / row["runs"]
            scale_rows.append(merged)
    plane_fields = [
        "path", "source_family", "category", "transform", "image_size", "qlic_bytes",
        "jxl9_bytes", "delta_to_jxl9", "plane", "pixels", "depth",
        "tile_log", "range_bytes", "map_bytes", "residual_range_bytes",
        "exact_entropy_bits", "range_decisions", "range_renormalizations",
        "map_decisions", "map_renormalizations", "zero_decisions",
        "zero_renormalizations", "magnitude_decisions",
        "magnitude_renormalizations", "tail_decisions",
        "tail_renormalizations", "sign_decisions", "sign_renormalizations",
        "current_proxy", "pixel_proxy", "current_regret",
        "regret_proxy_per_pixel", "current_equals_pixel_id",
        "current_equals_pixel_id_rate", "current_equals_pixel_cost",
        "current_equals_pixel_cost_rate", "purity_majority", "purity_total",
        "predictor_purity", "rank_counts",
    ]
    scale_fields = [
        "path", "source_family", "category", "transform", "image_size", "qlic_bytes",
        "jxl9_bytes", "delta_to_jxl9", "plane", "scale", "size",
        "regions", "current_proxy", "proxy", "free_selector_saving_proxy",
        "free_selector_saving_percent", "selector_entropy_bits",
        "selector_bits_per_region", "current_map_bits", "second_gap",
        "horizontal_transitions", "horizontal_pairs",
        "horizontal_transition_rate", "vertical_transitions", "vertical_pairs",
        "vertical_transition_rate", "runs", "mean_horizontal_run",
        "choice_counts",
    ]
    write_csv(args.output / "planes.csv", plane_fields, plane_rows)
    write_csv(args.output / "scales.csv", scale_fields, scale_rows)
    planes_by_path: dict[str, list[dict[str, object]]] = defaultdict(list)
    for row in plane_rows:
        planes_by_path[str(row["path"])].append(row)
    file_rows = []
    for path, rows_for_path in sorted(planes_by_path.items()):
        source = files[path]
        compared = comparison.get(path, {})
        rank_counts = [0] * 32
        for row in rows_for_path:
            for index, count in enumerate(str(row["rank_counts"]).split(",")):
                rank_counts[index] += int(count)
        plane_samples = sum(int(row["pixels"]) for row in rows_for_path)
        current_regret = sum(int(row["current_regret"]) for row in rows_for_path)
        jxl8 = int(compared["jxl_effort_8_bytes"]) if compared else ""
        jxl9 = int(source["Jxl9Bytes"])
        file_rows.append({
            "path": path,
            "source_family": source["SourceGroup"],
            "category": source["Category"],
            "transform": modes[path]["NativeTransform"],
            "image_size": image_size(int(source["Pixels"])),
            "image_pixels": int(source["Pixels"]),
            "planes": len(rows_for_path),
            "plane_samples": plane_samples,
            "qlic_file_bytes": int(source["QlicBytes"]),
            "jxl8_file_bytes": jxl8,
            "jxl9_file_bytes": jxl9,
            "jxl8_to_9_gain_bytes": jxl8 - jxl9 if compared else "",
            "qlic_minus_jxl9_bytes": int(source["DeltaBytes"]),
            "exact_range_bytes": sum(int(row["range_bytes"]) for row in rows_for_path),
            "exact_map_bytes": sum(int(row["map_bytes"]) for row in rows_for_path),
            "exact_residual_range_bytes": sum(
                int(row["residual_range_bytes"]) for row in rows_for_path
            ),
            "range_decisions": sum(
                int(row["range_decisions"]) for row in rows_for_path
            ),
            "range_renormalizations": sum(
                int(row["range_renormalizations"]) for row in rows_for_path
            ),
            "current_proxy": sum(int(row["current_proxy"]) for row in rows_for_path),
            "pixel_proxy": sum(int(row["pixel_proxy"]) for row in rows_for_path),
            "current_regret": current_regret,
            "regret_proxy_per_plane_sample": current_regret / plane_samples,
            "rank_counts": ",".join(str(value) for value in rank_counts),
        })
    file_fields = [
        "path", "source_family", "category", "transform", "image_size",
        "image_pixels", "planes", "plane_samples", "qlic_file_bytes",
        "jxl8_file_bytes", "jxl9_file_bytes", "jxl8_to_9_gain_bytes",
        "qlic_minus_jxl9_bytes", "exact_range_bytes", "exact_map_bytes",
        "exact_residual_range_bytes", "range_decisions",
        "range_renormalizations", "current_proxy", "pixel_proxy",
        "current_regret", "regret_proxy_per_plane_sample", "rank_counts",
    ]
    write_csv(args.output / "files.csv", file_fields, file_rows)
    aggregates = {}
    for scale in ["plane", "64x64", "32x32", "16x16", "8x8", "4x4", "2x2", "pixel"]:
        selected = [row for row in scale_rows if row["scale"] == scale]
        current = sum(row["current_proxy"] for row in selected)
        oracle = sum(row["proxy"] for row in selected)
        aggregates[scale] = {
            "planes": len(selected),
            "regions": sum(row["regions"] for row in selected),
            "current_proxy": current,
            "oracle_proxy": oracle,
            "free_selector_saving_proxy": current - oracle,
            "free_selector_saving_percent": 100.0 * (current - oracle) / current,
            "selector_entropy_bits": sum(row["selector_entropy_bits"] for row in selected),
            "current_map_bits": sum(row["current_map_bits"] for row in selected),
            "horizontal_transition_rate": (
                sum(row["horizontal_transitions"] for row in selected) /
                sum(row["horizontal_pairs"] for row in selected)
                if sum(row["horizontal_pairs"] for row in selected) else 0.0
            ),
            "vertical_transition_rate": (
                sum(row["vertical_transitions"] for row in selected) /
                sum(row["vertical_pairs"] for row in selected)
                if sum(row["vertical_pairs"] for row in selected) else 0.0
            ),
            "mean_horizontal_run": (
                sum(row["regions"] for row in selected)
                / sum(row["runs"] for row in selected)
                if sum(row["runs"] for row in selected)
                else 0.0
            ),
            "second_best_gap_proxy": sum(row["second_gap"] for row in selected),
        }
    by_source = {}
    for source in sorted({row["source_family"] for row in scale_rows}):
        by_source[source] = {}
        for scale in aggregates:
            selected = [
                row for row in scale_rows
                if row["source_family"] == source and row["scale"] == scale
            ]
            current = sum(row["current_proxy"] for row in selected)
            oracle = sum(row["proxy"] for row in selected)
            by_source[source][scale] = {
                "current_proxy": current,
                "oracle_proxy": oracle,
                "free_selector_saving_percent": (
                    100.0 * (current - oracle) / current if current else 0.0
                ),
            }
    group_rows = []
    dimensions = {
        "source_family": lambda row: str(row["source_family"]),
        "transform": lambda row: str(row["transform"]),
        "plane": lambda row: str(row["plane"]),
        "image_size": lambda row: str(row["image_size"]),
    }
    for dimension, key in dimensions.items():
        groups = sorted({key(row) for row in scale_rows})
        for group in groups:
            for scale in aggregates:
                selected = [
                    row for row in scale_rows
                    if key(row) == group and row["scale"] == scale
                ]
                current = sum(row["current_proxy"] for row in selected)
                oracle = sum(row["proxy"] for row in selected)
                group_rows.append({
                    "dimension": dimension,
                    "group": group,
                    "scale": scale,
                    "planes": len(selected),
                    "plane_samples": sum(
                        int(plane["pixels"])
                        for plane in plane_rows
                        if str(plane[dimension]) == group
                    ) if dimension in plane_rows[0] else "",
                    "regions": sum(row["regions"] for row in selected),
                    "current_proxy": current,
                    "oracle_proxy": oracle,
                    "free_selector_saving_proxy": current - oracle,
                    "free_selector_saving_percent": (
                        100.0 * (current - oracle) / current if current else 0.0
                    ),
                    "selector_entropy_bits": sum(
                        row["selector_entropy_bits"] for row in selected
                    ),
                })
    write_csv(
        args.output / "groups.csv",
        [
            "dimension", "group", "scale", "planes", "plane_samples",
            "regions", "current_proxy", "oracle_proxy",
            "free_selector_saving_proxy", "free_selector_saving_percent",
            "selector_entropy_bits",
        ],
        group_rows,
    )
    pixels_across_planes = sum(row["pixels"] for row in plane_rows)
    total_decisions = sum(row["range_decisions"] for row in plane_rows)
    decision_classes = {}
    for name in ("map", "zero", "magnitude", "tail", "sign"):
        decisions = sum(row[f"{name}_decisions"] for row in plane_rows)
        renormalizations = sum(
            row[f"{name}_renormalizations"] for row in plane_rows
        )
        decision_classes[name] = {
            "decisions": decisions,
            "renormalizations": renormalizations,
            "decisions_per_plane_sample": decisions / pixels_across_planes,
            "renormalizations_per_plane_sample": (
                renormalizations / pixels_across_planes
            ),
            "decision_share_percent": 100.0 * decisions / total_decisions,
        }
    rank_counts = [0] * 32
    for row in plane_rows:
        for index, count in enumerate(str(row["rank_counts"]).split(",")):
            rank_counts[index] += int(count)
    correlations = {}
    compared_files = [row for row in file_rows if row["jxl8_file_bytes"] != ""]
    if compared_files:
        correlations = {
            "predictor_regret_vs_qlic_minus_jxl9": pearson(
                [int(row["current_regret"]) for row in compared_files],
                [int(row["qlic_minus_jxl9_bytes"]) for row in compared_files],
            ),
            "predictor_regret_vs_jxl8_to_9_gain": pearson(
                [int(row["current_regret"]) for row in compared_files],
                [int(row["jxl8_to_9_gain_bytes"]) for row in compared_files],
            ),
            "jxl8_to_9_gain_vs_qlic_minus_jxl9": pearson(
                [int(row["jxl8_to_9_gain_bytes"]) for row in compared_files],
                [int(row["qlic_minus_jxl9_bytes"]) for row in compared_files],
            ),
        }
    summary = {
        "schema": 1,
        "files": len(records),
        "planes": len(plane_rows),
        "pixels_across_planes": pixels_across_planes,
        "qlic_file_bytes": sum(int(files[record["path"]]["QlicBytes"]) for record in records),
        "jxl9_file_bytes": sum(int(files[record["path"]]["Jxl9Bytes"]) for record in records),
        "exact_range_bytes": sum(row["range_bytes"] for row in plane_rows),
        "exact_map_bytes": sum(row["map_bytes"] for row in plane_rows),
        "exact_residual_range_bytes": sum(row["residual_range_bytes"] for row in plane_rows),
        "range_decisions": total_decisions,
        "range_renormalizations": sum(
            row["range_renormalizations"] for row in plane_rows
        ),
        "decision_classes": decision_classes,
        "current_predictor_rank_counts": rank_counts,
        "current_predictor_equals_pixel_oracle_id_rate": (
            sum(row["current_equals_pixel_id"] for row in plane_rows) /
            sum(row["pixels"] for row in plane_rows)
        ),
        "current_predictor_equals_pixel_oracle_cost_rate": (
            sum(row["current_equals_pixel_cost"] for row in plane_rows) /
            sum(row["pixels"] for row in plane_rows)
        ),
        "predictor_purity_in_current_tiles": (
            sum(row["purity_majority"] for row in plane_rows) /
            sum(row["purity_total"] for row in plane_rows)
        ),
        "scales": aggregates,
        "by_source": by_source,
        "correlations": correlations,
        "tables": {
            "files": "files.csv",
            "planes": "planes.csv",
            "scales": "scales.csv",
            "groups": "groups.csv",
        },
        "limits": {
            "oracle_type": "fixed residual proxy over the existing 32 predictors",
            "free_selector": True,
            "signaled_measurement": "zero-order selector entropy reported separately; no syntax is assumed",
            "causal_entropy_replay": False,
            "cross_plane_state": "baseline decoded state",
            "exact_entropy_bits_field": (
                "exact compressed range bytes multiplied by eight; not an "
                "ideal Shannon-entropy estimate"
            ),
        },
    }
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
