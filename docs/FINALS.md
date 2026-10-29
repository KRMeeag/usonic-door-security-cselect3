# Finals Scope — Door Intrusion Detection

`PRD.md` is **frozen**: it is the record of what was built and presented for the
midterm, and the paper cites it as the baseline. This file is the finals scope.
Where the two disagree, this file wins.

## Geometry — settled, do not re-open

Three ultrasonic sensors sit **on top of the door frame at roughly 45 degrees, aimed
down at the door leaf**.

- **Closed** — each beam clears the leaf and lands on whatever is beyond it, so every
  sensor reads long.
- **Opening** — the leaf swings into the beam and the reading **drops into the 0-20 cm
  trip band**.

`DOOR_OPEN_IS_FARTHER = false` is correct and the system works this way today.
`THRESH_DEFAULT_CM` is 20 cm to match; the V5 slider overrides it on connect.
PRD.md has been corrected to match — any older description of a jamb mount is gone.

## Validation rig

Testing and the demo use a **hand-held cardboard panel standing in for the door leaf**,
as at the midterm demo. This matters for how results are framed: the length of time a
beam reads open is set by how fast the operator sweeps the panel, so it is a
**controlled input, not an observed property of the system**.

The battery therefore runs two positive classes:

- **fast** — the panel is swept *through* the beams and kept moving, the way a briskly
  opened door crosses them.
- **held** — the panel is brought in and held for ~2 s. The easy case.

**What Tier 3 is for.** The midterm rule needs all three sensors reading open
simultaneously for `PERSIST_N = 3` consecutive 300 ms cycles — roughly **600-700 ms of
continuous agreement**. A held panel supplies that comfortably; a fast sweep may not.
If detection rate drops on the fast class, that gap is a controlled, repeatable result
and it is exactly what the coincidence window is built to close. If both classes score
equally well, Tier 3 has to be justified on false positives instead — and the baseline
run is what settles which. Do not assert the gap before measuring it.

## Tiers

Work is sequenced so a later tier can be dropped without unpicking an earlier
one. Each completed tier is tagged.

| Tier | Contains | Requirements covered | Tag |
| --- | --- | --- | --- |
| 1 | Baseline tagging, CSV logging, analysis script, Blynk console features | 1, 3, 4 | `tier1` |
| 2 | Desk-verifiable safeguards and dashboard controls | 5, part of 3 | `tier2` |
| 3 | Coincidence window, risk score | 2, 7 | `tier3` |
| — | Test battery, run against `midterm` and against the final build | 6 | — |

`midterm` tags the firmware as presented, and is the baseline the final numbers
are compared against.

## Tier 1 — delivered

- **Frozen CSV log schema** (`LOG_SCHEMA = 1`). `#D` telemetry samples once per
  cycle and `#E` system events, interleaved with the existing human-readable
  output on the same serial stream. Fields that only become meaningful in Tier 3
  (`tmask`, `risk`) are present from the start and read 0 until then, so the
  baseline capture and the final capture parse identically. **Append only; never
  reorder.**
- **One-tap test trials**, on Blynk buttons **V8** (door-opening trial) and
  **V9** (disturbance trial), and on serial `1` / `0`. A press clears any latch
  from the previous trial, re-arms, logs the ground-truth marker and chirps two
  high pips for "go". `tools/analyze.py` pairs each marker with what the system
  then did, producing the confusion matrix with no hand labelling.
  This is deliberately a single action so the whole battery can be run **solo
  from the phone at the door** — you cannot press a laptop key and open the door
  at the same moment.
- **`tools/analyze.py`** — summary statistics, tidy CSV export, and the paper's
  figures. Parsing and statistics are stdlib; matplotlib is needed only for
  figures.
- **`tools/make_selftest_log.py`** — synthetic capture so the analysis can be
  verified with no hardware. A correct run reports 7 TP / 3 FN / 0 FP / 8 TN.

## Capturing a run

```sh
pio device monitor -f log2file          # writes into logs/
python3 tools/analyze.py logs/*.log     # summary + data/*.csv + docs/figures/
```

`logs/` and `data/raw/` are gitignored; `data/*.csv` and `docs/figures/` are the
committed dataset and figures.

## Honesty constraints

Carried into the paper, and not negotiable:

- The midterm's false-positive rate was **never measured** — PRD §14 says so.
  Any claim of improvement needs the `midterm` tag run through the same battery.
- Dip durations shorter than one sampling period are reported at the measurement
  floor, not as real durations.
- `tools/make_selftest_log.py` output is synthetic and must never appear as a
  result.
