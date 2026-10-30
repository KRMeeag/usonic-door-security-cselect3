# Writing Handoff — context for the IMRAD paper

Context package for whoever is drafting the paper. Updated as tiers land and
as results come in.

> **Status as of 2026-10-30: Tiers 1–4 are built, the hardware test battery
> (HARDWARE_HANDOFF §I) has been run twice, and the Tier 4 bench checks (§J.2)
> are done.** The measured results are in **§9** — start there for Results and
> for the "measurable improvement" claim. Everything in §§5–6c that says
> "not yet measured" was written before the runs and is superseded by §9.
> Do not report any number that is not in §9 or in the tidy CSVs.

---

## 1. What the project is

A door intrusion alarm built on an ESP32-S3 with Blynk. The motivating problem
(PRD §2) is **alert fatigue**: conventional door alarms fire on every open/close
event, so users disable them and get zero security value. This system only
alarms inside a user-armed window, so the logic — not the user's tolerance —
filters out routine traffic.

Components, all retained from the midterm: ESP32-S3-WROOM-1, 3× HC-SR04/SR05
ultrasonic sensors, LM393 sound sensor (digital out), DHT11, 5 LEDs, a passive
piezo, and Blynk for monitoring and control.

Two alert tiers, deliberately not one (PRD §8.4):
- **Sound alone → warning.** Notification only, no siren.
- **Ultrasonic consensus while armed → full alarm.** Siren plus notification.

Sound is never a precondition for the alarm (a silent break-in must still
trigger it) and never sufficient on its own (incidental noise would recreate the
alert fatigue the project exists to solve).

---

## 2. Sensing geometry — write Methods from this

Three ultrasonic sensors are mounted **on top of the door frame at roughly 45
degrees, aimed down at the door leaf**.

- **Closed** — each beam clears the leaf and lands on whatever is beyond it (wall,
  floor, or nothing in range), so every sensor reads long.
- **Opening** — the leaf swings into the beam and the reading **drops into the
  0-20 cm trip band**. Door-open is a distance decrease.

`PRD.md` has been corrected to match this; earlier drafts described a jamb mount and
that text is gone. Use the current PRD §8.1 and §9.

## 2b. Validation rig — state this plainly in Methods

Testing and the demonstration use a **hand-held cardboard panel standing in for the
door leaf**, the same arrangement as the midterm demo. Say so in Methods rather than
implying a hinged door was instrumented, and carry it into Discussion as a limitation
of external validity: the panel reproduces the beam-interruption pattern of an opening
door, but not its mass, hinge arc, or the airflow and vibration that accompany a real
one.

It also shapes how results are read. How long a beam reads open is set by how fast the
operator sweeps the panel — a **controlled input, not an observed property**. The
battery exploits that by running two positive classes:

- **fast** — panel swept *through* the beams and kept moving, as a briskly opened door
  crosses them.
- **held** — panel brought in and held ~2 s. The easy case.

**The finals narrative, stated honestly.** The midterm rule needs all three sensors
reading open simultaneously for three consecutive 300 ms cycles, about **600-700 ms of
continuous agreement**. A held panel supplies that; a fast sweep may not. If detection
rate is high on `held` and lower on `fast`, that gap is the finding the Tier 3
coincidence window exists to close, and it is a controlled comparison rather than an
anecdote. **Do not write that gap up until the baseline run has produced it.** If both
classes score the same, the honest move is to say so and justify Tier 3 on false
positives instead.

## 3. The midterm baseline

Tagged `midterm` in git. Its decision rule, from PRD §11:

> While armed, treat the door as open only when all three sensors' live readings
> have crossed a physically-measured trip threshold, and only after that
> agreement holds for several consecutive readings. Then raise a latched alarm
> that only a deliberate disarm clears.

The PRD is unusually candid about its own gaps, and those admissions are the
finals project's agenda. From §14 and §15:

- **No false-positive rate was ever measured.** The project's central claim is
  fewer false alarms, and it rests on the armed-window *design*, not on data.
- Manual arming only; time-scheduled arming was deferred.
- Sound does not escalate sensitivity; that design was considered and declined.
- The trip threshold is manually measured per installation, not self-calibrating.
- A temporal-coherence window between sensor trigger times was cut as
  unnecessary.

---

## 4. Self-audit findings — good Discussion material

Found by reading the midterm firmware. Two are genuine security holes. A report
that says "we audited our own midterm and found these" reads considerably better
than one that only lists new features.

