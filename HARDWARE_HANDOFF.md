# Hardware Handoff

**One person, at a desk. ~30 minutes, and only the last 15 are time-critical.**
Gitignored working doc — tick things off in place.

Testing uses the **cardboard panel standing in for the door leaf**, same as the
midterm demo. No real door needed, no walking back and forth.

> **No Blynk? Everything testable still works.** The device runs fully
> standalone over the serial terminal — arming, trials, thresholds, silencing,
> status, notifications. **You can do the §C baseline run today with no app and
> no network.** See §H at the bottom. The Blynk sections are graded dashboard
> deliverables, not test prerequisites — defer them.

Geometry is settled: sensors on top of the frame at 45°, aimed at the leaf.
Reading drops into **0–20 cm = door opening**. Nothing to calibrate or measure.
Tier 1 adds logging only — detection behaviour is unchanged from the midterm.

---

## A · Blynk console — 10 min  *(DEFERRABLE — not needed for any test below)*

Four of these are graded features. All console work, no code.

- [ ] **Three trial buttons.** New datastreams **V8**, **V9**, **V10**, type
      Integer, min 0 max 1. One Button widget each, **mode PUSH** (not Switch):
      | Pin | Label |
      | --- | --- |
      | V8 | `TRIAL: fast sweep` |
      | V9 | `TRIAL: hold panel` |
      | V10 | `TRIAL: disturbance` |
- [ ] **History chart.** SuperChart over **V1** (temperature) and **V2**
      (humidity). → dashboard feature #1.
- [ ] **Scheduled auto-arm.** Automations → new schedule → **V0 = 1** at an arm
      time, **V0 = 0** at a disarm time. Set them a few minutes out so you can
      watch it fire. → dashboard feature #2.
- [ ] **Offline notification.** Template → Settings → Notifications → enable
      "device offline". → reliability safeguard.
- [ ] **Define event `sensor_fault`.** Template → Events → new event, code
      exactly `sensor_fault`, type Warning, notifications on. Firmware doesn't
      fire it until Tier 2; defining it now means no console work later.

---

## B · Flash and sanity check — 5 min

```sh
pio run -t upload
pio device monitor -f log2file
```

- [ ] `#H,` header appears, then `#D,` lines about 3× per second.
- [ ] `t` — five LEDs sweep, piezo chirps.
- [ ] Tap **TRIAL: hold panel**, or press **`2`** in the terminal. You should
      hear the arm chirp then **two high pips** — armed, marker logged, go.
- [ ] Hold the cardboard in front of all three sensors → siren. Tap any trial
      button again → clears and re-arms.

A **low buzz** instead of the two pips means arming was refused — something is
sitting in a beam. Clear it and tap again.

---

## C · The baseline run — 15 min, solo
### (STALE - DO NOT DO FOR THIS VERSION)

**Superseded by §I.** Tier 3 evaluates the old and new rules *in parallel* and
logs what each would have done, so one run on the current build produces the
before/after comparison directly. You no longer need to flash the `midterm`
tag or run a separate session. Go to §I.

<details><summary>kept for reference</summary>


**The only step that cannot be done later.** Once Tier 3 changes the detection
logic the midterm numbers are gone and "measurable improvement over the midterm"
becomes unprovable.

```sh
git checkout midterm
git checkout tier1 -- src/main.cpp tools/
pio run -t upload
pio device monitor -f log2file
```

Sit at the rig with the cardboard in one hand. Drive it from **the phone** or
**the terminal** — `1` fast, `2` held, `0` disturbance. Identical code path,
identical data. **One keypress per trial**; no disarming or re-arming.

### 10 × `TRIAL: fast sweep`  (serial `1`)
Tap → two pips → **sweep the panel through all three beams quickly and keep
going**, the way a briskly opened door crosses them. Don't stop in the beams.

- [ ] 10 done

### 10 × `TRIAL: hold panel`  (serial `2`)
Tap → two pips → **bring the panel in and hold it there** for about two seconds,
then withdraw.

- [ ] 10 done

### 12 × `TRIAL: disturbance`  (serial `0`)
Tap → two pips → do the thing. **Must not alarm.**

- [ ] 3 × block **one** beam only, hold a second
- [ ] 3 × block **two** beams only, hold a second
- [ ] 3 × wave a hand quickly through all three
- [ ] 3 × clap or knock the table hard, panel nowhere near

- [ ] Stop the monitor. Capture file: `logs/________________`

---

</details>

## D · One command

```sh
python3 tools/analyze.py logs/*.log
```

- [ ] Paste the printed summary below.

```
<paste here>
```

**The lines that matter** are under *by trial class*:

```
held       n=10   detected  ?/10
fast       n=10   detected  ?/10
neg        n=12   false alarms ?/12
```

If `held` is high and `fast` is noticeably lower, that gap is the finding the
whole finals algorithm change exists to close — and it's a controlled result,
because you set the sweep speed yourself. If both are high, say so; Tier 3 then
has to be justified on false positives instead, and that's worth knowing early.

