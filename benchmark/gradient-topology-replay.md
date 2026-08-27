# Gradient-topology replay

This benchmark-only experiment asks whether mode 53 can make its zero and
first magnitude decisions more conditionally sparse without a learned tree,
stored context map, new predictor, or production-format change.

The production activity class sums absolute gradients. The replay retains four
additional causal geometry bits from neighbors that are already available to
the decoder:

1. the horizontal slope reverses around the north sample;
2. the horizontal and vertical slopes at the northwest corner oppose;
3. horizontal energy is greater than vertical energy;
4. either axis reverses relative to its second-order sample.

The signature is unchanged by global sample inversion. Its low bits define
nested 4-, 8-, and 16-class candidates; class 1 is the unchanged baseline.
Gradient magnitude remains in QLIC's existing activity context.

## Measurements

For the zero/nonzero and first magnitude decisions, every plane reports:

- ideal conditional entropy, which is an optimistic oracle;
- causal Krichevsky--Trofimov (KT) bits, which show the cost of naively
  duplicating every detailed context;
- a small activity/predictor-family topology bank blended with the live QLIC
  probability at fixed weights from 1/8 through 6/8;
- the live QLIC probability cost and native range bytes for scale.

The reducer requires a frozen `Discovery`/`Holdout` column. A candidate advances
only when its ideal decision saving is at least 2% on both splits and its shared
causal replay is nonnegative for every represented source family. Class count
and blend weight are selected using discovery only, then applied unchanged to
holdout. Passing permits a bounded decoder prototype, not production approval.
Decode speed, memory, exact completed bytes, and the full qualification corpus
remain later gates.

## Frozen 114-stream pass

The 2026-08-25 pass decoded all 114 retained mode-53 streams: 55 discovery
files and 59 holdout files, totaling 115,299,160 plane samples. The 8-class
mapping exposed 2.5559% discovery and 2.6356% holdout ideal decision savings;
the 16-class mapping exposed 3.9025% and 3.9807%. The signal is real, but its
available coding forms increased modeled size:

- naive 8-class context duplication grew modeled range by 0.4735% on discovery
  and 0.3968% on holdout;
- the lightest 8-class shared blend grew modeled range by 0.0064% on discovery
  and 0.0311% on holdout;
- the closest result overall, the lightest 4-class blend, still grew by 0.0039%
  and 0.0295% and did not reach the 2% oracle gate.

Verdict: reject this signature and shared-bank formulation. It does not justify
a decoder prototype or a wire identifier. The replay remains useful for testing
a different bounded topology mapping against the same gates.

## Build and run

Configure the benchmark target without changing ordinary codec targets:

```powershell
cmake -S . -B build/gradient-topology `
  -DQLIC_BENCHMARK_TRIAL=ON -DBUILD_TESTING=ON
cmake --build build/gradient-topology `
  --target qlic-gradient-topology-replay
```

The manifest contains one absolute mode-53 `.qlic` path per line. The CSV rows
must be in the same order and contain `Stream`, `Category`, `Split`, and
`ExpectedBytes` columns:

```powershell
build/gradient-topology/qlic-gradient-topology-replay.exe `
  0 streams.txt topology.trace
python benchmark/tools/summarize_gradient_topology.py `
  --trace topology.trace --files selection.csv --out topology-results
```

The reducer writes `files.csv`, `sources.csv`, `summary.json`, and `RESULTS.md`.
The trace executable rejects streams whose native mode is not 53 and verifies
every decoded stream before accepting its measurements.