| Finding | Status |
| --- | --- |
| A latched alarm does not survive a power cut — `armed` is restored from NVS but `alarmed` is hardcoded false on boot, so cutting power silences the siren. Contradicts the PRD's own §8.5 reasoning. | **Fixed in Tier 2** |
| One dead sensor silently disables the alarm — an unhealthy sensor is forced to "not open", and the alarm needs all three, so one unplugged ECHO makes the system permanently unable to fire while still displaying `ARMED`. Fail-silent, not fail-safe. | **Fixed in Tier 2** |
| About 40% of each cycle is spent in blocking `delay()` for anti-crosstalk spacing, stalling the UI, networking and sound polling in 120 ms blocks. | **Fixed in Tier 4** |
| The sound sensor is polled every 50 ms and not at all during ping delays, so short LM393 pulses are undercounted. | **Fixed in Tier 4** |
| The offline event queue is one bit deep, so two offline alarms collapse into one notification and warnings are not queued at all. | **Fixed in Tier 2** |
| The DHT11 contributes to no decision — it is a rubric-required sensor used only as telemetry. | **Addressed in Tier 4**: it sets the speed of sound for ranging (small effect, see §6c) |

---

## 5. What Tier 1 delivered (done, safe to write up)

Tier 1 adds **no behavioural change** — this was deliberate, so the baseline
measurement is of the midterm system and not of something subtly different.

- **A frozen CSV log schema.** The firmware interleaves machine-readable `#D`
  telemetry samples (once per cycle) and `#E` system events with its existing
  human-readable output on the same serial stream. Fields that only become
  meaningful later read 0 until then, so the baseline capture and the final
  capture parse identically — without that, the before/after comparison would
  not be sound.
- **One-tap test trials.** A single button press — on the Blynk dashboard or
  over serial — clears the previous trial's latch, re-arms, logs a ground-truth
  marker and chirps "go". The analysis pairs each marker with what the system
  then did, producing the confusion matrix with no hand labelling. Worth
  describing in Methods: it is why the results are reproducible rather than
  tallied by hand, and why the battery is single-operator.
- **An analysis script** (`tools/analyze.py`) producing the summary statistics,
  the tidy CSV dataset, and the figures.
- **A synthetic self-test** so the analysis can be verified without hardware.

### Figures the analysis produces

1. `door_opening_trace.png` — distance against time for all three beams during
   one real opening, with the trip threshold and the alarm instant marked.
2. `detection_latency.png` — histogram of first-beam-break to siren, measured in
   firmware rather than by stopwatch.
3. `confusion_matrix.png` — TP / FP / TN / FN with recall, precision, specificity.
4. `dip_durations.png` — how long each beam actually reads open during an
   opening, against the ~700 ms bar the midterm rule needs. **This is the
   figure that carries the argument.**
5. `free_heap.png` — memory over the capture, for the no-leak claim.

---

## 6. What Tier 3 delivered (built; results are in §9)

The design can be written up as done. The hardware runs have since been done —
see **§9** for what they showed, including a false-alarm cost the design
section should not hide. Full detail is in `docs/FINALS.md` under Tier 3.

**The coincidence window (Req 2).** Each sensor latches its own trigger with a
time-to-live (default 1200 ms). The window rule fires when all three latches are
live at once. That fits a transient sweep; the midterm rule needs ~600-700 ms of
continuous three-beam agreement. The two rules are **OR-ed**, so the midterm
behaviour is a strict subset: detection cannot get worse, and only the
false-positive count can.

**One run gives the before/after.** Both rules are evaluated every cycle,
including after the latch, and what each *would* have done is logged. The
`rule comparison` block in `analyze.py` therefore compares the old rule and the
new on the same trials. Say so in Methods: it is a paired comparison on
identical inputs, not two sessions.

**The risk score (Req 7), in shadow mode.** A 0-100 weighted fusion of beams
open now, beams latched in the window, persistence progress, depth past the
threshold, and sound in the last 30 s. The weights sum to exactly 100 and are
tabled in FINALS. It is published and logged but **never triggers the alarm**;
crossing 30 raises a `pre_alarm` notification only. The design dropped the
earlier *time armed* term, so do not list it.

---

## 6c. What Tier 4 delivered (done, safe to write up)

Closes three of the §4 self-audit findings. **No detection verdict changes**,
which matters for the paper: the Tier 3 comparison means the same thing on
either side of this tier.