---

## If you're out of time

**Do §I.** It is ~8 minutes and it produces every number the submission needs,
including the before/after comparison, from a single run on the current build.

---

## Known, not worth chasing

- One unplugged sensor means no alarm can fire and nothing warns you.
  **Still true on the `tier1` build you use for the baseline run in §C.**
  Fixed on `tier2` — it now sends a `sensor_fault` notification and drops V15.
- The boot jingle adds ~4 s to startup. Don't time latency from boot.
- Blynk rate-limits event notifications and silently drops the excess. Test the
  notification path the day before the demo, on the hotspot you'll actually use
  (the ESP32-S3 is 2.4 GHz only).


---
---

# Tier 2 — safeguards  *(added after §A–§D; do §C first)*

Nothing above is stale. §C still uses the `tier1` build on the `midterm` tag —
**run it before flashing `tier2`**, because `tier2` is the last point where the
baseline is still recoverable.

Flash with:

```sh
git checkout finals
pio run -t upload
pio device monitor -f log2file
```

## E · Five more Blynk datastreams — 5 min  *(DEFERRABLE — see §H for serial equivalents)*

| Pin | Type | Widget | Label |
| --- | --- | --- | --- |
| V11 | Integer, min 2 max 8, **default 3** | Slider | `Persistence (cycles)` |
| V12 | Integer, 0–1 | Button, **PUSH** | `Siren test` |
| V13 | Integer, 0–1 | Button, **PUSH** | `Silence siren` |
| V14 | Integer | Label / value display | `Free heap` |
| V15 | Integer, 0–1 | LED | `Sensors OK` |

Set V11's datastream **default to 3**. A default of 0 would sync on connect and
trip the alarm instantly.

## F · Bench checks — 10 min, desk only, no cardboard

Each is one action with an obvious pass or fail.

- [ ] **Dead sensor is now visible.** Arm, then unplug one ECHO wire. Within
      ~2 s: phone notification `sensor_fault`, V15 goes dark, serial prints
      `[FAULT]`. Plug it back in → `[FAULT] cleared`.
      *This is the fail-silent fix, and it is the best 5-second demo moment.*
- [ ] **Latch survives power loss.** Trigger an alarm, then **pull the USB
      lead** and plug it back in. The device boots and comes back **still
      latched, siren running**. On the midterm build it came back silent.
- [ ] **Silence without disarming.** During an alarm, tap `Silence siren`.
      Siren stops; V7 stays lit and V4 still reads `ALARM`. Only a disarm
      clears it.
- [ ] **Siren duty cycle.** Listen for ~15 s during an alarm — 10 s on, 2 s off.
- [ ] **Stale readings.** Unplug the DHT11. After ~30 s an `#E,...,dht_stale`
      line appears and V1/V2 stop updating instead of showing a frozen value.
- [ ] **Siren test.** Tap `Siren test` while disarmed → rising sweep. Tap it
      during a real alarm → ignored on purpose.
- [ ] **Persistence slider.** Move V11 → serial prints
      `[cfg] persistence = N cycles`.
- [ ] **Escape hatch.** If a latch ever gets stuck, send `X` over serial. It
      clears the persisted armed/alarm state and disarms.

## G · Optional — the reliability numbers

Only if you want a fourth performance indicator. Everything needed is already
logged.

- [ ] Toggle your hotspot off and on 10 times, a minute or so apart. Then
      `python3 tools/analyze.py logs/*.log` and read the `wifi_down` /
      `wifi_up` event pairs for reconnect success and outage duration.
- [ ] Leave it running overnight and check `free_heap.png` for a flat line.


---

## H · Standalone mode — no Blynk, no WiFi

Every control the dashboard offers has a serial twin, so the whole device is
testable from `pio device monitor` alone. Press **`h`** for the list.

```
a  arm                     1  trial: FAST sweep
d  disarm                  2  trial: HELD panel
s  silence siren           0  trial: disturbance
?  full status block       c  CSV log on/off
X  clear stuck latch       w  WiFi/Blynk on/off
T20 <Enter>  set trip threshold to 20 cm   (bare T reports it)
N3  <Enter>  set persistence to 3 cycles   (bare N reports it)
v ping-burst   t self-test   b buzzer   m pinmap   p bring-up view
```

- [ ] Press **`w`** to go fully standalone. WiFi switches off and the
      `[net] WiFi down` retries stop. Press again to re-enable.
- [ ] Press **`?`** — one screen with everything the dashboard would show:
      state, per-sensor health and distances, trip threshold, persistence,
      streak, door, sound, environment, network, queue depth, heap, uptime.

**Notifications are visible without a phone.** Every alert prints as it fires:

```
[notify] sensor_fault  (offline — queued, 1 waiting)
[notify] intrusion_alarm  -> Blynk
```

