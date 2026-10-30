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
| 4 | Non-blocking ping scheduler, interrupt sound capture, DHT11 speed-of-sound compensation | 5 | not yet — tag after the bench check |
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

## Tier 2 — delivered

Eight safeguards, all desk-verifiable. Two close genuine security holes found by
auditing the midterm firmware.

| # | Safeguard | What it fixes |
| --- | --- | --- |
| 1 | Alarm latch persisted to NVS | The midterm restored `armed` but hardcoded `alarmed = false`, so cutting power silenced a latched siren — a power-cycle bypass of §8.5's own argument |
| 2 | Debounced `sensor_fault` notification + `Sensors OK` on V15 | A dead sensor made the alarm permanently unable to fire while the dashboard still read `ARMED`. Fail-silent becomes fail-safe |
| 3 | 12-deep offline event ring, drained one per second | The one-bit queue collapsed two offline alarms into one notification and never queued warnings |
| 4 | Notification budget, 20/hour, refusals logged | Blynk rate-limits events server-side and silently drops the overflow |
| 5 | DHT staleness window (30 s) | Stale readings were published forever after the sensor died |
| 6 | `esp_reset_reason()` logged at boot | Crashes and brownouts become data |
| 7 | Siren duty cycle, 10 s on / 2 s off | Lower average current, far less unpleasant, still inside the 3-minute cap |
| 8 | Siren test, silence-without-disarm, runtime persistence slider | Manual control, and the silence button is what a real user actually wants |

The siren's three-minute budget **restarts on boot** rather than being
persisted. That avoids writing to flash on a timer, and an intruder
power-cycling the device to silence it getting a fresh siren each time is the
correct outcome. Serial `X` clears the persisted state if a latch ever sticks.

New datastreams: V11 persistence, V12 siren test, V13 silence, V14 free heap,
V15 sensors OK. Event `sensor_fault` is now fired by the firmware.

## Tier 3 — delivered (branch `develop/v4`)

**Coincidence window (Req 2).** The persistence rule asks all three beams to
read open *at the same time* for `persistN` consecutive cycles — ~600-700 ms of
continuous agreement. The panel crosses the beams in sequence, so a fast sweep
can break all three without ever satisfying it. The window asks a weaker,
better-matched question: did each beam trip at *some* point within the last
`windowMs`? Each sensor latches its own trigger with a time-to-live; the rule
closes when all three latches are live together.

The two are **OR-ed**, so midterm behaviour is a strict subset: detection cannot
get worse, only the false-positive count can. Default window 1200 ms, tunable
live via V17 or serial `W<ms>` with no reflash.

**Both rules are evaluated every cycle, including after the latch**, and what
each *would* have done is recorded in `tmask` bits 3 and 4. One run therefore
yields the before/after comparison directly — no second session on the old
firmware. This is the measurement design, and it is why `docs` no longer asks
for a separate midterm baseline run.

**Risk score (Req 7) — shadow mode.** A 0-100 fusion published on V16, logged in
the `risk` column, and banded (<30 normal, 30-69 elevated, ≥70 critical). It
does **not** gate the alarm; crossing into elevated raises a `pre_alarm`
notification only. Weights sum to exactly 100 and are reproducible for the
paper:

| Term | Max |
| --- | --- |
| beams open now (3 × 10) | 30 |
| beams latched inside the window (3 × 8) | 24 |
| persistence progress | 20 |
| depth past the trip threshold | 16 |
| sound within the last 30 s | 10 |

**Verified on the host, not just compiled.** The decision logic was lifted
verbatim into a harness and run against synthetic beam traces: held panel (both
rules fire), fast staggered sweep with and without overlap (window only),
reversed sweep direction (window only), all-three-for-two-cycles (window only),
one beam, two beams, beams tripped beyond the TTL, and nothing at all (neither
rule fires in any negative case). A TTL sweep shows the window length trading
recall against false alarms as expected.

**What is not guaranteed.** Whether the window actually *gains* detections
depends on an empirical fact about this rig — whether a fast sweep really does
fail the persistence rule. If `detections gained` comes back 0, that is a real
negative result and should be reported as one; the paper then leans on the
Tier 2 reliability improvements, which are deterministic.

## Tier 4 — delivered (branch `develop/v4`)

The three findings the self-audit left open. **None of them changes a detection
verdict** — see the verification below — so a capture taken before or after this
tier answers the Tier 3 question the same way.