1. **Non-blocking sensing.** The three pings used to run back to back behind two
   blocking 60 ms delays. They are now scheduled one per slot, with identical
   order and spacing and one decision per 300 ms round, so the rest of the
   firmware keeps running between them. Keeping the timing identical was a
   deliberate choice: a faster cadence would change what "three consecutive
   cycles" means in milliseconds and break comparability. One subtlety is worth
   a sentence in Discussion. When the 25 ms DHT11 read was allowed to land
   between two pings, the uneven sample spacing cost ~2% of the fastest sweeps
   in simulation. Slow work now waits for the round to finish.
2. **Interrupt-driven sound capture.** Short knocks the 50 ms poll used to miss
   are counted, with a 50 ms refractory period so one chattering comparator
   burst stays one event. In simulation the poll caught 80 of 260 pulses and the
   interrupt caught all 260. Sound still never gates the alarm (§1); this only
   affects the warning tier, V6 and the risk score's sound term.
3. **DHT11 temperature compensation of the speed of sound.** The firmware used a
   fixed 58 µs/cm, which is only right at ~22 °C. It now uses
   c = 331.3 + 0.606·T m/s from the DHT11 reading and falls back to the constant
   without one. This gives the DHT11 a role in the measurement the alarm
   decides on, which answers the "telemetry only" finding. **Size it honestly:**
   at the 20 cm trip point the correction is ~0.3 cm over the indoor range.

**How it was verified.** The firmware source was compiled natively against
stubbed hardware APIs on a virtual clock and run through 4,800 simulated trials
alongside the Tier 3 firmware, with identical verdicts on every trial. That is
a software verification. The simulated trials are **not** results and must
not appear in Results.

---

## 6b. What Tier 2 delivered (done, safe to write up)

Eight safeguards. The first two close the security holes in §4; all are
desk-verifiable, so none of them cost door time.

1. **The alarm latch survives power loss.** Both `armed` and `alarmed` are
   persisted now. The siren's three-minute budget deliberately restarts on boot
   rather than being persisted — it avoids writing to flash on a timer, and
   someone power-cycling the device to silence it getting a fresh siren each
   time is the correct outcome. Worth one sentence in Discussion: the midterm
   had a power-cycle bypass of its own latching argument.
2. **A dead sensor is now visible.** Debounced over five consecutive cycles,
   then a `sensor_fault` notification, a `Sensors OK` indicator that drops, and
   a log entry. The system still cannot fire with a sensor down — that part is
   unchanged — but it no longer claims to be protecting anything. Fail-safe
   rather than fail-silent.
3. **A real offline notification queue** — a twelve-deep ring of timestamped
   events drained one per second on reconnect, replacing the single bool.
   Dropped-event counts are logged, so the queue's own behaviour is measurable.
4. **A notification budget** of twenty per hour. Blynk rate-limits events
   server-side and silently discards the overflow, so an unbounded burst means
   *no* alerts rather than many. Anything refused is written to the log.
5. **Reading staleness.** The midterm published the last good temperature and
   humidity forever after the DHT11 died. Past a thirty-second window the
   readings are invalidated and publishing stops.
6. **Reset-reason logging** at every boot, so crashes and brownouts appear in
   the data instead of being inferred.
7. **Siren duty cycling** — ten seconds on, two off, inside the existing
   three-minute cap. Lower average current and considerably less unpleasant.
8. **Manual controls** — a siren test, a *silence without disarming* button
   (the control a real user actually wants, and the one that reads as a design
   decision rather than a feature count), and a runtime persistence slider.

A serial `X` command clears the persisted state if a latch ever sticks.

**Standalone operation.** Every dashboard control has a serial equivalent and
every notification is echoed to the terminal, so the prototype is fully
operable and testable with no network and no app. Worth a line in Methods: the
validation battery does not depend on cloud connectivity, which is consistent
with the offline-resilience argument in PRD §8.6 and means the measured
detection results are not entangled with network behaviour.

**Reliability indicator available for free:** the Wi-Fi down/up event pairs are
logged with outage durations, so reconnect success rate and mean recovery time
can be reported without any extra instrumentation.

---

## 7. IMRAD mapping

- **Introduction** — PRD §2 (alert fatigue) and §5 (differentiation from the
  other Smart Home groups). Largely reusable as written.
- **Methods** — PRD §8 for design rationale, **corrected per §2 above**. Add the
  trial-marker protocol, the logging schema, and the test battery. The
  transient-dip analysis belongs here as the motivation for the Tier 3 rule.
- **Results** — generated by `tools/analyze.py`, tabulated in §9. The core table
  is the paired old-rule vs old-rule-plus-window comparison (§9.2), shown for
  both runs, with the false-alarm cost beside the detection gain.
- **Discussion** — start from PRD §14 and work through it: which limitations
  were closed, by what, and which remain. Then the self-audit findings. That
  structure writes itself and is exactly what a panel wants.