So the §F checks all pass standalone. The dead-sensor check becomes: arm,
unplug an ECHO wire, watch for `[FAULT]` and `[notify] sensor_fault`.

### What genuinely needs Blynk

Only the graded dashboard features — the history chart, the scheduled auto-arm,
the offline-device notification, and phone delivery of the three events. None of
them block the test battery, and all of them are console work you can do in one
sitting whenever the account is ready.

**Defaults are already correct for standalone use:** trip threshold 20 cm,
persistence 3 cycles. Nothing needs to be set before you start.


---
---

# Tier 3 — sanity test and the measurable result  *(this is the one to do)*

~8 minutes total, one person, at the desk, no Blynk needed.

Replaces §C entirely: the firmware now evaluates **both** the old persistence
rule and the new coincidence window every cycle and records what each *would*
have done on its own. One run gives you the comparison. No midterm reflash.

```sh
git checkout develop/v4
pio run -t upload
pio device monitor -f log2file
```

Press `w` if there is no WiFi. Defaults are already correct: trip 20 cm,
persistence 3 cycles, window 1200 ms.

## I.1 · Sanity check — 3 min

- [ ] Press `?`. Two new lines should appear:
      ```
      window     1200    latches=000   would-fire: persistence=0 window=0
      risk       0       normal  (shadow — never triggers)
      ```
- [ ] Press `2` (held), **hold** the panel across all three beams.
      → alarm. Serial reads `[ALARM] intrusion — fired by persistence` or
      `by both`.
- [ ] Press `1` (fast), **sweep** the panel through and keep going.
      → alarm. Serial reads `[ALARM] intrusion — fired by window`.
      **This single line is the whole point of Tier 3.** If it says
      `by persistence` or `by both` every time, the old rule was already
      catching your sweeps — see §I.4.
- [ ] Press `0`, block **one** beam only and hold. → no alarm.
- [ ] Press `0`, block **two** beams only. → no alarm.
- [ ] Watch `risk` on `?` while the panel is in the beams — it should climb
      toward 70+ and drop back to near 0 when clear.

If the fast sweep does *not* alarm at all, lengthen the window: type `W2000`
and Enter, then retry. If disturbances *do* alarm, shorten it: `W800`.

## I.2 · The run — 5 min

Same one-key loop. Panel in one hand, keyboard in the other.

- [ ] **10 ×** `1` — fast sweep through all three beams, keep moving
- [ ] **10 ×** `2` — bring the panel in and hold ~2 s
- [ ] **12 ×** `0` — disturbance, must NOT alarm:
      3 × one beam · 3 × two beams · 3 × quick hand wave · 3 × knock the table

- [ ] Stop the monitor. Capture file: `logs/________________`

## I.3 · One command → every number

```sh
python3 tools/analyze.py logs/*.log
```

Paste the output here:

```
<paste>
```

What to look for, and what each block is for in the paper:

| Output block | Satisfies |
| --- | --- |
| `rule comparison` — persistence alone vs + coincidence window | **Req 2** and the "measurable improvement" clause |
| `risk score` — openings vs disturbances, separation | **Req 7** |
| confusion matrix, `by trial class`, latency | **Req 6** |
| stability, free heap, uptime | **Req 6**, extra indicators |

The headline number is:

```
rule comparison  (both evaluated in parallel, same run)
  persistence alone      ?/20
  + coincidence window   ?/20
  detections gained      ?
```

## I.4 · If `detections gained` is 0

That is a **real result, not a failure.** It means the old rule already caught
every sweep on your rig, and you should report it plainly: *"the coincidence
window produced no additional detections at the sweep speeds tested."*

Before settling for that, try once more with faster sweeps — move the panel
through as fast as you physically can, 5 trials. If the gap still does not
appear, report the negative result and lean the paper on what is already
guaranteed: the Tier 2 reliability improvements in §F, which are deterministic.

In that case also run this 5-minute before/after, which cannot come out flat:

- [ ] Flash `midterm`. Trigger an alarm, pull the USB lead, replug. Repeat 5 ×.
      Count how many come back still latched. **Expect 0/5.**
- [ ] Flash `develop/v4`. Same 5 ×. **Expect 5/5.**
- [ ] Flash `midterm`. Arm, unplug an ECHO wire, wait 30 s. Any warning? 5 ×.
      **Expect 0/5.**
- [ ] Flash `develop/v4`. Same 5 ×. **Expect 5/5**, `[notify] sensor_fault`.

## I.5 · Blynk, whenever it is ready  *(DEFERRABLE)*

Two more datastreams and one more event:

| Pin | Type | Widget | Label |
| --- | --- | --- | --- |
| V16 | Integer 0–100 | Gauge | `Risk` |
| V17 | Integer, min 300 max 4000, **default 1200** | Slider | `Window (ms)` |

- [ ] Template → Events → new event, code exactly `pre_alarm`, type Warning.
      That is the 4th of 5 event slots.
