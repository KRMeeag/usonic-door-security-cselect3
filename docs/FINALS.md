# Finals Scope — Door Intrusion Detection

`PRD.md` is **frozen**: it is the record of what was built and presented for the
midterm, and the paper cites it as the baseline. This file is the finals scope.
Where the two disagree, this file wins.

## Correction to PRD §8.1 — mounting geometry

The PRD describes the three ultrasonic sensors as mounted on the jamb, stacked
vertically ~50–60 cm apart at knee/chest/head height, angled across the doorway.
**That is not how the prototype is built.** The sensors sit **on top of the door
frame at roughly 45°, aimed down at the door leaf.**

The *signal direction* the PRD describes is still correct, and
`DOOR_OPEN_IS_FARTHER = false` is the right setting:

- **Closed** — the beam clears the leaf and lands on the nearest wall or the
  floor, so each sensor reads long (typically a few metres, or the
  `DIST_MAX_CM` ≈ 431 cm ceiling if nothing is in range).
- **Opening** — the swinging leaf cuts into the beam and the reading **drops**
  sharply, to roughly where the leaf passes.

One consequence drives most of the finals work. Because the leaf *sweeps
through* the beam rather than coming to rest in it, each sensor sees a
**transient dip**, not a sustained state. The midterm rule requires all three
sensors to read open simultaneously for `PERSIST_N = 3` consecutive 300 ms
cycles — roughly **600–700 ms of continuous agreement**. A briskly opened door
may not hold the beams that long, and because the three sensors sit along the
top of the frame the leaf crosses them *sequentially*, so they may never all
read open inside the same cycle.

Whether this actually costs detections is an empirical question, and Tier 1
exists partly to answer it: `tools/analyze.py` reports measured dip durations
against that ~700 ms bar.

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
- **Ground-truth trial markers.** `1` before a door-opening trial, `0` before a
  disturbance trial. The firmware chirps so whoever is at the door hears the
  marker register. `tools/analyze.py` pairs each marker with what the system
  then did, which produces the confusion matrix with no hand labelling.
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