---

## 8. Do not claim

- **Do not claim a false-positive improvement.** The measured effect of the
  coincidence window on false alarms is a *cost* (§9.2): 0/12 → 1/12 in run 1,
  0/14 → 2/14 in run 2. The old rule was evaluated in parallel inside the same
  firmware run; a separate `midterm`-tag session was **not** run, so state that
  the baseline is the in-firmware parallel evaluation of the midterm rule and
  that its equivalence to the midterm firmware is assumed, not separately
  measured.
- **Do not present run 1 or run 2 on its own as "the" result, and do not pick
  the better one.** Two runs exist, they differ for an identifiable reason
  (operator sweep speed, §9.4), and both go in the paper.
- **Do not call the risk score a working discriminator.** In both runs the
  openings and disturbances overlapped (§9.5).
- **Do not present synthetic self-test output as a result.** It is fabricated
  test data for checking the analysis script. The same goes for the Tier 4 host
  simulation: it verified the firmware logic, and its trial counts are not
  measurements of the device.
- **Do not claim temperature compensation improves detection.** It corrects the
  distance by ~0.3 cm at the trip point. Call it a correctness fix that gives
  the DHT11 a role in ranging, and stop there.
- **Do not compare sound-event counts (`sndev`) across the Tier 4 boundary.**
  The count changed from polled edges to debounced interrupt events, so the
  same noise reads higher after it.
- **Do not describe adaptive thresholds, anomaly detection, or machine learning.**
  None are being built. The advanced feature is a weighted risk score with a
  published coefficient table, which is more defensible anyway.
- **Do not round dip durations below one sampling period into real numbers.**
  They are at the measurement floor; say so.
- **Do not imply a real hinged door was instrumented.** All trials use a hand-held
  cardboard panel; say so in Methods and name it as a limitation in Discussion.
- **Fast openings: say what was measured, no more.** Persistence alone caught
  none of the fast sweeps in either run (0/8 and 0/11; §9.2). The final rule
  caught 5/8 in run 1 and 1/11 in run 2, but in run 2 only 3 of the 11 fast
  trials brought a beam inside the trip band at all (§9.1). Quote both numbers
  and that fact together; do not quote 1/11 as the detector's fast-sweep rate.
- Keep statistics at the level the work supports: means, standard deviations, a
  confusion matrix, and a paired before/after table. No significance testing.
  §9.10 gives optional 95% intervals as a courtesy; they are derived, not
  produced by `analyze.py`, and no hypothesis test is reported.

---

## 9. Measured results — battery runs 1 and 2 (2026-10-30)

Numbers are of two kinds. Unmarked ones are copied from the `tools/analyze.py`
printout of each capture (saved as `data/run1/summary.txt` and
`data/run2/summary.txt`). Ones marked **(derived)** are simple counts and
filters on `data/run*/trials.csv` (columns `cls`, `old_fired`, `win_fired`,
`outcome`, `first_open_ms`, `min_dist_cm`, `latency_detect_ms`), or Wilson
intervals computed from those counts; `analyze.py` does not print them. Where
something is inferred rather than read off the data, it says so.

**Setup, both runs.** Firmware `claude/develop-v4-remaining-ln7q5n` at commit
`9456d5a` (Tier 4) on an ESP32-S3-DevKitC-1, **standalone** (Wi-Fi off, no
Blynk). Trip threshold 20 cm, persistence 3 cycles, **coincidence window
1200 ms (never retuned)**, 300 ms sampling period. One operator, hand-held
cardboard panel, one rig. Raw logs (gitignored, kept locally):
`logs/run1/device-monitor-261030-210948.log` and
`logs/run2/device-monitor-261030-212816.log`. Per-run printout, tidy CSVs and
figures: `data/run1/`, `data/run2/` (each with `summary.txt`, `samples.csv`,
`events.csv`, `trials.csv`) and `docs/figures/run1/`, `docs/figures/run2/`.
Regenerating them from the raw logs reproduced the headline numbers (confusion
matrix, rule comparison, latency, risk score, heap, fault rate) exactly.

### 9.1 Read this first — provenance

- **Two runs were done, and both go in the paper.** The operator repeated the
  battery after judging the execution of run 1 uneven. Run 2 is a repeat, not a
  replacement. Do not drop either because of how its numbers look.
