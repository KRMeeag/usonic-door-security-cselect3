# Writing Handoff — context for the IMRAD paper

Context package for whoever is drafting the paper. **Not committed**
(gitignored). Updated as tiers land.

> **Status as of this writing: Tiers 1–4 are built. The hardware test battery
> (HARDWARE_HANDOFF §I) has not been run yet**, so there are no measured results.
> Do not report any number that has not actually been measured on the rig.

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

## 6. What Tier 3 delivered (built; results not yet measured)

The design can be written up as done. Every result it implies still waits on
the hardware run. Full detail is in `docs/FINALS.md` under Tier 3.

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
- **Results** — generated by `tools/analyze.py`. The core table is midterm and
  final side by side on the same battery.
- **Discussion** — start from PRD §14 and work through it: which limitations
  were closed, by what, and which remain. Then the self-audit findings. That
  structure writes itself and is exactly what a panel wants.

---

## 8. Do not claim

- **Do not claim a false-positive improvement without both runs.** The midterm
  figure does not exist until the `midterm` tag goes through the same battery.
  If only one run gets done, report the final rate alone and say plainly that no
  baseline comparison was collected.
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
- **Do not assert that fast openings are missed until the baseline run shows it.**
  It is a reasonable prediction from the timing, not a measured fact yet.
- Keep statistics at the level the work supports: means, standard deviations, a
  confusion matrix, and a paired before/after table. No significance testing.
