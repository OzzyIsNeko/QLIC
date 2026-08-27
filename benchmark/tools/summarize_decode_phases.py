#!/usr/bin/env python3
"""Summarize the QLIC user-mode decoder phase trace."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path


PHASE_RE = re.compile(
    r"^decode-phase name=(?P<name>[^ ]+)"
    r"(?: index=(?P<index>\d+))? mode=(?P<mode>\d+) "
    r"transform=(?P<transform>\d+) pixels=(?P<pixels>\d+) "
    r"seconds=(?P<seconds>[0-9.]+) err=(?P<error>\d+)$"
)
QST2_RE = re.compile(
    r"^qst2-cost plane=(?P<plane>\d+) "
    r"map-decisions=(?P<map_decisions>\d+) "
    r"map-renormalizations=(?P<map_renormalizations>\d+) "
    r"zero-decisions=(?P<zero_decisions>\d+) "
    r"zero-renormalizations=(?P<zero_renormalizations>\d+) "
    r"magnitude-decisions=(?P<magnitude_decisions>\d+) "
    r"magnitude-renormalizations=(?P<magnitude_renormalizations>\d+) "
    r"tail-decisions=(?P<tail_decisions>\d+) "
    r"tail-renormalizations=(?P<tail_renormalizations>\d+) "
    r"sign-decisions=(?P<sign_decisions>\d+) "
    r"sign-renormalizations=(?P<sign_renormalizations>\d+)$"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--discovery", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--memory", type=Path)
    parser.add_argument("--skip-groups", type=int, default=0)
    parser.add_argument("--processor", type=int, required=True)
    args = parser.parse_args()

    with args.discovery.open(newline="", encoding="utf-8-sig") as handle:
        discovery = list(csv.DictReader(handle))
    selected = []
    for family in sorted({row["SourceGroup"] for row in discovery}):
        rows = [row for row in discovery if row["SourceGroup"] == family]
        selected.append(max(rows, key=lambda row: int(row["Pixels"])))
    by_pixels = {int(row["Pixels"]): row for row in selected}
    if len(by_pixels) != len(selected):
        raise SystemExit("selected source representatives do not have unique sizes")

    groups: list[tuple[list[dict[str, object]], list[dict[str, int]]]] = []
    current: list[dict[str, object]] = []
    current_costs: list[dict[str, int]] = []
    with args.trace.open(encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, 1):
            clean_line = line.rstrip("\r\n")
            cost_match = QST2_RE.match(clean_line)
            if cost_match:
                current_costs.append(
                    {key: int(value) for key, value in cost_match.groupdict().items()}
                )
                continue
            match = PHASE_RE.match(clean_line)
            if not match:
                continue
            phase: dict[str, object] = {
                "name": match["name"],
                "index": int(match["index"]) if match["index"] else None,
                "mode": int(match["mode"]),
                "transform": int(match["transform"]),
                "pixels": int(match["pixels"]),
                "seconds": float(match["seconds"]),
                "error": int(match["error"]),
                "line": line_number,
            }
            current.append(phase)
            if phase["name"] == "total":
                groups.append((current, current_costs))
                current = []
                current_costs = []
    if current or current_costs:
        raise SystemExit("trace ended inside a decode group")
    if args.skip_groups < 0 or args.skip_groups >= len(groups):
        raise SystemExit("invalid skip-groups value")
    measured = groups[args.skip_groups :]

    rows = []
    phase_seconds: defaultdict[str, float] = defaultdict(float)
    phase_counts: Counter[str] = Counter()
    total_seconds = 0.0
    total_pixels = 0
    source_seconds: defaultdict[str, float] = defaultdict(float)
    source_pixels: defaultdict[str, int] = defaultdict(int)
    source_decodes: Counter[str] = Counter()
    decision_classes = ["map", "zero", "magnitude", "tail", "sign"]
    decisions: Counter[str] = Counter()
    renormalizations: Counter[str] = Counter()
    plane_samples = 0
    for group_index, (group, costs) in enumerate(measured):
        if any(int(phase["error"]) for phase in group):
            raise SystemExit(f"decoder error in measured group {group_index}")
        total = [phase for phase in group if phase["name"] == "total"]
        if len(total) != 1:
            raise SystemExit(f"bad total count in measured group {group_index}")
        total_phase = total[0]
        pixels = int(total_phase["pixels"])
        source = by_pixels.get(pixels)
        if source is None:
            raise SystemExit(f"unmapped representative pixel count: {pixels}")
        family = source["SourceGroup"]
        if costs and len(costs) != 3:
            raise SystemExit(
                f"expected three QST2 plane costs in measured group {group_index}"
            )
        for cost in costs:
            plane_samples += pixels
            for name in decision_classes:
                decisions[name] += cost[f"{name}_decisions"]
                renormalizations[name] += cost[f"{name}_renormalizations"]
        component_seconds = 0.0
        for phase in group:
            if phase["name"] == "total":
                continue
            name = str(phase["name"])
            if phase["index"] is not None:
                name += f"-{phase['index']}"
            seconds = float(phase["seconds"])
            phase_seconds[name] += seconds
            phase_counts[name] += 1
            component_seconds += seconds
        seconds = float(total_phase["seconds"])
        unaccounted = seconds - component_seconds
        if unaccounted < -1e-6:
            raise SystemExit(f"negative unaccounted time in group {group_index}")
        phase_seconds["unaccounted"] += max(0.0, unaccounted)
        phase_counts["unaccounted"] += 1
        total_seconds += seconds
        total_pixels += pixels
        source_seconds[family] += seconds
        source_pixels[family] += pixels
        source_decodes[family] += 1
        rows.append(
            {
                "group": group_index,
                "source_family": family,
                "path": source["Path"],
                "stream": source["Stream"],
                "pixels": pixels,
                "mode": total_phase["mode"],
                "transform": total_phase["transform"],
                "seconds": seconds,
                "nanoseconds_per_pixel": seconds * 1e9 / pixels,
                "unaccounted_seconds": max(0.0, unaccounted),
            }
        )

    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "decodes.csv").open(
        "w", newline="", encoding="utf-8"
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    phases = [
        {
            "phase": phase,
            "calls": phase_counts[phase],
            "seconds": seconds,
            "share_percent": 100.0 * seconds / total_seconds,
            "nanoseconds_per_decoded_pixel": seconds * 1e9 / total_pixels,
        }
        for phase, seconds in sorted(
            phase_seconds.items(), key=lambda item: item[1], reverse=True
        )
    ]
    sources = [
        {
            "source_family": family,
            "decodes": source_decodes[family],
            "decoded_pixels": source_pixels[family],
            "seconds": source_seconds[family],
            "nanoseconds_per_pixel": source_seconds[family]
            * 1e9
            / source_pixels[family],
        }
        for family in sorted(source_seconds)
    ]
    entropy_seconds = sum(
        seconds for phase, seconds in phase_seconds.items() if phase.startswith("plane-")
    )
    total_decisions = sum(decisions.values())
    total_renormalizations = sum(renormalizations.values())
    range_cost = {
        "present": bool(total_decisions),
        "plane_samples": plane_samples,
        "decisions": total_decisions,
        "renormalizations": total_renormalizations,
        "decisions_per_plane_sample": (
            total_decisions / plane_samples if plane_samples else 0.0
        ),
        "renormalizations_per_plane_sample": (
            total_renormalizations / plane_samples if plane_samples else 0.0
        ),
        "decisions_per_output_pixel": (
            total_decisions / total_pixels if total_pixels else 0.0
        ),
        "renormalizations_per_output_pixel": (
            total_renormalizations / total_pixels if total_pixels else 0.0
        ),
        "entropy_nanoseconds_per_decision": (
            entropy_seconds * 1e9 / total_decisions if total_decisions else 0.0
        ),
        "classes": {
            name: {
                "decisions": decisions[name],
                "renormalizations": renormalizations[name],
                "decision_share_percent": (
                    100.0 * decisions[name] / total_decisions
                    if total_decisions
                    else 0.0
                ),
                "renormalization_share_percent": (
                    100.0 * renormalizations[name] / total_renormalizations
                    if total_renormalizations
                    else 0.0
                ),
            }
            for name in decision_classes
        },
    }
    memory = {"present": False}
    if args.memory is not None:
        with args.memory.open(newline="", encoding="utf-8-sig") as handle:
            memory_rows = list(csv.DictReader(handle))
        if not memory_rows:
            raise SystemExit("memory table is empty")
        numeric_fields = [
            "BaselinePeakWorkingSetBytes",
            "CandidatePeakWorkingSetBytes",
            "DeltaPeakWorkingSetBytes",
            "BaselinePeakPrivateBytes",
            "CandidatePeakPrivateBytes",
            "DeltaPeakPrivateBytes",
        ]
        values = {
            field: [int(row[field]) for row in memory_rows] for field in numeric_fields
        }
        paths = sorted({row["Path"] for row in memory_rows})
        repetitions = sorted({int(row["Repetition"]) for row in memory_rows})
        memory = {
            "present": True,
            "path": str(args.memory.resolve()),
            "sha256": sha256(args.memory),
            "files": len(paths),
            "samples": len(memory_rows),
            "repetitions_per_file": len(repetitions),
            "baseline_max_peak_working_set_bytes": max(
                values["BaselinePeakWorkingSetBytes"]
            ),
            "candidate_max_peak_working_set_bytes": max(
                values["CandidatePeakWorkingSetBytes"]
            ),
            "max_peak_working_set_bytes": max(
                values["BaselinePeakWorkingSetBytes"]
                + values["CandidatePeakWorkingSetBytes"]
            ),
            "baseline_max_peak_private_bytes": max(
                values["BaselinePeakPrivateBytes"]
            ),
            "candidate_max_peak_private_bytes": max(
                values["CandidatePeakPrivateBytes"]
            ),
            "max_peak_private_bytes": max(
                values["BaselinePeakPrivateBytes"]
                + values["CandidatePeakPrivateBytes"]
            ),
            "median_working_set_delta_bytes": statistics.median(
                values["DeltaPeakWorkingSetBytes"]
            ),
            "median_private_delta_bytes": statistics.median(
                values["DeltaPeakPrivateBytes"]
            ),
            "maximum_absolute_working_set_delta_bytes": max(
                abs(value) for value in values["DeltaPeakWorkingSetBytes"]
            ),
            "maximum_absolute_private_delta_bytes": max(
                abs(value) for value in values["DeltaPeakPrivateBytes"]
            ),
            "paths": paths,
            "meaning": (
                "separate-process peak memory for exact baseline/candidate pair "
                "decodes; the paired streams are identical in this control"
            ),
        }
    summary = {
        "schema": 1,
        "trace_groups": len(groups),
        "skipped_unpinned_verification_groups": args.skip_groups,
        "measured_pinned_groups": len(measured),
        "processor": args.processor,
        "all_exact": True,
        "decoded_pixels": total_pixels,
        "total_seconds": total_seconds,
        "nanoseconds_per_pixel": total_seconds * 1e9 / total_pixels,
        "phases": phases,
        "range_cost": range_cost,
        "memory": memory,
        "sources": sources,
        "trace": str(args.trace.resolve()),
        "trace_sha256": sha256(args.trace),
        "binary": str(args.binary.resolve()),
        "binary_sha256": sha256(args.binary),
        "static_library": str(args.library.resolve()),
        "static_library_sha256": sha256(args.library),
        "discovery": str(args.discovery.resolve()),
        "discovery_sha256": sha256(args.discovery),
        "hardware_counters": {
            "status": "unavailable",
            "reason": "xperf could not start NT Kernel Logger: Access is denied (0x5)",
            "available_sources": [
                "InstructionRetired",
                "TotalCycles",
                "BranchInstructions",
                "BranchMispredictions",
                "DcacheAccesses",
                "DcacheMisses",
            ],
            "values_estimated": False,
        },
        "limits": {
            "timer": "QueryPerformanceCounter around exact decoder phases",
            "timing_scope": "inner native stream decode",
            "representatives": "largest discovery file from each visible source family",
            "verification_groups": (
                "the pair harness verifies each stream before pinning; those groups "
                "are omitted"
            ),
            "pmc": "kernel privilege was not available; no branch or cache values are claimed",
        },
    }
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