- **Neither run matches the planned 10 fast / 10 held / 12 disturbance.**
  Run 1 was 8 fast / 12 held / 12 disturbance. Run 2 was 11 fast / 11 held /
  14 disturbance (36 trials). The trial class is set by the key pressed, so the
  split reflects what was pressed. The cause of run 2's two extra positives and
  two extra negatives is **unconfirmed** (a sanity check inside the same
  session would produce exactly 1 fast + 1 held + 2 disturbance); the operator
  should confirm, and Methods should say whichever it was. Report n as measured.
- **Arm refusals** (`arm_abort`, a sensor check failed at arming): 11 in run 1,
  1 in run 2. A refused arm logs no marker, so these do not affect any count.
- **Trial execution differed a lot between the runs, and it is an
  operator-controlled input, not a property of the system** (see §2b). The
  largest effect (**derived**): in run 2, **8 of the 22 positive trials — all of
  them fast sweeps — never brought any beam inside the 20 cm trip band** (closest
  approach 20.2–27.2 cm on those trials; `first_open_ms` is blank), so neither
  rule had anything to detect. In run 1 this happened once in 20 (a held trial,
  closest approach 148.8 cm). Holds were also shorter in run 2 (§9.4). Both
  facts have to be stated wherever the two runs are compared (§9.2, §9.4).

### 9.2 Headline — paired comparison, old rule vs old rule + window

Both rules were evaluated every cycle on the same trials, so this is a paired
comparison on identical inputs (the design in §6).

| | Run 1 | Run 2 |
| --- | --- | --- |
| Positive trials | 20 | 22 |
| Persistence rule alone | 8/20 (40.0%) | 2/22 (9.1%) |
| Persistence OR window (final rule) | 16/20 (80.0%) | 10/22 (45.5%) |
| **Detections gained by the window** | **8** | **8** |
| Detections lost | 0 (the rules are OR-ed, so none by construction) | 0 |
| Disturbance trials | 12 | 14 |
| False alarms, persistence alone | 0/12 | 0/14 |
| False alarms, with window | 1/12 (8.3%) | 2/14 (14.3%) |

**By trial class (derived).** Persistence alone vs the final rule, positives only:

| | Run 1 | Run 2 |
| --- | --- | --- |
| Fast sweeps: persistence alone | 0/8 | 0/11 |
| Fast sweeps: final rule | 5/8 | 1/11 |
| Held: persistence alone | 8/12 | 2/11 |
| Held: final rule | 11/12 | 9/11 |
| Detections gained by the window | 5 fast + 3 held | 1 fast + 7 held |
| Detections lost | 0 | 0 |

Persistence alone caught **no fast sweep in either run** (0/19 across both).

**Only trials where the stimulus reached the beams (derived).** A positive
"counts" here when at least one beam read inside the 20 cm trip band
(`first_open_ms` present, equivalently `min_dist_cm` ≤ 20.0; the two criteria
agree on every trial). This removes trials where nothing could be detected by
either rule. It is a **post-hoc** filter chosen after seeing the runs, so report it
*next to* the unfiltered table above, never instead of it. It is based on the
sensor readings, not on the alarm outcome.

| | Run 1 | Run 2 |
| --- | --- | --- |
| Positives with a beam opening | 19/20 | 14/22 |
| Persistence alone | 8/19 (42.1%) | 2/14 (14.3%) |
| Final rule | 16/19 (84.2%) | 10/14 (71.4%) |
| Of those, fast sweeps: persistence / final | 0/8 / 5/8 | 0/3 / 1/3 |
| Of those, held: persistence / final | 8/11 / 11/11 | 2/11 / 9/11 |

On this view the two runs are much closer (84% vs 71%) than the unfiltered 80%
vs 45%, and most of the difference is the eight run-2 fast trials in which the
panel never got inside the trip band. The remaining difference is mainly that
run 2's holds were shorter (§9.4). The four run-2 misses that did open a beam
had closest approaches of 18.4, 19.7, 19.7 and 12.6 cm — three of them only just
inside the threshold. On none of the four did all three beams open (at most two
beams, each a single-sample dip, apart from one 617 ms dip on one beam in the
12.6 cm trial).

**Suggested wording for the "measurable improvement" claim** (adjust, but keep
both halves and the caveat): *In a paired comparison on identical trials, adding
the coincidence window to the midterm persistence rule raised detection from
8/20 (40%) to 16/20 (80%) in run 1 and from 2/22 (9%) to 10/22 (45%) in run 2 —
a gain of eight detections in each run — at a cost of one (1/12) and two (2/14)
false alarms among disturbance trials, where persistence alone produced none.
The midterm rule detected none of the fast sweeps in either run. Restricted to
trials in which the panel actually reached the trip band, detection rose from
8/19 (42%) to 16/19 (84%) and from 2/14 (14%) to 10/14 (71%).* This is a
sensitivity gain with a specificity cost, not an across-the-board improvement.
Do not generalise beyond this panel, operator and rig.

