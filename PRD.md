# Door Intrusion Detection — PRD & Build Plan

**How to use this document**
- Writing the IMRAD paper → read §1–13 (skip §14+, that's implementation detail).
- Coding the firmware → read §7, §9–11, then §14–16.

---

## 1. Context

CS Elect 3 midterm IoT project (Group 2, Smart Home theme). Deliverable is a **functional
ESP32 + Blynk prototype plus an IMRAD-format paper**. The team has explicitly prioritized
time-to-MVP over research rigor — scope is held tight on purpose, and the validation plan
(§13) is deliberately minimal rather than exhaustive. Don't over-build past what's written
here; the cuts in §15 were made on purpose.

## 2. Problem & Motivation

Standard door alarm systems trigger on every open/close event, all day, regardless of context.
Because routine daily traffic (people entering and leaving) produces constant false alarms,
users experience alert fatigue and frequently disable the system entirely — at which point it
provides zero security value. This project targets a lightweight, low-cost door intrusion
alert that only activates during a user-set **armed window**, so the alarm logic itself — not
just the user's tolerance — filters out routine use.

## 3. Goals

- Detect an unauthorized door-opening event only when the system is armed.
- Provide an early **warning** for forced-entry noise (banging, prying) even before the door
  gives way — a break-in is not guaranteed to trip the door sensor first.
- Notify the user in near-real-time via Blynk.
- Satisfy all CS Elect3 midterm rubric requirements (component list, theme, algorithm,
  functional prototype).
- Build using primarily already-owned hardware.

## 4. Non-Goals

- No facial recognition, camera, or advanced access control.
- No physical locking/deadbolt actuation (buzzer alert only, not a lock override).
- No multi-door / multi-zone support (single entry point only).
- No cloud/database logging beyond Blynk's native event history.
- No measured false-positive rate and no time-scheduled auto-arming — see §14 for why these
  are named as limitations/future work rather than built.

## 5. Target Theme & Differentiation

Theme: Smart Home. Differs from other Smart Home groups covering comfort detection and
automated lighting/cooling, and from the Safety Monitoring group's fire-detection project —
different sensor logic, different hazard type. This is intrusion detection specifically.

## 6. User Stories

- **As a resident leaving for the day**, I arm the system from the Blynk app so the door and
  sound sensors start guarding the entry point, without needing to touch the ESP32 itself.
- **As a resident at home**, I can see live door/sound/temperature/humidity readings in Blynk
  regardless of armed state, so the dashboard is useful even when not armed.
- **As a resident away from home**, if someone forces the door while I'm armed, I receive a
  push notification immediately and the on-site piezo sounds to deter the intruder and alert
  neighbors.
- **As a resident away from home**, if someone bangs on or tampers with the door before it
  actually opens, I get an early warning notification — distinct from a full alarm — even if
  the door itself never crosses the open threshold.
- **As a resident returning home**, I can silence a triggered alarm and re-disarm the system
  with one switch in the app, without needing physical access to the device.
- **As the team documenting this project**, I can point to one concrete decision algorithm
  (armed state × sensor consensus × persistence) as the rubric-required "basic algorithm."

## 7. Hardware / Bill of Materials

| Component | Role | Source |
| --- | --- | --- |
| ESP32 | Main controller | Course kit |
| 3× HC-SR05 ultrasonic | Door open/closed detection via 3-sensor consensus (additional sensor) | Confirmed in hand |
| Sound sensor (LM393, digital out) | Forced-entry noise → early warning (required component) | Confirmed in hand |
| DHT11 | Passive ambient temp/humidity telemetry (required component) | Confirmed in hand |
| Passive piezo speaker | Alarm + arm/disarm chirp actuator | Confirmed in hand |
| 5× LED + resistor | Status/health indicators (required component) | Confirmed in hand |
| 6× resistor (1kΩ/2kΩ) | Voltage dividers for 3 ECHO lines | From kit stock |
| Breadboard + jumper wires | Wiring | Course kit |

Expected new purchases: none. Everything above was confirmed in-hand before this design was
finalized.

## 8. System Design & Rationale

This is the "why" behind each non-obvious decision — reuse this prose directly in the
Methods/Discussion sections; it's the justification a reader or panel member will ask for.

**8.1 Sensing geometry.** The three ultrasonic sensors mount on the door **frame/jamb**,
aimed at the face of the **closed door leaf** at a 10–20cm standoff, spaced vertically
~50–60cm apart (roughly knee/chest/head height). A closed door is a stable, flat, nearby
target. When the door opens, the leaf swings away from all three beams and each reading jumps
from ~10–20cm to well past 50cm — **door-open is a distance increase, not a decrease.** This
was chosen over the inverse (sensors aimed into the room, closed door reading a distant far
wall) because that geometry only works if the door happens to swing toward the mounting side,
and any person or object passing between the sensor and the far wall produces the same signal
as a door opening. Anchoring the baseline to a fixed, nearby physical surface — the door
itself — is far more robust than anchoring it to "whatever happens to be far away right now."

**8.2 Why three sensors, not one.** A single ultrasonic sensor cannot distinguish "the door
opened" from "a sensor glitched" or "something briefly blocked the beam." Requiring **three
independently-mounted sensors to agree** before raising an alarm is the project's core
decision-making contribution: it trades a small amount of hardware for a large reduction in
spurious single-sensor triggers. The three sensors are fired **sequentially** (~60ms apart,
never simultaneously) because concurrent HC-SR04 ultrasonic pulses cross-talk and corrupt each
other's echo readings.

**8.3 Auto-baseline calibration.** Rather than hand-tuning a fixed distance threshold against
one specific door (which would need to be redone for any other door, mounting height, or
sensor-to-leaf gap), the firmware captures each sensor's own closed-door reading **at the
moment the user arms the system** and treats that as its personal zero-point. The alarm
condition is then framed as *deviation from self-captured baseline*, not *distance below a
hardcoded number*. This makes the system self-calibrating and portable across doors, at the
cost of one operating rule: the door must be closed at the moment of arming.

**8.4 Two-tier alert model.** The sound sensor and the ultrasonic consensus feed two different
alert tiers, not one shared trigger:
- **Sound alone → warning** (Blynk event/notification, LED, no siren).
- **Ultrasonic consensus (armed) → full alarm** (siren + notification), independent of sound.

Sound is deliberately **never a precondition** for the full alarm. A silent, careful break-in
that never makes a loud noise must still trigger the alarm from door movement alone — an
`AND` between sound and door would defeat the project's own purpose. Sound is also
deliberately **never sufficient on its own** to trigger the siren, because incidental noise
(a dropped object, a slammed cabinet elsewhere in the room) would reproduce exactly the alert
fatigue problem this project exists to solve. Framed for the paper: sound is the **pre-entry**
signal, ultrasonic consensus is the **confirmed-entry** signal, and they sit on two different
severities on purpose.

**8.5 Latching and timeout.** Once triggered, the alarm state latches — it does not clear
itself if the door swings shut again, because an intruder who closes the door behind them
should not silence their own alarm. Only a Blynk disarm clears it. The piezo itself is capped
at **3 minutes** of continuous sound (the latched *state*, shown in the app, persists beyond
that) purely for demo and neighbor-tolerance practicality — an unbounded siren is a liability
during a live class demonstration.

**8.6 Offline resilience.** The system does not depend on connectivity to detect an intrusion
or sound the alarm — WiFi/Blynk failure must not equal security failure. The armed/disarmed
state is persisted to the ESP32's non-volatile storage (NVS) so a power blip doesn't silently
disarm the house, and the Blynk connection is handled non-blockingly so a dropped network
never stalls the sensor loop.

## 9. Wiring & Pinout

**Physical mounting (side view, door closed):**
```
  DOOR FRAME (sensors fixed here)         DOOR LEAF (swings away from sensors)
  ┌───────────────────────┐
  │ [S1]═══▶  ~150cm (head)│   10-20cm    ┌─────────────┐
  │ [S2]═══▶  ~100cm (chest)│───gap when──▶│             │
  │ [S3]═══▶  ~ 40cm (knee) │   closed     │             │
  │                         │              └─────────────┘
  │  ESP32 + dividers       │
  │  mounted on frame       │        hinge ►
  └───────────────────────┘
```
Open door: all three readings jump from ~10–20cm to 50cm+ (§8.1). Sequential firing order is
S1 → S2 → S3, ~60ms apart, repeating every 300ms.

**Electrical block diagram:**
```
 HC-SR04 x3            [1kΩ]──┬──[2kΩ]         ESP32 DevKit             Blynk Cloud
 ECHO (5V) ────────────┘      └──GND    ───►  GPIO 34/35/36                 │
 TRIG (3.3V) ◄─────────────────────────────── GPIO 25/26/27                 │
                                                                             │
 Sound sensor DO ──────────────────────────►  GPIO 39                      │
 DHT11 data     ◄────────────────────────►   GPIO 18            WiFi ──────┘
 LED 1-5        ◄──────────────────────────  GPIO 16,17,21,22,19    (2.4GHz only)
 Passive piezo  ◄──────────────────────────  GPIO 23 (tone())          │
                                                                    Blynk App
                                                          (arm switch, delta slider,
                                                           live dashboard, events)
```

**GPIO table:**

| Signal | GPIO | Note |
| --- | --- | --- |
| ECHO 1/2/3 | 34, 35, 36 | input-only; **each behind a 1k+2k divider** — ECHO idles at 5V and ESP32 is not 5V tolerant |
| SOUND DO | 39 | input-only; power the module from 3.3V so DO is already 3.3V, no divider needed |
| TRIG 1/2/3 | 25, 26, 27 | individual, never shared (sequential firing) |
| DHT11 | 18 | bidirectional |
| LED 1–5 | 16, 17, 21, 22, 19 | |
| PIEZO | 23 | passive, driven with `tone()` |

14 pins total. Avoids strapping pins (0/2/12/15), flash pins (6–11), and UART0 (1/3).

## 10. Blynk Virtual Pin / Widget Specification

Free tier allows 50 datastreams and 5 events per template — comfortably within budget.

| Pin | Widget | Type | Notes |
| --- | --- | --- | --- |
| V0 | Arm/Disarm switch | int 0/1 | writing 1 triggers baseline capture (§8.3); requires door closed |
| V1 | Temperature | double | DHT11, streamed continuously regardless of armed state |
| V2 | Humidity | double | DHT11, streamed continuously regardless of armed state |
| V3 | Door state | int (0 closed / 1 open) | reflects live consensus, active regardless of armed state |
| V4 | System state | string | `DISARMED` / `ARMED` / `ALARM` |
| V5 | Deviation delta (cm) | slider | the one runtime-tunable threshold; persistence count is a firmware constant |

Events (Blynk notifications): `intrusion_alarm`, `door_noise_warning`.

## 11. Algorithm

This is the rubric-required "basic algorithm for automation/decision-making." In prose: while
armed, the system treats the door as open only when all three ultrasonic sensors agree it has
moved away from its own closed-door baseline, and only after that agreement holds for several
consecutive readings (rejecting single-cycle glitches) — at which point it raises a latched
alarm that only a deliberate disarm can clear. A loud sound alone is downgraded to a
non-blocking warning rather than the full alarm, since it is corroborating, not conclusive.

```
on arm (Blynk V0 = 1):
  require door closed, all 3 sensors healthy
  take 5 readings per sensor, median → baseline[i]
  persist armed=true to NVS

every 300ms:
  for i in 0..2:
    ping sensor i, wait ~60ms before next        # sequential, avoids crosstalk
    healthy[i] = echo returned within timeout
    open[i]    = (dist[i] - baseline[i]) > DELTA_CM

  LED1..3 = off if !healthy[i], blink if open[i], else solid
  LED4    = soundDO
  LED5    = solid disarmed / slow blink armed / fast blink alarm

  if armed and not alarm:
    open_streak = all(open) ? open_streak + 1 : 0
    if open_streak >= PERSIST_N:
      alarm = true (LATCHED); siren_start = millis()
      Blynk.logEvent("intrusion_alarm")
    else if soundDO and not any(open):
      Blynk.logEvent("door_noise_warning")        # rate-limited, no siren

  piezo = alarm and (millis() - siren_start < 3min)
  # sound-warning path muted whenever alarm is latched — the piezo would trip our own mic

on disarm (Blynk V0 = 0):
  alarm = false; open_streak = 0; persist armed=false
```

## 12. Rubric Compliance Checklist

| Requirement | Satisfied by |
| --- | --- |
| ESP32 as main microcontroller | §7, §9 |
| Blynk for monitoring/control | §10 |
| 5 LEDs integrated | §9 LED1–5 |
| Sound sensor integrated | §8.4 warning tier |
| Temperature & humidity sensor | DHT11, V1/V2 |
| 1 additional sensor of choice | 3× ultrasonic, §8.1–8.2 |
| 1 actuator of choice | Passive piezo, §8.5 |
| 1 basic algorithm for decision-making | §11 |
| ESP32 ↔ Blynk communication demonstrated | §10, arm switch + live telemetry + events |
| Functional prototype | §16 build order |

## 13. Testing & Validation Plan (source for IMRAD Methods/Results)

Per the team's MVP-over-rigor decision (§1), only two trials are run:

1. **Detection rate** — arm the system, open the door 30 times under normal conditions,
   record true-positive percentage (alarm correctly latches).
2. **Notification latency** — 20 timed samples from the moment the door crosses the open
   threshold to the moment the Blynk push notification is received; report mean and standard
   deviation.

No false-positive rate and no disarmed-state control trial are collected — see §14 for how to
frame that honestly in the Discussion rather than implying it was measured.

## 14. Known Limitations & Future Work (source for IMRAD Discussion)

State these explicitly rather than leaving them implicit:

- **No measured false-positive rate.** The project's motivating claim is fewer false alarms
  during routine use, but Results (§13) does not include a trial measuring false positives —
  only detection rate and latency were collected under the team's time-to-MVP decision. The
  false-positive reduction claim rests on the armed-window *design* (§2, §8.4), not on
  collected data. State this plainly rather than implying it was measured.
- **Manual arming only.** The system only protects the user during windows they remember to
  arm. A time-scheduled auto-arm (via Blynk Automation) was considered and deliberately
  deferred — name it explicitly as future work.
- **Sound sensor does not escalate sensitivity.** A design where a detected bang temporarily
  lowers the persistence requirement (§11's `PERSIST_N`) for ~30 seconds was considered and
  declined for MVP scope. Worth naming as a natural extension of the two-tier model (§8.4).
- Single entry point, no lock actuation, no camera/facial recognition — see §4 Non-Goals.

## 15. MVP Scope Cuts

Deliberately removed to shorten the build's critical path — noted so nobody re-adds them
thinking they were overlooked:

- **Sound sensor analog output (AO)** — would only have fed a noise-floor measurement that
  §13 no longer collects. Digital output alone drives the warning tier.
- **Temporal-coherence window** between sensor trigger times — "all 3 deviate, sustained N
  cycles" (§11) is already a complete decision rule without it.
- **Second Blynk slider for persistence count** — auto-baseline (§8.3) removed most of the
  need for hand-tuning; one delta slider (V5) is enough.
- **NVS persistence for slider values** — Blynk retains datastream values server-side and
  re-syncs on reconnect; only the armed flag needs local persistence.
- Ablation testing, false-positive trials, time-scheduled arming, sound-triggered sensitivity
  escalation — all named in §13/§14 as consciously deferred, not forgotten.

---

## 16. Engineering Build Plan (for the coding agent)

The repo is currently a bare PlatformIO scaffold: [platformio.ini](platformio.ini) targets
`esp32dev` + Arduino framework, and [src/main.cpp](src/main.cpp) is still the generated stub.
Everything below is greenfield. Not a git repo.

### Files

- **[platformio.ini](platformio.ini)** — **pin `platform = espressif32@<version>`** (the LEDC
  API used by `tone()` changed between Arduino-ESP32 core 2.x and 3.x; a floating toolchain
  that breaks pre-demo is avoidable). Add `lib_deps`: `blynkkk/Blynk`,
  `adafruit/DHT sensor library`, `adafruit/Adafruit Unified Sensor`.
- **`include/secrets.h`** (new) — WiFi SSID/pass + `BLYNK_AUTH_TOKEN`. Not a git repo, so no
  gitignore or example-file ceremony needed.
- **[src/main.cpp](src/main.cpp)** — single file is correct for MVP scope. Sections: pin
  constants, armed-flag NVS via `Preferences`, sequential ultrasonic read, DHT11 (min 2s
  interval), LED driver, buzzer behind a small `sirenOn()/sirenOff()` wrapper, `BlynkTimer`
  tasks, `BLYNK_WRITE(V0/V5)` handlers.

**Non-blocking Blynk is load-bearing** — use `Blynk.config()` + `Blynk.connect(timeout)`,
never blocking `Blynk.begin()` in `setup()`, and never let `Blynk.run()` stall the sensor loop.

### Build order

**Blynk goes first, not last.** WiFi + auth is the riskiest unknown (ESP32 is 2.4GHz-only,
campus WiFi is often 5GHz or captive-portal). Discovering that on day 3 is a disaster;
discovering it in hour 1 costs nothing.

1. **Blynk hello-world** — ESP32 connects, one datastream toggles from the app. Nothing else.
2. Wire and verify **one** ultrasonic with its divider; print distance to serial.
3. Add sensors 2 and 3 with sequential firing; confirm no crosstalk (stable readings).
4. LEDs + passive piezo (`tone()` two-tone siren, short chirp on arm/disarm).
5. Baseline capture + deviation + persistence, tested over serial, **still offline**.
6. Sound sensor DO, DHT11.
7. Wire it all into Blynk — remaining datastreams, arm switch, delta slider, events,
   armed-flag NVS.

### Verification

- `pio run` builds; `pio run -t upload && pio device monitor` shows per-sensor distance,
  health, baseline, and state on every cycle.
- **Bench:** hold a flat board at ~15cm, arm, remove it — all 3 LEDs blink in sequence,
  siren fires, Blynk notification arrives. Disarm from app clears the latch.
- **Sensor fault:** unplug one ECHO while armed → its LED goes off, no alarm fires.
- **Offline:** kill WiFi, power-cycle → armed flag survives, siren still fires locally,
  notifications resume on reconnect.
- **Demo-day risk:** ESP32 is 2.4GHz-only and campus WiFi is often 5GHz or captive-portal —
  **test on a phone hotspot and bring one.**