| # | Change | What it fixes |
| --- | --- | --- |
| 1 | Pings scheduled one per slot from `loop()` instead of behind two `delay(PING_GAP_MS)` calls | The cycle spent ~120 ms of every 300 ms in `delay()`, plus the pings, stalling the UI tick, Blynk and the serial console. Longest `loop()` pass in steady state: **195 ms → 42 ms** (one ping plus, at most, one DHT read) |
| 2 | LM393 DO counted by an edge interrupt with a 50 ms refractory period | The 50 ms poll missed any pulse shorter than its period, and every pulse during the blocking stretch. Short knocks now reach the warning tier, V6 and the risk score's sound term |
| 3 | DHT11 temperature sets the speed of sound used in ranging | The fixed 58 µs/cm is only right at ~22 °C. At 30 °C it reads ~1.3% short. The DHT11 was the one rubric sensor that fed no decision; it now feeds the distance the decision is made on |

**Timing is deliberately unchanged.** S1 fires on the cycle tick, S2 60 ms after S1's
ping returns, S3 the same, and one decision runs per 300 ms round on three fresh
readings, exactly as the midterm did. A faster cadence is now a one-constant change
(`CYCLE_MS`), but it would change how many milliseconds `PERSIST_N` cycles stands for,
so it must not change mid-battery. The 25 ms DHT11 read is held until the round
finishes. In simulation, letting it land between two pings stretched one sample gap
and cost ~2% of the fastest sweeps.

**Compensation, honestly sized.** `c = 331.3 + 0.606·T` m/s, dry air; humidity is
ignored (worth ~0.4% at 30 °C, 80% RH). At the 20 cm trip point the correction is
~0.3 cm across the indoor range, so it is a **correctness fix, not a detection
improvement**. With no usable DHT reading it falls back to the midterm constant. `?`
shows the speed in use on a `ranging` line, and each `arm` event carries it as
`sos=`. Off switch: `TEMP_COMPENSATE`.

**`sndev` changed meaning.** It now counts debounced interrupt events rather than
polled rising edges, so it reads higher than in a pre-Tier-4 capture of the same
noise. Do not compare `sndev` across that boundary.

**Verified on the host, not compiled for the ESP32.** `main.cpp` was built natively
against stubbed Arduino / Blynk / DHT APIs on a virtual clock. The stubs model HC-SR04
echo timing from speed-of-sound physics and deliver sound-pin edges as interrupts.
Both the `tier3` firmware and this one ran the same 150 randomised batteries, 4,800
trials in all: fast sweeps of random dip length, stagger and direction, held panels,
and one-beam, two-beam, hand-wave and knock negatives. **Every trial got the same
verdict and the same alarm attribution from both firmwares.** The same harness
shows:

| Measure | `tier3` | Tier 4 |
| --- | --- | --- |
| longest `loop()` pass, steady state | 195 ms | 42 ms |
| UI tick gap, p99 | 220 ms | 67 ms |
| sound events counted, out of 260 (2-120 ms pulses, chattering bursts) | 80 | 260 |
| minimum spacing between any two pings | 77 ms | 77 ms |
| sample cadence | 300 ms | 300 ms |
| reading at a true 20.0 cm, 30 °C / 35 °C | 19.7 / 19.6 cm | 20.0 / 20.0 cm |

The cross-compile still has to be confirmed with `pio run` (see Hardware Handoff §J);
the build sandbox could not reach the PlatformIO registry.

## Standalone operation

The firmware is fully operable with no Blynk account and no network. Every
dashboard control has a serial twin and every notification is echoed to the
terminal, so the entire test battery can be run from `pio device monitor`.

| Dashboard | Serial |
| --- | --- |
| V0 arm switch | `a` / `d` |
| V5 trip threshold | `T20` + Enter (bare `T` reports) |
| V11 persistence | `N3` + Enter (bare `N` reports) |
| V8 / V9 / V10 trials | `1` fast, `2` held, `0` disturbance |
| V12 siren test | `b` |
| V13 silence siren | `s` |
| V4 state, V14 heap, V15 health, everything else | `?` status block |
| Event notifications | printed as `[notify] <code>` when raised |

`w` toggles WiFi and Blynk off entirely, which stops reconnect retries during
bench work. Defaults are correct for standalone use out of the box: trip
threshold 20 cm, persistence 3 cycles.

Only the graded dashboard features themselves — history chart, scheduled
auto-arm, offline-device notification, and phone delivery of events — require
the Blynk console. None of them gate the test battery.

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