### 9.3 Confusion matrix and rates (final rule)

| | Run 1 | Run 2 |
| --- | --- | --- |
| TP / FN | 16 / 4 | 10 / 12 |
| FP / TN | 1 / 11 | 2 / 12 |
| Recall | 80.0% | 45.5% |
| Precision | 94.1% | 83.3% |
| Specificity | 91.7% | 85.7% |
| False-positive rate | 8.3% | 14.3% |
| Accuracy | 84.4% | 61.1% |
| F1 | 86.5% | 58.8% |

By trial class (final rule): run 1 — fast 5/8 (62.5%), held 11/12 (91.7%),
disturbance false alarms 1/12; run 2 — fast 1/11 (9.1%), held 9/11 (81.8%),
disturbance false alarms 2/14.

### 9.4 Beam dips — why the runs differ

| | Run 1 | Run 2 |
| --- | --- | --- |
| Fast: beam dips (n) | 24 | 33 |
| Fast: mean ± sd (max) | 375.8 ± 231.7 ms (894) | 45.5 ± 109.2 ms (300) |
| Fast trials holding all three beams for ~720 ms | 0/8 | 0/11 |
| Held: beam dips (n) | 36 | 33 |
| Held: mean ± sd (max) | 2423.3 ± 2432.6 ms (12276) | 908.1 ± 674.8 ms (2700) |
| Held trials holding all three beams for ~720 ms | 10/12 | 3/11 |
| Single-sample dips (≤ 300 ms) | 14/60 | 11/66 |

How to read this table. `n` is one dip per beam per trial (3 × the number of
trials), and a beam that never opened counts as a dip of 0 ms. That is why run
2's fast row is so low: **8 of its 11 fast trials had no beam opening at all**
(§9.1). **Derived:** 28 of its 33 fast dips are zero and the other five are
single-sample dips of exactly 300 ms (run 1: 3 of 24 are zero). The run 2 fast
mean is therefore mostly a count of trials in which the panel did not reach the
beams, not a measure of how quickly it moved through them.

The other difference is the holds: run 2's were much shorter (mean 908 ms vs
2423 ms; only 3/11 sustained ~720 ms vs 10/12), which is consistent with
persistence alone catching 2/11 held trials in run 2 against 8/12 in run 1. The
"held ~720 ms" row is computed afterwards from the logged samples, so it is a
guide and not the firmware's own decision; 10/12 qualifying against 8/12 firing
in run 1 shows the two do not map one-to-one. Do not equate them.

**Single-sample dips are at the measurement floor (§8) — do not quote them as
real durations.** A reasonable reading, not a measured fact: for dips under one
sampling period the 300 ms cadence limits detection whichever rule is used.
`CYCLE_MS` is a one-constant change that was deliberately left alone to keep the
runs comparable, and is a natural item for future work. `dip_durations.png` for
**both** runs carries this argument.

### 9.5 Risk score (Req 7, shadow mode)

| | Run 1 | Run 2 |
| --- | --- | --- |
| Openings: n, mean (min) | 20, 59.6 (0.0) | 22, 31.3 (0.0) |
| Disturbances: n, mean (max) | 12, 33.4 (72.0) | 14, 35.8 (58.0) |
| `analyze.py` separation | −72.0 (overlapping) | −58.0 (overlapping) |

The score never triggers the alarm, and in neither run did it separate openings
from disturbances (some openings scored 0, some disturbances scored 58–72).
Report it as built and logged, not as a demonstrated discriminator.

### 9.6 Latency — read the caveats before quoting

As printed by `analyze.py`:

| | Run 1 | Run 2 |
| --- | --- | --- |
| Detect, beam → siren: n | 17 | 12 |
| mean ± sd (min / max) | 909.9 ± 1529.3 ms (−11.0 / 6000.0) | 845.2 ± 731.7 ms (−10.0 / 2161.0) |
| Marker → siren: mean ± sd | 3487.5 ± 2092.2 ms | 2164.4 ± 1150.3 ms |

**True positives only (derived)** — this is the figure to quote for detection
latency, because the printed `n` also includes false alarms (below):

| | Run 1 | Run 2 |
| --- | --- | --- |
| Detect, beam → siren: n | 16 | 10 |
| mean ± sd | 836.6 ± 1548.2 ms | 649.0 ± 610.7 ms |
| **median** (min / max) | **301 ms** (−11 / 6000) | **602 ms** (−10 / 2103) |
| Fast sweeps (n, values) | 5: 278, −10, 299, 893, 589 | 1: −10 |
| Held (n) median | 11, 303 ms | 9, 602 ms |

