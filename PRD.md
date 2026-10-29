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
- Satisfy all CS Elect 3 midterm rubric requirements (component list, theme, algorithm,
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
| ESP32-S3-WROOM-1 (DevKitC-1) | Main controller | Course kit |
| 3× HC-SR04/SR05 ultrasonic | Door open/closed detection via 3-sensor consensus (additional sensor) | Confirmed in hand |
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

**8.1 Sensing geometry.** The three ultrasonic sensors are mounted **on top of the
door frame at roughly 45 degrees, aimed down at the door leaf**. With the door closed
each beam clears the leaf and lands on whatever is beyond it — the nearest wall, the
floor, or nothing in range — so every sensor reads long. As the door opens the leaf
swings **into** the beam and intercepts it close to the sensor, so every reading
**drops sharply** — **door-open is a distance decrease.**

A sensor reads open once its live distance falls to the trip threshold, which in this
installation is **0-20 cm**. The known trade-off, carried into §14: anything else that
crosses a beam produces the same "closer" signal, which is why detection is gated on
**all three** beams agreeing plus a persistence count (§8.2, §11) rather than any
single beam.

If the far wall is beyond the sensor's ~4.3m range the closed-door beam returns no echo. The
firmware handles this by reading the ECHO line's edges directly: a live sensor always asserts
ECHO shortly after its trigger pulse (holding it high in proportion to distance, or high for
its own internal maximum when nothing is in range), whereas a faulty or disconnected sensor
never asserts it at all. So "nothing in range" is reported as a fixed ceiling
(`DIST_MAX_CM` ≈ 431cm, an armable baseline) while a genuine fault stays flagged unhealthy —
the two are no longer conflated as a bare timeout. A leaf swinging into a beam still drops
that reading by hundreds of cm, so the ceiling baseline detects opening just as well.

**8.2 Why three sensors, not one.** A single ultrasonic sensor cannot distinguish "the door
opened" from "a sensor glitched" or "something briefly blocked the beam." Requiring **three
independently-mounted sensors to agree** before raising an alarm is the project's core
decision-making contribution: it trades a small amount of hardware for a large reduction in
spurious single-sensor triggers. The three sensors are fired **sequentially** (~60ms apart,
never simultaneously) because concurrent HC-SR04 ultrasonic pulses cross-talk and corrupt each
other's echo readings.

**8.3 Threshold, not auto-calibration.** An earlier draft of this design captured each
sensor's closed-door reading at arm time and framed the alarm as *deviation from that
self-captured baseline* — workable for a sensor aimed at the closed leaf a fixed 10–20cm
away, where the baseline is always a small, predictable number. It breaks down for the
mount actually built here (§8.1): the closed-door reading is whatever happens to lie
beyond the leaf, anywhere from tens of centimetres to several metres, so a
deviation sized to be meaningful at one baseline is either swamped by real readings or
triggered by noise at another — this was tried on the bench and confirmed unworkable.

The firmware instead uses an absolute, physically-measured **trip distance**
(`thresholdCm`, Blynk V5): at install time, measure how far the door leaf sits from a sensor
once it has swung far enough into that sensor's beam to count as "opening," and set the
slider to that number. A sensor reads open the moment its live distance crosses the
threshold — no baseline comparison at all. At arm time the firmware still takes a
closed-door reading per sensor (median of 5), but only as a **health and sanity check**: it
confirms the sensor is alive, and refuses to arm if a sensor's closed-door reading already
sits past the threshold (a sign the slider or the aim needs adjusting, since that channel
would read "open" before anyone touches the door). Named honestly for the paper: this is
less "self-calibrating" than a baseline-deviation design would be, at the cost of one
manual measurement per installation — but that measurement is far more legible and
debuggable than tuning a delta against an arbitrary, invisible baseline (§14).

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

**Physical mounting (side view):**
```
      top of door frame
   ┌───[S1][S2][S3]───┐      sensors on the frame header, angled ~45 deg down
   │      ╲  ╲  ╲     │
   │       ╲  ╲  ╲    │      CLOSED: beams clear the leaf, land on the wall /
   │ leaf   ╲  ╲  ╲   │              floor / nothing in range  -> reads LONG
   │(closed) ╲  ╲  ╲  │
   │          ▼  ▼  ▼ │      OPENING: the leaf swings INTO the beams and
   └──────────────────┘               intercepts them  -> reads 0-20 cm
```
Closed door: each beam clears the leaf and reads long.
Open door: the leaf swings into the beams and each reading **drops** into the trip
band (§8.1). The three sit across the top of the frame; firing order S1 → S2 → S3,
~60ms apart, repeating every 300ms. A far wall beyond the sensor's ~4.3m range is fine — the
firmware reports it as a fixed ceiling (`DIST_MAX_CM`) and still arms; only a genuinely
faulty sensor blocks arming (§8.1). `v` in the serial monitor shows which of the three:
a cm value, `> 431 cm` (far, OK), or `FAULT`.

**Electrical block diagram:**
```
 HC-SR04 x3            [1kΩ]──┬──[2kΩ]      ESP32-S3-DevKitC-1          Blynk Cloud
 ECHO (5V) ────────────┘      └──GND    ───►  GPIO 4/5/6                    │
 TRIG (3.3V) ◄─────────────────────────────── GPIO 15/16/17                 │
                                                                             │
 Sound sensor DO ──────────────────────────►  GPIO 7                       │
 DHT11 data     ◄────────────────────────►   GPIO 18            WiFi ──────┘
 LED 1-5        ◄──────────────────────────  GPIO 8,9,10,11,12     (2.4GHz only)
 Passive piezo  ◄──────────────────────────  GPIO 13 (tone())          │
                                                                    Blynk App
                                                          (arm switch, threshold slider,
                                                           live dashboard, events)
```

**GPIO table (ESP32-S3-WROOM-1):**

| Signal | GPIO | Note |
| --- | --- | --- |
| ECHO 1/2/3 | 4, 5, 6 | **each behind a 1k+2k divider** — ECHO idles at 5V and the S3 is not 5V tolerant |
| SOUND DO | 7 | power the module from 3.3V so DO is already 3.3V logic, no divider needed |
| TRIG 1/2/3 | 15, 16, 17 | individual, never shared (sequential firing) |
| DHT11 | 18 | bidirectional |
| LED 1–5 | 8, 9, 10, 11, 12 | each via ~330 Ω |
| PIEZO | 13 | passive, driven with `tone()` (LEDC) |

14 pins, all in the S3's universally-safe low band. The S3 has **no input-only pins** — every
GPIO is bidirectional — so the divider on ECHO is now purely for the 5 V level, not a pin
constraint. Avoids: strapping pins (0/3/45/46), native-USB/JTAG (19/20), the UART0 console
(43/44 — the "UART" USB-C port used for the serial monitor), SPI-flash/PSRAM (26–37), and the
on-board RGB LED (38/48). ADC1 on the S3 is GPIO 1–10, so if anything analog is ever re-added
(e.g. sound AO) it lands there; GPIO 1/2/14/21 are left free as spares.

## 10. Blynk Virtual Pin / Widget Specification

Free tier allows 50 datastreams and 5 events per template — comfortably within budget.

| Pin | Widget | Type | Notes |
| --- | --- | --- | --- |
| V0 | Arm/Disarm switch | int 0/1 | writing 1 triggers baseline capture (§8.3); requires door closed |
| V1 | Temperature | double | DHT11, streamed continuously regardless of armed state |
| V2 | Humidity | double | DHT11, streamed continuously regardless of armed state |
| V3 | Door state | int (0 closed / 1 open) | live 3-sensor consensus, **the door right now** — not the alarm; an intruder closing the door behind them clears this while the alarm stays latched. Active regardless of armed state |
| V4 | System state | string | `DISARMED` / `ARMED` / `ALARM` |
| V5 | Trip threshold (cm) | slider, 5–200 | absolute distance, physically measured per install (§8.3) — not a deviation from baseline. The one runtime-tunable value; persistence count is a firmware constant. Set the datastream **default value = 30** so the on-connect sync lands sane |
| V6 | Sound | int 0/1 | LED widget. `1` if the LM393 DO fired at least once in the last ~1 s push window — the dashboard presence for the sound sensor (its only other surface is the `door_noise_warning` event) |
| V7 | Alarm | int 0/1 | LED widget, solid. `1` while the alarm is latched. Blynk LED widgets can't blink — the fast-blink alarm indicator is hardware LED5; this is the app's unmistakable latched-alarm light, paired with V4 |

Events (Blynk notifications): `intrusion_alarm`, `door_noise_warning`.

Widget notes: V4 reads best as a Label/Value-Display with conditional colour (green
DISARMED / amber ARMED / red ALARM). V3, V6, V7 are LED widgets. V0 is the only control that
writes a pin (the device writes V0 back to sync after arm/disarm/boot — don't add a second
control on it).

## 11. Algorithm

This is the rubric-required "basic algorithm for automation/decision-making." In prose: while
armed, the system treats the door as open only when all three ultrasonic sensors' live
readings have crossed a physically-measured trip threshold (§8.3 — the distance at which the
swinging leaf intercepts each beam), and only after that agreement holds for several
consecutive readings (rejecting single-cycle glitches) — at which point it raises a latched
alarm that only a deliberate disarm can clear. A loud sound alone is downgraded to a
non-blocking warning rather than the full alarm, since it is corroborating, not conclusive.

```
on arm (Blynk V0 = 1):
  require door closed, all 3 sensors healthy
  take 5 readings per sensor, median → baseline[i]   # health + sanity check only, not compared below
  reject arm if any baseline[i] already past THRESHOLD_CM
  persist armed=true to NVS

every 300ms:
  for i in 0..2:
    ping sensor i, wait ~60ms before next        # sequential, avoids crosstalk
    healthy[i] = ECHO asserted after trigger      # faulty/disconnected => not healthy
    dist[i]    = pulse width in cm, or DIST_MAX_CM if ECHO never fell (nothing in range)
    open[i]    = dist[i] <= THRESHOLD_CM          # leaf swung into the beam (§8.1)
                 # inverse mount: dist[i] >= THRESHOLD_CM, via DOOR_OPEN_IS_FARTHER

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
- **Trip threshold is manually measured, not self-calibrating.** V5 (`thresholdCm`) must be
  physically measured and set per installation (§8.3) — a deviation-from-baseline design would
  have auto-calibrated at arm time, but that model didn't hold up for a mount whose baseline is
  an arbitrary far-wall distance (§8.3). Re-measure and re-set the slider if the sensors are
  remounted or the doorway changes.
- **Beam-crossing ambiguity.** Any object that enters a beam — a hand, a person passing
  directly beneath the sensors — produces the same "closer" signal as the door leaf. The
  three-beam consensus plus persistence count mitigates but does not eliminate this. A
  cross-sensor timing/depth check is the natural hardening step.
- Single entry point, no lock actuation, no camera/facial recognition — see §4 Non-Goals.

## 15. MVP Scope Cuts

Deliberately removed to shorten the build's critical path — noted so nobody re-adds them
thinking they were overlooked:

- **Sound sensor analog output (AO)** — would only have fed a noise-floor measurement that
  §13 no longer collects. Digital output alone drives the warning tier.
- **Temporal-coherence window** between sensor trigger times — "all 3 deviate, sustained N
  cycles" (§11) is already a complete decision rule without it.
- **Second Blynk slider for persistence count** — one threshold slider (V5, §8.3) plus the
  fixed `PERSIST_N` constant is enough; a second tunable wasn't worth the added surface.
- **NVS persistence for slider values** — Blynk retains datastream values server-side and
  re-syncs on reconnect; only the armed flag needs local persistence.
- Ablation testing, false-positive trials, time-scheduled arming, sound-triggered sensitivity
  escalation — all named in §13/§14 as consciously deferred, not forgotten.

---

## 16. Engineering Build Plan (for the coding agent)

Target board: **ESP32-S3-WROOM-1 (DevKitC-1)**, `board = esp32-s3-devkitc-1`. Serial is UART0
— use the USB-C port silk-screened **"UART"** (the CP210x bridge), not the native-USB port.
Git repo (`origin` on GitHub) — `include/secrets.h` is gitignored; copy
`include/secrets.h.example` to `include/secrets.h` and fill in real values.

### Files

- **[platformio.ini](platformio.ini)** — `platform` pinned to `espressif32@7.0.1` (ships
  Arduino-ESP32 core 2.0.17; the LEDC API used by `tone()` changed in core 3.x, and a floating
  toolchain that breaks pre-demo is avoidable). **One environment**, `esp32-s3-devkitc-1`,
  one binary — `pio run -t upload` / the PlatformIO → button just works, no `-e`.
  `build_flags = -DARDUINO_USB_CDC_ON_BOOT=0`. `lib_deps`: `blynkkk/Blynk@1.3.5`,
  `adafruit/DHT sensor library@1.4.7`, `adafruit/Adafruit Unified Sensor@1.1.15`
  (exact-pinned for demo-day reproducibility).
- **[include/pins.h](include/pins.h)** (new) — the GPIO map + `SOUND_ACTIVE_LOW`.
- **[include/secrets.h](include/secrets.h)** (new) — WiFi SSID/pass, `BLYNK_AUTH_TOKEN`,
  `BLYNK_TEMPLATE_ID`, `BLYNK_TEMPLATE_NAME`, included before the Blynk header. These live
  here **only** — defining them again in `main.cpp` silently overrides this file.
- **[src/main.cpp](src/main.cpp)** — single file. Sections: Blynk datastream defines, timing
  constants, armed-flag NVS via `Preferences`, sequential ultrasonic read, DHT11 (min 2s
  interval), LED driver, siren behind a `sirenOff()`/`serviceSiren()` wrapper, `BlynkTimer`
  tasks, `BLYNK_WRITE(V0/V5)` + `BLYNK_CONNECTED()` handlers. Arming is Blynk-switch-only per
  §8.3; serial-monitor arm/disarm (`a`/`d`) sits behind `ALLOW_SERIAL_CONTROL` as a bench aid
  — set it `false` for a spec-clean build. Hardware bring-up checks are also serial commands
  (`h` lists them): **`p` toggles the serial output between deployment view (`[cyc]` + `[env]`
  lines) and a bring-up view** (one hwcheck-style `US/SND/DHT` status line + LED1-3 = raw echo
  health); `v` one-shot verbose ping burst, `t` LED/piezo self-test, `b` buzzer, `m` pin map,
  `?` state. (This replaces the former standalone `hw_check.cpp` / second env.) A boot
  Easter egg — LED sweep + music-synced LED dance + piezo jingle — runs once first thing in
  `setup()`; `PLAY_BOOT_EGG = false` reduces it to a 0.3s LED blip.

**Non-blocking Blynk is load-bearing** — use `Blynk.config()` + a `Blynk.run()` gated on
`WiFi.status() == WL_CONNECTED`, never blocking `Blynk.begin()` in `setup()`, and never let
`Blynk.run()` stall the sensor loop.

### Build order

**Blynk goes first, not last.** WiFi + auth is the riskiest unknown (the ESP32-S3 is
2.4GHz-only, campus WiFi is often 5GHz or captive-portal). Discovering that on day 3 is a
disaster; discovering it in hour 1 costs nothing.

0. **Bring-up checks first** — flash, open the monitor, send `t` (LED/piezo self-test) and
   `p` (toggle to the bring-up view). Confirm every LED lights, the piezo chirps, and each
   sensor reports sane cm before relying on the decision logic. `p` again to return to `[cyc]`.
1. **Blynk hello-world** — S3 connects, one datastream toggles from the app. Nothing else.
2. Wire and verify **one** ultrasonic with its divider; print distance to serial.
3. Add sensors 2 and 3 with sequential firing; confirm no crosstalk (stable readings).
4. LEDs + passive piezo (`tone()` two-tone siren, short chirp on arm/disarm).
5. Threshold check + persistence, tested over serial, **still offline** (§8.3).
6. Sound sensor DO, DHT11.
7. Wire it all into Blynk — remaining datastreams, arm switch, threshold slider, events,
   armed-flag NVS.

### Verification

- `pio run` builds; `pio run -t upload && pio device monitor` (or the → / 🔌 buttons) shows
  per-sensor distance, health, baseline, and state on every `[cyc]` line plus `[env]` DHT.
- **Wiring:** boot LED sweep hits all 5; `t` chirps the piezo; `v` reports cm (not `NO ECHO`)
  for all three with a target in range; the bring-up view (`p`) shows plausible `°C`/`%`.
- **Bench:** with nothing closer than the threshold in front of any sensor, arm (§8.3); then
  bring a flat board within the threshold distance of all three sensors and hold it — all 3
  LEDs blink in sequence, siren fires, Blynk notification arrives. Disarm from app clears the
  latch.
- **Sensor fault:** unplug one ECHO while armed → its LED goes off, no alarm fires.
- **Offline:** kill WiFi, power-cycle → armed flag survives, siren still fires locally,
  notifications resume on reconnect.
- **Demo-day risk:** the S3 is 2.4GHz-only and campus WiFi is often 5GHz or captive-portal —
  **test on a phone hotspot and bring one.**