- **Report the median alongside the mean.** Run 1's mean is pulled up by one
  trial (below); the median is far steadier. Detection latency is quantised by
  the 300 ms sampling period, so values cluster near multiples of it.
- **The printed `n` includes false alarms** (confirmed in `trials.csv`: run 1
  trial 28 and run 2 trials 30 and 32 have a latency and outcome FP). A false
  alarm has no true opening to be late for, so do not quote the printed mean as
  detection latency.
- **Marker → siren is not detection latency.** It runs from the key press and
  includes however long the operator took to start the motion.
- **The 6000 ms maximum in run 1 is held trial 12** (marker → siren 8174 ms). The
  latency is measured from the first sample in which *any* beam read open, so an
  early stray opening, or a hand entering before the hold proper, would produce
  this. That is a reading of how `first_open_ms` is defined (`analyze.py`
  `build_trials`), not something checked against the raw samples; look at the
  `door_opening_trace.png` trace for that trial before quoting it.
- **Negative values (−10 or −11 ms)** occur in three run-1 trials and one run-2
  trial. The alarm event is stamped a few milliseconds before the first
  open-beam sample, which is a timestamp-ordering offset between the two log
  lines, not a siren before the opening. Treat them as ≈ 0.

### 9.7 Stability and reliability

| | Run 1 | Run 2 |
| --- | --- | --- |
| Capture length / samples | 9.9 min / 1752 | 6.3 min / 1054 |
| Samples with a sensor fault | 0 (0.00%) | 0 (0.00%) |
| Free heap: mean ± sd | 275,730.9 ± 79.1 B | 275,695.9 ± 25.2 B |
| Heap drift over capture | −128 B | +8 B |
| Temperature / humidity (mean) | 28.9 °C / 72.0 % | 28.8 °C / 72.9 % |
| RSSI | no data (standalone) | no data (standalone) |

Both captures are short: they support "no leak or fault in a few minutes",
not any long-duration claim. **Excluded:** the first bench capture (before the
markers existed, 8.8 min, no trials) is not a result — sensor 2 had a loose
connection and 63.4% of its samples had a fault. Its +35,696 B heap rise was
flagged INVESTIGATE by the script; the most likely cause is that Wi-Fi was
switched off part-way through (15 `net_mode` events), which frees heap, but that
is an inference from the event counts and was not checked line by line. Either
way it was a rise, not a leak, and both battery runs (Wi-Fi already off) are
flat.

### 9.8 Bench checks (HARDWARE_HANDOFF §J.2 and §F)

"Observed" means seen or heard by the operator; the log cannot record it.

| Check | Evidence | Result |
| --- | --- | --- |
| Tier 4 build | Compiled and flashed on the real toolchain (`espressif32@7.0.1`); flash 22.7%, RAM 14.3% | Pass |
| Console responsive while logging | Observed | Pass |
| `t` self-test (LEDs sweep, piezo chirps) | Observed | Pass |
| Interrupt sound capture | 5 table taps → `events=` 0 → 5; LED4 flashed once per tap | Pass |
| DHT11 sets the speed of sound | `ranging` 349.2–349.7 m/s (≈ 30 °C by c = 331.3 + 0.606·T) | Pass |
| DHT staleness and recovery | `dht_stale` at 40.6 s uptime, `dht_ok` at 88.6 s after replug | Pass |
| Fallback `ranging` line while stale (344.8 m/s fixed) | Not captured | Not shown |
| Siren test `b` | Rising sweep, observed | Pass |
| Persistence setting `N3` | `[cfg] persistence = 3 cycles` | Pass |
| Alarm and siren duty cycle | `[ALARM] intrusion — fired by window`; 10 s on / 2 s off observed | Pass |
| Silence without disarming | `[silence] siren muted; alarm still latched` | Pass |
| Latch survives power loss | After a USB pull: `[boot] ALARM latch restored from NVS — siren resuming`, `ready — state ALARM` | Pass |
| Escape hatch `X` clears the latch | Observed | Pass |
| Dead sensor visible (deliberate, sensor 1) | `[FAULT] sensor down (h=011)`, `[notify] sensor_fault` queued, `[FAULT] cleared` on replug | Pass |

**Incidental Tier 2 evidence.** In the first bench capture, the dead-sensor
detector flagged a real loose connection on sensor 2 (`h=101`: three `fault`
and three `fault_cleared` events). That is the fail-safe behaviour of §6b item 2
working on a genuine fault, not a staged one. After reseating, both battery
runs show 0% faulted samples. Notifications could only be checked as the
`[notify] … (offline — queued)` echo, since the runs were standalone.

### 9.9 Limitations to carry into Discussion

- One operator, one hand-held panel, one rig, short captures, standalone only:
  the validation battery says nothing about a real hinged door, a different
  operator, or network behaviour.
- The old rule is the in-firmware parallel evaluation, not a separate run of
  the `midterm` tag (§8).
- Sample sizes are small (20–22 positives, 12–14 disturbances) and the class
  split is unplanned (§9.1); the intervals in §9.10 are wide.
- Trial execution was uneven: in run 2 the panel never reached the trip band on
  8 of 11 fast trials, and run 2's holds were shorter than run 1's (§9.1, §9.4).
  The raw run-2 fast-sweep rate (1/11) says more about the trials than about the
  detector. The restricted view in §9.2 addresses this but is a post-hoc filter.
- Disturbance subtype (one beam, two beams, hand wave, table knock) is not
  logged — every negative trial is class `neg` — so a false alarm cannot be
  attributed to a subtype from the data. The closest-approach column shows that
  some negatives were far from the beams (e.g. 149–177 cm) and some very close
  (2–4 cm), but not which gesture each was.
- Detection is quantised by the 300 ms sampling period (§9.4).
- Not exercised: the Blynk dashboard features and phone delivery of events,
  the Wi-Fi reconnect measurement, any overnight run, and the stale-DHT
  fallback `ranging` line.

### 9.10 Optional — 95% Wilson intervals (derived, not from `analyze.py`)

Computed from the counts above with the Wilson score interval. They are
estimates of uncertainty, not hypothesis tests, and each run is treated
separately — do not pool the two runs, since the operating points differ.

| Quantity | Run | Count | Rate | 95% interval |
| --- | --- | --- | --- | --- |
| Persistence alone, all positives | 1 | 8/20 | 40.0% | 21.9–61.3% |
| Persistence OR window, all positives | 1 | 16/20 | 80.0% | 58.4–91.9% |
| With window, fast | 1 | 5/8 | 62.5% | 30.6–86.3% |
| With window, held | 1 | 11/12 | 91.7% | 64.6–98.5% |
| False alarms, persistence alone | 1 | 0/12 | 0.0% | 0.0–24.2% |
| False alarms, with window | 1 | 1/12 | 8.3% | 1.5–35.4% |
| Persistence alone, all positives | 2 | 2/22 | 9.1% | 2.5–27.8% |
| Persistence OR window, all positives | 2 | 10/22 | 45.5% | 26.9–65.3% |
| With window, fast | 2 | 1/11 | 9.1% | 1.6–37.7% |
| With window, held | 2 | 9/11 | 81.8% | 52.3–94.9% |
| False alarms, persistence alone | 2 | 0/14 | 0.0% | 0.0–21.5% |
| False alarms, with window | 2 | 2/14 | 14.3% | 4.0–39.9% |
| Fast sweeps caught by persistence alone | 1 | 0/8 | 0.0% | 0.0–32.4% |
| Fast sweeps caught by persistence alone | 2 | 0/11 | 0.0% | 0.0–25.9% |
| Beam-opening positives, persistence alone | 1 | 8/19 | 42.1% | 23.1–63.7% |
| Beam-opening positives, final rule | 1 | 16/19 | 84.2% | 62.4–94.5% |
| Beam-opening positives, persistence alone | 2 | 2/14 | 14.3% | 4.0–39.9% |
| Beam-opening positives, final rule | 2 | 10/14 | 71.4% | 45.4–88.3% |

"Beam-opening positives" is the post-hoc subset of §9.2 (at least one beam read
inside the 20 cm trip band).

### 9.11 Open items for whoever writes Results

- **Operator to confirm:** why run 2 has 22 positives / 14 negatives (§9.1), and
  — if the order was written down or remembered — which disturbance type was
  trial 28 in run 1 and trials 30 and 32 in run 2 (the three false alarms).
- Decide how to state the in-firmware-baseline assumption in Methods (§8).
- Decide whether to present the post-hoc "beam-opening" view (§9.2) as a
  secondary table. If so, say plainly that the filter was applied after seeing
  the runs.
- Look at `door_opening_trace.png` for run 1 held trial 12 before quoting any
  latency mean (§9.6); the median is the safer figure.
- If the stale-DHT fallback `ranging` line matters, it needs a 1-minute re-test.
