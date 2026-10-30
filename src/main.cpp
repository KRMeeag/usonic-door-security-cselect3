// ===========================================================================
// Door Intrusion Detection — ESP32-S3 firmware
//
// A door alarm that only fires inside a user-armed window, so routine daily
// entry/exit does not cause alert fatigue.
//
// Sensing geometry: 3 ultrasonic sensors on TOP of the door frame, angled
// ~45 degrees down at the door leaf. Closed door -> the beam clears the leaf
// and lands on the wall/floor/nothing beyond it, so the sensor reads long.
// Opening leaf swings INTO the beam -> the reading DROPS into the trip band
// (0-20 cm in this installation). The three are fired sequentially ~60 ms
// apart to avoid crosstalk.
//
// Threshold, not deviation: V5 (`thresholdCm`) is an ABSOLUTE distance in cm.
// A sensor reads open once its live distance falls to or below it, full stop —
// no comparison to a per-sensor baseline. This installation trips at 0-20 cm.
// Deliberate switch away from "deviation from an auto-captured baseline": the
// closed-door reading is just whatever lies beyond the leaf (tens of cm to
// several metres, entirely installation-dependent), so a delta sized relative
// to it was either swamped by real readings or arbitrary.
//
// Arm  : health-checks each sensor (median of 5) and sanity-checks that its
//        closed-door reading doesn't already cross the threshold (door must
//        be shut).
// Alarm: all 3 sensors past the threshold for PERSIST_N consecutive cycles,
//        OR all 3 tripped inside the coincidence window (Tier 3). LATCHES
//        until disarmed from the app.
//        Piezo is capped at 3 minutes; the latched state stays visible.
// Sound: LM393 DO is a WARNING tier only — never a precondition for the alarm
//        (break-ins can be silent). Muted while the alarm is latched.
//
// Fully autonomous offline: non-blocking Blynk, millis-based BlynkTimer loop,
// pings scheduled one per slot rather than behind delay(), armed flag
// persisted to NVS. Siren still fires with no network; the intrusion
// notification is queued and sent on reconnect.
// ===========================================================================

#define BLYNK_PRINT Serial
#include "secrets.h"          // WiFi + BLYNK_TEMPLATE_ID/NAME/AUTH_TOKEN — must precede Blynk

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_system.h>   // esp_reset_reason()
#include <esp_timer.h>    // esp_timer_get_time() — ISR-safe clock for the sound edge counter
#include <BlynkSimpleEsp32.h>
#include <DHT.h>

#include "pins.h"             // GPIO map + SOUND_ACTIVE_LOW + LED_SOUND / LED_MODE

// --------------------------------------------------------------------------
// Blynk datastreams
// --------------------------------------------------------------------------
#define VPIN_ARM    V0   // int 0/1   — Arm/Disarm switch (only way to arm)
#define VPIN_TEMP   V1   // double    — Temperature (DHT11)
#define VPIN_HUM    V2   // double    — Humidity (DHT11)
#define VPIN_DOOR   V3   // int 0/1   — Door state (1 = all 3 sensors open)
#define VPIN_STATE  V4   // string    — DISARMED / ARMED / ALARM
#define VPIN_THRESH V5   // slider    — Trip distance in cm (PERSIST_N is a constant)
#define VPIN_SOUND  V6   // int 0/1   — noise detected since the last push (~1 s window)
#define VPIN_ALARM  V7   // int 0/1   — alarm latched (solid; pairs with V4)
#define VPIN_TRIAL_F V8  // button    — trial: panel swept through FAST  (Req 6)
#define VPIN_TRIAL_H V9  // button    — trial: panel HELD in the beams    (Req 6)
#define VPIN_TRIAL_N V10 // button    — trial: disturbance, must NOT alarm (Req 6)
#define VPIN_PERSIST V11 // slider 2-8 — consecutive all-open cycles before alarm
#define VPIN_TEST    V12 // button    — siren self-test (ignored during a real alarm)
#define VPIN_SILENCE V13 // button    — silence the siren WITHOUT clearing the latch
#define VPIN_HEAP    V14 // int       — free heap, bytes (soak / leak evidence)
#define VPIN_HEALTH  V15 // int 0/1   — 1 = all three sensors healthy
#define VPIN_RISK    V16 // int 0-100 — risk score (shadow: reported, never triggers)
#define VPIN_WINDOW  V17 // slider ms — coincidence window length

// (GPIO map lives in include/pins.h)

// --------------------------------------------------------------------------
// Timing / thresholds
// --------------------------------------------------------------------------
static const uint32_t CYCLE_MS         = 300;     // main sensor + decision cycle
static const uint32_t UI_MS            = 50;      // LED blink / siren / sound sampling
static const uint32_t DHT_MS           = 2000;    // DHT11 needs >= 2 s between reads
static const uint32_t PUBLISH_MS       = 1000;    // push telemetry to Blynk
static const uint32_t NETCHECK_MS      = 5000;    // WiFi reconnect watchdog

static const uint16_t PING_GAP_MS      = 60;      // spacing between the 3 pings (anti-crosstalk)
static const uint32_t ECHO_START_US    = 6000;    // ECHO must assert within 6 ms of trigger, else sensor is faulty
static const uint32_t ECHO_MAX_US      = 25000;   // ~4.3 m; still HIGH past this => nothing in range
static const float    DIST_MAX_CM      = ECHO_MAX_US / 58.0f;  // ~431 cm — the value reported for "far wall"
static const uint8_t  BASELINE_SAMPLES = 5;       // readings per sensor at arm time -> median
static const uint8_t  PERSIST_N        = 3;       // consecutive all-open cycles -> alarm (~0.9 s)

// Threshold direction. false = this build: sensors on top of the frame angled
// down at the leaf, closed door reads long, an opening leaf swings into the
// beam so the reading FALLS -> door-open = dist <= thresholdCm. This is the
// mount that is wired and working. SETTLED — do not flip it.
static const bool     DOOR_OPEN_IS_FARTHER = false;

static const float    THRESH_DEFAULT_CM = 20.0f;   // trip band is 0-20 cm; V5 overrides on connect
static const float    THRESH_MIN_CM     = 5.0f;
static const float    THRESH_MAX_CM     = 200.0f;

static const uint32_t SIREN_MAX_MS     = 3UL * 60UL * 1000UL;  // piezo capped at 3 minutes
static const uint16_t SIREN_SWEEP_MS   = 350;    // two-tone alternation period
static const uint16_t SIREN_LO_HZ      = 800;
static const uint16_t SIREN_HI_HZ      = 1200;

static const uint32_t WARN_COOLDOWN_MS = 60000;  // door_noise_warning rate limit
static const uint16_t SOUND_LED_HOLD_MS = 250;   // stretch momentary DO pulses so LED4 is visible

// --- Tier 3: coincidence window + risk score -----------------------------
//
// The persistence rule asks all three beams to read open AT THE SAME TIME for
// `persistN` consecutive cycles — roughly 600-700 ms of continuous agreement.
// The panel crosses the three beams in sequence, so a fast sweep can break all
// three without ever satisfying that.
//
// The coincidence window asks a weaker, better-matched question: did each beam
// trip at SOME point inside the last `windowMs`? Each sensor latches its own
// trigger with a time-to-live; the rule closes when all three latches are
// simultaneously live.
//
// The two rules are OR-ed, so midterm behaviour is a strict subset — detection
// cannot get worse, only the false-positive count can. BOTH are evaluated every
// cycle including after the latch, and what each WOULD have done is recorded
// separately (tmask bits 3 and 4). One run therefore yields the before/after
// comparison directly, with no second session on the old firmware.
static const uint32_t WINDOW_DEFAULT_MS = 1200;
static const uint32_t WINDOW_MIN_MS     = 300;
static const uint32_t WINDOW_MAX_MS     = 4000;
static const uint32_t SOUND_RECENT_MS   = 30000;  // "a bang just happened" horizon
static const uint8_t  RISK_ELEVATED     = 30;     // band edges for the shadow score
static const uint8_t  RISK_CRITICAL     = 70;
static const uint32_t PREALARM_COOLDOWN_MS = 60000;

// --- Tier 2 safeguards ---------------------------------------------------
static const uint8_t  FAULT_CONFIRM_N  = 5;      // consecutive bad cycles before calling it a fault
static const uint32_t DHT_STALE_MS     = 30000;  // past this with no good read, readings are invalid
static const uint32_t SIREN_ON_MS      = 10000;  // siren duty cycle: on...
static const uint32_t SIREN_OFF_MS     = 2000;   // ...then a gap. Cuts average current and ear fatigue.
static const uint8_t  EVENT_MAX_PER_HR = 20;     // hard cap on Blynk notifications; the server
                                                 // rate-limits and SILENTLY DROPS the excess
static const uint8_t  EVQ_N            = 12;     // offline notification ring depth

// --- Tier 4: non-blocking sensing ----------------------------------------
//
// The midterm cycle pinged all three sensors back to back with two blocking
// delay(PING_GAP_MS) calls between them, stalling the UI tick, Blynk and
// sound sampling for ~120 ms (plus the pings themselves) every 300 ms. The
// pings are now scheduled one per slot from loop(). Timing is deliberately
// UNCHANGED — S1, S2 60 ms after S1 finishes, S3 60 ms after that, one
// decision per CYCLE_MS round — so the persistence rule means the same number
// of milliseconds it always did and captures stay comparable.
static const uint32_t SOUND_REFRACTORY_US = 50000;  // one sound event per 50 ms at most —
                                                    // a knock chatters the comparator
static const float    US_PER_CM_FIXED  = 58.0f;    // midterm constant (~345 m/s, ~22 C)
static const bool     TEMP_COMPENSATE  = true;     // speed of sound from the DHT11

static const uint16_t BLINK_FAST_MS    = 100;    // LED1-3 door-open, LED5 alarm
static const uint16_t BLINK_SLOW_MS    = 400;    // LED5 armed

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------
Preferences prefs;
BlynkTimer  timer;
DHT         dht(DHT_PIN, DHT11);

bool     armed       = false;
bool     alarmed     = false;          // LATCHED until disarm
uint32_t sirenStart  = 0;
uint16_t currentToneHz = 0;
uint8_t  openStreak  = 0;

float baseline[3] = {0, 0, 0};         // closed-door reading recorded at last arm — diagnostic only
float dist[3]     = {0, 0, 0};
bool  healthy[3]  = {false, false, false};
bool  openFlag[3] = {false, false, false};
bool  doorOpen    = false;             // V3: all three healthy AND open

bool     soundSinceCycle   = false;    // set in uiTick, consumed by sensorCycle
bool     soundSincePublish = false;    // set in uiTick, consumed by publishStatus (V6)
bool     soundActive       = false;    // stretched view, drives LED4
uint32_t soundLedUntil     = 0;
uint32_t soundEventCount   = 0;        // cumulative debounced DO events, from the ISR (log + diag)

float lastTempC     = NAN;
float lastHumidity  = NAN;
bool  dhtOk         = false;           // last DHT read succeeded
float thresholdCm   = THRESH_DEFAULT_CM;  // absolute trip distance, V5

// Serial view: false = deployment (`[cyc]` + `[env]` lines), true = bring-up
// (one hwcheck-style status line, LED1-3 = raw echo health). Toggle with 'p'.
bool  diagMode = false;

// Offline notification ring. The midterm carried a single bool, so two
// offline alarms collapsed into one notification and warnings were never
// queued at all. `code` always points at a string literal.
struct QEvent { uint32_t ms; const char *code; };
QEvent   evq[EVQ_N];
uint8_t  evqHead = 0, evqCount = 0, evqDropped = 0;

uint32_t eventWindowStart = 0;         // rolling hour for the notification budget
uint8_t  eventsThisWindow = 0;

uint8_t  faultStreak = 0;              // consecutive cycles with a sensor down
bool     faultActive = false;          // debounced "a sensor is dead" state
uint32_t lastDhtOkMs = 0;
bool     dhtStale    = false;
bool     sirenMuted  = false;          // silenced by the user; latch stays set
uint8_t  persistN    = PERSIST_N;      // V11 / serial 'N' — runtime-tunable
bool     netEnabled  = true;           // serial 'w' — off = fully standalone, no WiFi/Blynk

// Tier 3 state
uint32_t windowMs      = WINDOW_DEFAULT_MS;  // V17 / serial 'W'
uint32_t trigAt[3]     = {0, 0, 0};    // millis of each sensor's most recent open reading
bool     liveLatch[3]  = {false, false, false};
bool     altOldFired   = false;        // persistence rule reached threshold this arm cycle
bool     altWinFired   = false;        // coincidence window closed this arm cycle
uint8_t  riskScore     = 0;            // 0-100, SHADOW: reported, never triggers the alarm
uint32_t lastSoundMs   = 0;
uint32_t lastPreAlarmMs = 0;
uint32_t lastWarnMs = 0;

// Tier 4 state
uint8_t  pingIdx   = 3;                // next sensor to ping; 3 = idle until the next round
uint32_t pingDueUs = 0;                // when that ping's slot opens
bool     envDue    = false;            // DHT read waiting for the round to finish
volatile uint32_t soundIsrEdges  = 0;  // debounced active DO edges, counted in the ISR
volatile uint32_t soundIsrLastUs = 0;
float    usPerCm   = US_PER_CM_FIXED;  // round-trip us per cm; follows the DHT11 temperature

// --------------------------------------------------------------------------
// Forward declarations
// --------------------------------------------------------------------------
void doArm();
void disarm(const char *src);
const char *stateString();

// --------------------------------------------------------------------------
// CSV data log  (finals Req 4 — record sensor readings and system events)
//
// Two machine-readable line types are interleaved with the human-readable
// output on the same serial stream, so one capture file holds both:
//   #D,...  one telemetry sample per CYCLE_MS
//   #E,...  one discrete system event
// A `#H,` header block is emitted at boot so a capture file is self-describing.
//
// Capture a run with:  pio device monitor -f log2file
// then point tools/analyze.py at the resulting file.
//
// THE SCHEMA IS FROZEN. The midterm-baseline capture and the final capture
// have to parse identically or the before/after comparison means nothing, so
// fields that only become meaningful in a later tier (tmask, risk) are
// present from the start and simply read 0 until then. Append new fields at
// the END and bump LOG_SCHEMA; never reorder or remove one.
// --------------------------------------------------------------------------
static const int LOG_SCHEMA = 1;

bool csvMode = true;             // 'c' toggles; ON by default so a run is never lost

// One discrete event. `detail` must contain no comma and no newline.
void csvEvent(const char *type, const char *detail = "") {
  if (!csvMode) return;
  Serial.printf("#E,%lu,%s,%s\n", (unsigned long)millis(), type, detail);
}

void csvHeader() {
  Serial.printf("#H,V,%d,door-intrusion\n", LOG_SCHEMA);
  Serial.println(F("#H,D,ms,state,armed,alarm,d1,d2,d3,hmask,omask,tmask,thr,streak,risk,snd,sndev,temp,hum,dhtok,rssi,heap"));
  Serial.println(F("#H,E,ms,type,detail"));
}

// One telemetry sample; called at the end of every sensor cycle. hmask/omask
// are per-sensor bitfields (bit i = sensor i+1) for healthy / reads-open.
void logSample(bool sound) {
  if (!csvMode) return;
  unsigned hmask = 0, omask = 0, tmask = 0;
  for (int i = 0; i < 3; i++) {
    if (healthy[i])   hmask |= (1u << i);
    if (openFlag[i])  omask |= (1u << i);
    if (liveLatch[i]) tmask |= (1u << i);
  }
  // tmask bits 0-2 = per-sensor trigger latch live. Bits 3 and 4 record what
  // each rule WOULD have done this arm cycle, which is what makes the
  // old-vs-new comparison derivable from a single run.
  if (altOldFired) tmask |= 0x08;
  if (altWinFired) tmask |= 0x10;
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  Serial.printf("#D,%lu,%s,%d,%d,%.1f,%.1f,%.1f,%u,%u,%u,%.1f,%u,%u,%d,%lu,%.1f,%.1f,%d,%d,%lu\n",
                (unsigned long)millis(), stateString(), (int)armed, (int)alarmed,
                dist[0], dist[1], dist[2],
                hmask, omask, tmask,
                thresholdCm, (unsigned)openStreak, (unsigned)riskScore,
                sound ? 1 : 0, (unsigned long)soundEventCount,
                lastTempC, lastHumidity, dhtOk ? 1 : 0,
                rssi, (unsigned long)ESP.getFreeHeap());
}

// --------------------------------------------------------------------------
// Risk score  (finals Req 7) — SHADOW MODE
//
// A 0-100 fusion of the evidence available each cycle. It is published,
// logged and banded, but it does NOT gate the alarm: the siren is still
// decided by the two rules above. Running it in shadow first means its
// behaviour can be characterised against real data before it is ever given
// authority, and it cannot break detection in the meantime.
//
// Weights are chosen so the terms sum to exactly 100 at maximum, and are
// printed here so the paper can reproduce the table:
//   beams open now        3 x 10 = 30
//   beams latched in window 3 x 8 = 24
//   persistence progress         = 20
//   depth past the threshold     = 16
//   recent sound                 = 10
// --------------------------------------------------------------------------
uint8_t computeRisk(bool soundRecent) {
  uint16_t r = 0;
  uint8_t openN = 0, liveN = 0;
  for (int i = 0; i < 3; i++) {
    if (openFlag[i])  openN++;
    if (liveLatch[i]) liveN++;
  }
  r += openN * 10;
  r += liveN * 8;

  uint8_t p = persistN ? persistN : 1;
  r += (openStreak >= p) ? 20 : (uint16_t)((20UL * openStreak) / p);

  float minD = DIST_MAX_CM;
  bool  haveD = false;
  for (int i = 0; i < 3; i++)
    if (healthy[i] && dist[i] < minD) { minD = dist[i]; haveD = true; }
  if (haveD && thresholdCm > 0.0f && minD <= thresholdCm) {
    float frac = (thresholdCm - minD) / thresholdCm;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    r += (uint16_t)(frac * 16.0f);
  }

  if (soundRecent) r += 10;
  return (r > 100) ? 100 : (uint8_t)r;
}

const char *riskBand(uint8_t r) {
  return (r >= RISK_CRITICAL) ? "CRITICAL" : (r >= RISK_ELEVATED ? "ELEVATED" : "normal");
}

// --------------------------------------------------------------------------
// Test trials  (finals Req 6)
//
// One button press runs a whole trial cycle: clear any latch left over from
// the previous trial, re-arm, log the ground-truth marker, then chirp "go".
// One tap per trial, so the whole battery is a one-person job.
//
// Three classes, because sweep speed is the independent variable that matters:
//   fast    a positive trial where the panel is swept THROUGH the beams, the
//           way a briskly opened door crosses them. The midterm rule needs
//           ~600-700 ms of sustained all-beam agreement, which a fast sweep
//           may never supply — this class is what Tier 3 has to improve.
//   held    a positive trial where the panel is brought in and HELD. Easy
//           case; the midterm rule should already pass it. Having both
//           classes turns "did it get better" into a controlled comparison
//           rather than one pooled number.
//   neg     the panel never fully blocks all three beams; must NOT alarm.
// The class rides in the event's detail field, so the log schema is unchanged.
// --------------------------------------------------------------------------
void startTrial(bool positive, const char *cls) {
  if (armed || alarmed) disarm("trial");
  doArm();
  if (!armed) {                                  // refused — panel in a beam, or a fault
    Serial.println("[trial] ARM REFUSED — clear the beams, then tap again");
    tone(PIEZO_PIN, 400, 400);                   // low buzz = not ready
    currentToneHz = 0;
    return;
  }
  csvEvent(positive ? "trial_pos" : "trial_neg", cls);
  Serial.printf("[trial] %s (%s) — go\n",
                positive ? "POSITIVE" : "NEGATIVE", cls);
  tone(PIEZO_PIN, 2600, 70); delay(90);          // two high pips = armed and logged
  tone(PIEZO_PIN, 2600, 70); delay(90);
  noTone(PIEZO_PIN);
  currentToneHz = 0;
}

// --------------------------------------------------------------------------
// Ultrasonic
// --------------------------------------------------------------------------
float medianOf(float *a, int n) {
  for (int i = 1; i < n; i++) {                 // insertion sort, n <= 5
    float k = a[i];
    int   j = i - 1;
    while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; }
    a[j + 1] = k;
  }
  return (n & 1) ? a[n / 2] : 0.5f * (a[n / 2 - 1] + a[n / 2]);
}

// One HC-SR04 / HC-SR05 ping, measuring the ECHO edges by hand so "faulty" and
// "nothing in range" can be told apart. Returns:
//   > 0 (< DIST_MAX_CM)  real distance in cm
//   DIST_MAX_CM          sensor answered but ECHO stayed HIGH past ECHO_MAX_US
//                        -> nearest surface is beyond ~4.3 m (still armable)
//   -1                   ECHO never asserted (or was stuck HIGH before trigger)
//                        -> faulty / disconnected -> unhealthy
float pingCm(int i) {
  if (digitalRead(ECHO_PIN[i]) == HIGH) return -1.0f;   // stuck HIGH = wiring fault

  digitalWrite(TRIG_PIN[i], LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG_PIN[i], HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN[i], LOW);

  uint32_t t = micros();
  while (digitalRead(ECHO_PIN[i]) == LOW) {             // wait for the sensor to answer
    if (micros() - t > ECHO_START_US) return -1.0f;     // no answer -> faulty / disconnected
  }

  uint32_t rise = micros();
  while (digitalRead(ECHO_PIN[i]) == HIGH) {            // measure the echo pulse
    if (micros() - rise > ECHO_MAX_US) return DIST_MAX_CM;  // nothing in range -> report the ceiling
  }
  float cm = (micros() - rise) / usPerCm;               // round trip, temperature-compensated
  return (cm < DIST_MAX_CM) ? cm : DIST_MAX_CM;         // keep the ceiling a fixed sentinel
}

// Speed of sound from the DHT11 (called after every environment read). The
// midterm divided by a fixed 58 us/cm, which is only right at ~22 C. Indoors
// here it is usually 28-32 C, where that constant reads about 1-2% short.
// At the 20 cm trip point that is ~0.3 cm, so this is a correctness fix, not
// a detection improvement — but it is the one place the DHT11 now feeds the
// measurement the alarm decides on. No usable reading -> the midterm constant.
void updateSoundSpeed() {
  if (!TEMP_COMPENSATE || isnan(lastTempC)) { usPerCm = US_PER_CM_FIXED; return; }
  float t = lastTempC;
  if (t < 0.0f)  t = 0.0f;                              // DHT11's rated range
  if (t > 50.0f) t = 50.0f;
  float c = 331.3f + 0.606f * t;                        // m/s, dry air
  usPerCm = 20000.0f / c;                               // 2 cm of path per cm, at c * 1e-4 cm/us
}

// Health-gate + closed-door sanity check for all three sensors. Returns false
// if any sensor is unhealthy, OR its closed-door reading already crosses the
// threshold (arming would immediately read that channel as "open").
bool captureBaseline() {
  // Arming owns the sensors while it runs: drop any half-finished round so the
  // next decision sees three fresh readings, and leave the anti-crosstalk gap
  // after whatever the scheduler pinged last (it may be this same sensor, with
  // its ECHO still high).
  pingIdx = 3;
  delay(PING_GAP_MS);
  bool ok = true;
  for (int i = 0; i < 3; i++) {
    float s[BASELINE_SAMPLES];
    int   n = 0;
    for (int k = 0; k < BASELINE_SAMPLES; k++) {
      float d = pingCm(i);
      if (d > 0.0f) s[n++] = d;
      delay(PING_GAP_MS);
    }
    if (n < (BASELINE_SAMPLES / 2 + 1)) {       // need a majority of valid reads
      ok = false;
      baseline[i] = 0.0f;
      Serial.printf("[arm] sensor %d UNHEALTHY (%d/%d echoes)\n", i + 1, n, BASELINE_SAMPLES);
      continue;
    }
    baseline[i] = medianOf(s, n);
    bool alreadyOpen = DOOR_OPEN_IS_FARTHER ? (baseline[i] >= thresholdCm)
                                             : (baseline[i] <= thresholdCm);
    if (alreadyOpen) {
      ok = false;
      Serial.printf("[arm] sensor %d reads %.1f cm, already past the %.1f cm threshold — "
                     "raise/lower the V5 slider to match this mount, or re-aim the sensor\n",
                     i + 1, baseline[i], thresholdCm);
    } else {
      Serial.printf("[arm] sensor %d closed-door reading = %.1f cm  (threshold %.1f cm, %d/%d echoes)\n",
                     i + 1, baseline[i], thresholdCm, n, BASELINE_SAMPLES);
    }
  }
  return ok;
}

// --------------------------------------------------------------------------
// Piezo
// --------------------------------------------------------------------------
void sirenOff() {
  noTone(PIEZO_PIN);
  currentToneHz = 0;
}

// Called every UI tick. Two-tone sweep while the alarm is latched and inside
// the 3-minute window; silent otherwise. The alarm stays latched after the
// siren times out.
void serviceSiren(uint32_t now) {
  uint32_t el   = now - sirenStart;
  bool     want = alarmed && !sirenMuted && (el < SIREN_MAX_MS);
  if (want) {                                   // duty-cycle: SIREN_ON_MS on, then a gap
    uint32_t phase = el % (SIREN_ON_MS + SIREN_OFF_MS);
    if (phase >= SIREN_ON_MS) want = false;
  }
  if (!want) {
    if (currentToneHz != 0) sirenOff();
    return;
  }
  uint16_t f = (((now - sirenStart) / SIREN_SWEEP_MS) & 1) ? SIREN_HI_HZ : SIREN_LO_HZ;
  if (f != currentToneHz) {
    tone(PIEZO_PIN, f);
    currentToneHz = f;
  }
}

void armChirp() {                                // short rising two-tone
  tone(PIEZO_PIN, 1500, 80);  delay(100);
  tone(PIEZO_PIN, 2200, 90);  delay(110);
  sirenOff();
}

void disarmChirp() {                             // short falling two-tone
  tone(PIEZO_PIN, 2200, 80);  delay(100);
  tone(PIEZO_PIN, 1500, 90);  delay(110);
  sirenOff();
}

// --------------------------------------------------------------------------
// Notifications
//
// Every outbound Blynk event goes through notify(): budget-checked, and
// queued rather than dropped when offline. Blynk rate-limits events
// server-side and discards the overflow without telling anyone, so an
// unbounded burst does not mean "lots of alerts", it means "no alerts".
// --------------------------------------------------------------------------
const char *resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    default:                return "unknown";
  }
}

bool eventBudgetOk() {
  uint32_t now = millis();
  if (now - eventWindowStart > 3600000UL) {     // roll the hour
    eventWindowStart = now;
    eventsThisWindow = 0;
  }
  if (eventsThisWindow >= EVENT_MAX_PER_HR) return false;
  eventsThisWindow++;
  return true;
}

void queueEvent(const char *code) {
  if (evqCount == EVQ_N) {                      // full: drop the oldest, count it
    evqHead = (evqHead + 1) % EVQ_N;
    evqCount--;
    if (evqDropped < 255) evqDropped++;
  }
  evq[(evqHead + evqCount) % EVQ_N] = { millis(), code };
  evqCount++;
}

// Send one notification, or queue it if that is not possible right now.
// Every notification is announced on serial as well as sent, so the whole
// alert path can be verified on the terminal with no phone and no Blynk.
void notify(const char *code) {
  if (!netEnabled || !Blynk.connected()) {
    queueEvent(code);
    Serial.printf("[notify] %s  (offline — queued, %u waiting)\n", code, evqCount);
    return;
  }
  if (!eventBudgetOk()) {
    Serial.printf("[notify] %s  DROPPED — hourly budget spent\n", code);
    csvEvent("event_dropped", code);
    return;
  }
  Serial.printf("[notify] %s  -> Blynk\n", code);
  Blynk.logEvent(code);
}

// Drain at most one queued event per call, so a backlog never blocks the loop.
void flushEventQueue() {
  if (!evqCount || !netEnabled || !Blynk.connected()) return;
  if (!eventBudgetOk()) return;
  const char *code = evq[evqHead].code;
  uint32_t    age  = millis() - evq[evqHead].ms;
  evqHead = (evqHead + 1) % EVQ_N;
  evqCount--;
  Blynk.logEvent(code);
  char d[48];
  snprintf(d, sizeof(d), "%s age=%lums", code, (unsigned long)age);
  csvEvent("queued_sent", d);
}

void raiseIntrusionEvent() { notify("intrusion_alarm"); }

void maybeWarn() {
  uint32_t now = millis();
  if (now - lastWarnMs < WARN_COOLDOWN_MS) return;
  lastWarnMs = now;
  Serial.println("[warn] door_noise_warning");
  csvEvent("warn", "sound-no-door");
  notify("door_noise_warning");
}

// --------------------------------------------------------------------------
// Persistence
//
// The midterm persisted `armed` but hardcoded `alarmed = false` at boot, so
// cutting the power silenced a latched siren — a power-cycle bypass, and the
// opposite of what §8.5 argues for. Both flags are persisted now.
//
// The siren's 3-minute budget deliberately restarts on boot rather than being
// persisted: it avoids writing to NVS on a timer, and someone power-cycling
// the device to shut it up getting a fresh siren each time is the correct
// outcome. Serial 'X' clears the stored state if a latch ever gets stuck.
// --------------------------------------------------------------------------
void persistArmed(bool v) {
  prefs.putBool("armed", v);
}

void persistAlarmed(bool v) {
  prefs.putBool("alarmed", v);
}

// --------------------------------------------------------------------------
// Ping scheduler  (Tier 4)
//
// startRound() fires every CYCLE_MS and pings S1 on the tick itself;
// servicePings() runs on every loop() pass and fires the next ping once its
// slot is due, so the gaps between pings are free for the UI tick, Blynk and
// the serial console instead of being spent in delay(). The order and spacing
// are exactly the midterm's: S1, then S2 PING_GAP_MS after S1's ping returns,
// then S3 the same, and the decision runs once, on three fresh readings,
// after S3.
//
// Nothing slower than a ping may run between two pings of a round. Uneven
// sample spacing costs detections: a fast sweep's dip can fall into the one
// stretched gap and be missed (host simulation: ~2% of fast sweeps, when the
// 25 ms DHT11 read was allowed to land mid-round). So the DHT read waits for
// the round to finish — which is where the midterm ran it anyway.
// --------------------------------------------------------------------------
void sensorCycle();
void readEnvironment();

void readSensor(int i) {
  float d = pingCm(i);
  healthy[i] = (d > 0.0f);
  if (healthy[i]) {
    dist[i] = d;
    // absolute threshold, not baseline deviation — see header comment
    openFlag[i] = DOOR_OPEN_IS_FARTHER ? (d >= thresholdCm)   // leaf swings out of beam
                                        : (d <= thresholdCm); // leaf swings into beam (this build)
  } else {
    openFlag[i] = false;                        // an unhealthy sensor cannot vote "open"
  }
}

void servicePings() {                           // every loop() pass
  if (pingIdx >= 3 || (int32_t)(micros() - pingDueUs) < 0) return;
  readSensor(pingIdx++);
  if (pingIdx < 3) {
    pingDueUs = micros() + PING_GAP_MS * 1000UL;  // sequential firing, anti-crosstalk
    return;
  }
  sensorCycle();
  if (envDue) { envDue = false; readEnvironment(); }
}

void startRound() {                             // every CYCLE_MS
  if (pingIdx < 3) return;                      // last round still in flight — never overlap two
  pingIdx   = 0;
  pingDueUs = micros();
  servicePings();                               // S1 on the tick, as the midterm cycle did
}

void environmentTick() {                        // every DHT_MS
  if (pingIdx < 3) envDue = true;               // mid-round: run it after the decision instead
  else             readEnvironment();
}

// --------------------------------------------------------------------------
// Decision cycle  (once per CYCLE_MS round, after the third ping)
// --------------------------------------------------------------------------
void sensorCycle() {
  bool sound = soundSinceCycle;
  soundSinceCycle = false;

  bool allHealthy = healthy[0] && healthy[1] && healthy[2];

  // Debounced sensor-fault detection. The midterm forced an unhealthy sensor
  // to "not open" and required all three to agree, so one unplugged ECHO made
  // the alarm permanently unable to fire while the dashboard still read ARMED.
  // That is fail-silent. It still cannot fire — but now it says so.
  if (allHealthy) faultStreak = 0;
  else if (faultStreak < 255) faultStreak++;

  bool nowFault = (faultStreak >= FAULT_CONFIRM_N);
  if (nowFault != faultActive) {
    faultActive = nowFault;
    char d[32];
    snprintf(d, sizeof(d), "h=%d%d%d", healthy[0], healthy[1], healthy[2]);
    if (faultActive) {
      Serial.printf("[FAULT] sensor down (%s) — detection is degraded\n", d);
      csvEvent("fault", d);
      notify("sensor_fault");
    } else {
      Serial.println("[FAULT] cleared — all three sensors healthy");
      csvEvent("fault_cleared", d);
    }
  }

  bool allOpen    = allHealthy && openFlag[0] && openFlag[1] && openFlag[2];
  bool noneOpen   = !openFlag[0] && !openFlag[1] && !openFlag[2];
  doorOpen = allOpen;

  // --- coincidence window -------------------------------------------------
  uint32_t nowMs = millis();
  for (int i = 0; i < 3; i++) {
    if (openFlag[i]) trigAt[i] = nowMs;
    liveLatch[i] = trigAt[i] && (nowMs - trigAt[i] <= windowMs);
  }
  bool coincidence = liveLatch[0] && liveLatch[1] && liveLatch[2];

  // --- risk score (shadow — reported, never triggers) ----------------------
  bool soundRecent = lastSoundMs && (nowMs - lastSoundMs <= SOUND_RECENT_MS);
  riskScore = computeRisk(soundRecent);

  if (armed) {
    openStreak = allOpen ? (openStreak + 1) : 0;

    // Both rules are evaluated EVERY cycle, including after the latch, so the
    // log records what each would have done on its own. Without this the
    // first rule to fire would mask the other and the comparison would be
    // biased toward whichever is faster.
    if (openStreak >= persistN) altOldFired = true;
    if (coincidence)            altWinFired = true;

    if (!alarmed) {
      if (altOldFired || altWinFired) {
        alarmed    = true;                      // LATCHED
        sirenStart = nowMs;
        sirenMuted = false;                     // a new alarm overrides a previous silence
        persistAlarmed(true);                   // survives a power cut
        const char *by = altOldFired ? (altWinFired ? "both" : "persistence")
                                     : "window";
        Serial.printf("[ALARM] intrusion — fired by %s\n", by);
        {
          char d[72];
          snprintf(d, sizeof(d), "by=%s d=%.0f/%.0f/%.0f thr=%.0f risk=%u",
                   by, dist[0], dist[1], dist[2], thresholdCm, (unsigned)riskScore);
          csvEvent("alarm", d);
        }
        raiseIntrusionEvent();
      } else {
        if (sound && noneOpen) maybeWarn();     // warning tier — no siren

        // Pre-alarm: the shadow score crossing into ELEVATED. Notification
        // only; it never latches and never sounds the siren.
        if (riskScore >= RISK_ELEVATED &&
            (lastPreAlarmMs == 0 || nowMs - lastPreAlarmMs > PREALARM_COOLDOWN_MS)) {
          lastPreAlarmMs = nowMs;
          char d[32];
          snprintf(d, sizeof(d), "risk=%u", (unsigned)riskScore);
          Serial.printf("[pre-alarm] risk %u (%s)\n", (unsigned)riskScore, riskBand(riskScore));
          csvEvent("pre_alarm", d);
          notify("pre_alarm");
        }
      }
    }
  }

  if (diagMode) {
    char db[3][12];
    for (int i = 0; i < 3; i++) {
      if (!healthy[i])              snprintf(db[i], sizeof(db[i]), "FAULT");
      else if (dist[i] >= DIST_MAX_CM) snprintf(db[i], sizeof(db[i]), ">%.0fcm", DIST_MAX_CM);
      else                         snprintf(db[i], sizeof(db[i]), "%.1fcm", dist[i]);
    }
    char eb[20];
    if (dhtOk) snprintf(eb, sizeof(eb), "%.1fC %.0f%%", lastTempC, lastHumidity);
    else       snprintf(eb, sizeof(eb), "DHT fail");
    Serial.printf("US1 %-9s US2 %-9s US3 %-9s | SND %-5s ev=%lu | %s | %s\n",
                  db[0], db[1], db[2], sound ? "SOUND" : "quiet",
                  (unsigned long)soundEventCount, eb, stateString());
  } else {
    // b= is the closed-door reading logged at the last arm (reference only —
    // the open/closed decision compares d= directly against thr=, no baseline math)
    Serial.printf("[cyc] d=%.0f/%.0f/%.0f  b=%.0f/%.0f/%.0f  thr=%.0f  h=%d%d%d  o=%d%d%d  snd=%d  strk=%u  %s\n",
                  dist[0], dist[1], dist[2],
                  baseline[0], baseline[1], baseline[2],
                  thresholdCm,
                  healthy[0], healthy[1], healthy[2],
                  openFlag[0], openFlag[1], openFlag[2],
                  sound ? 1 : 0, openStreak, stateString());
  }

  logSample(sound);
}

// --------------------------------------------------------------------------
// UI tick — LEDs, siren, sound sampling  (every UI_MS)
// --------------------------------------------------------------------------
// LM393 DO edge (Tier 4). The midterm only polled the pin every UI_MS, and not
// at all while the cycle sat in delay(), so a knock shorter than the poll
// period was simply missed. The interrupt sees every edge; the refractory
// period folds a chattering comparator burst back into one event, which is
// the same one-per-50-ms ceiling the poll had.
void IRAM_ATTR soundIsr() {
  uint32_t now = (uint32_t)esp_timer_get_time();
  if (now - soundIsrLastUs >= SOUND_REFRACTORY_US) {
    soundIsrLastUs = now;
    soundIsrEdges++;
  }
}

void uiTick() {
  uint32_t now = millis();

  // Sound = DO still active now, OR the ISR caught a pulse since the last tick.
  static uint32_t seenEdges = 0;
  uint32_t edges = soundIsrEdges;                 // aligned 32-bit read is atomic
  int  raw = digitalRead(SOUND_DO_PIN);
  bool s   = (SOUND_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH)) || (edges != seenEdges);
  soundEventCount += edges - seenEdges;           // debounced events, for the log and diag line
  seenEdges = edges;
  if (s) {
    soundSinceCycle   = true;
    soundSincePublish = true;
    soundLedUntil      = now + SOUND_LED_HOLD_MS;
    lastSoundMs        = now;                   // feeds the risk score's sound term
  }
  soundActive = (now < soundLedUntil);

  bool bf = (now / BLINK_FAST_MS) & 1;
  bool bs = (now / BLINK_SLOW_MS) & 1;

  if (diagMode) {
    // bring-up view: LED1-3 = raw echo health, LED5 = heartbeat
    for (int i = 0; i < 3; i++) digitalWrite(LED_PIN[i], healthy[i] ? HIGH : LOW);
    digitalWrite(LED_SOUND, soundActive ? HIGH : LOW);
    digitalWrite(LED_MODE, bs ? HIGH : LOW);
  } else {
    // LED1-3: off = fault, solid = valid echo, blink = that sensor reads door-open
    for (int i = 0; i < 3; i++) {
      uint8_t level;
      if (!healthy[i])      level = LOW;
      else if (openFlag[i]) level = bf ? HIGH : LOW;
      else                  level = HIGH;
      digitalWrite(LED_PIN[i], level);
    }
    digitalWrite(LED_SOUND, soundActive ? HIGH : LOW);
    // LED5: solid disarmed / slow blink armed / fast blink alarm
    uint8_t mode;
    if (alarmed)    mode = bf ? HIGH : LOW;
    else if (armed) mode = bs ? HIGH : LOW;
    else            mode = HIGH;
    digitalWrite(LED_MODE, mode);
  }

  serviceSiren(now);
}

// --------------------------------------------------------------------------
// DHT11  (every DHT_MS)
// --------------------------------------------------------------------------
void readEnvironment() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  dhtOk = !isnan(t) && !isnan(h);
  if (dhtOk) {
    lastTempC    = t;
    lastHumidity = h;
    lastDhtOkMs  = millis();
    if (dhtStale) { dhtStale = false; csvEvent("dht_ok"); }
  } else if (!dhtStale && millis() - lastDhtOkMs > DHT_STALE_MS) {
    // The midterm kept publishing the last good value forever after the sensor
    // died. Past the staleness window the readings are invalidated instead, so
    // nothing downstream trusts a number that is no longer being measured.
    dhtStale     = true;
    lastTempC    = NAN;
    lastHumidity = NAN;
    csvEvent("dht_stale");
    Serial.println("[env] DHT stale — readings invalidated");
  }
  updateSoundSpeed();                             // stale -> back to the fixed constant
  if (!diagMode)                                  // diag view carries t/h on its status line
    Serial.printf("[env] t=%.1f C  h=%.0f %%%s\n", lastTempC, lastHumidity,
                  dhtOk ? "" : "   (DHT read failed)");
}

// --------------------------------------------------------------------------
// Telemetry push  (every PUBLISH_MS)
// --------------------------------------------------------------------------
void publishStatus() {
  if (!netEnabled || !Blynk.connected()) return;
  if (!isnan(lastTempC))    Blynk.virtualWrite(VPIN_TEMP, lastTempC);
  if (!isnan(lastHumidity)) Blynk.virtualWrite(VPIN_HUM,  lastHumidity);
  Blynk.virtualWrite(VPIN_DOOR,  doorOpen ? 1 : 0);
  Blynk.virtualWrite(VPIN_STATE, stateString());
  Blynk.virtualWrite(VPIN_SOUND, soundSincePublish ? 1 : 0);
  soundSincePublish = false;
  Blynk.virtualWrite(VPIN_ALARM, alarmed ? 1 : 0);
  Blynk.virtualWrite(VPIN_HEAP,   (uint32_t)ESP.getFreeHeap());
  Blynk.virtualWrite(VPIN_HEALTH, faultActive ? 0 : 1);
  Blynk.virtualWrite(VPIN_RISK,   (int)riskScore);
  flushEventQueue();                            // at most one per tick
}

// --------------------------------------------------------------------------
// WiFi reconnect watchdog  (every NETCHECK_MS)
// --------------------------------------------------------------------------
void netWatchdog() {
  if (!netEnabled) return;                        // standalone mode — serial 'w'
  static bool wasDown = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (wasDown) {                                // log the recovery edge too
      wasDown = false;
      Serial.println("[net] WiFi back up");
      csvEvent("wifi_up");
    }
    return;
  }
  if (!wasDown) {
    wasDown = true;
    csvEvent("wifi_down");
  }
  static uint32_t lastKick = 0;
  uint32_t now = millis();
  if (now - lastKick < 15000) return;
  lastKick = now;
  static uint8_t tries = 0;                       // don't shout every 15 s forever
  if (tries % 4 == 0)
    Serial.printf("[net] WiFi down — reconnecting (attempt %u; 'w' for standalone)\n",
                  (unsigned)(tries + 1));
  tries++;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// --------------------------------------------------------------------------
// Arm / disarm
// --------------------------------------------------------------------------
const char *stateString() {
  return alarmed ? "ALARM" : (armed ? "ARMED" : "DISARMED");
}

void doArm() {
  if (armed || alarmed) return;                 // idempotent; must disarm before re-arming
  Serial.println("[arm] capturing baseline — door must be shut");
  if (!captureBaseline()) {
    Serial.println("[arm] ABORTED — one or more sensors unhealthy");
    csvEvent("arm_abort", "health-or-sanity-check-failed");
    if (Blynk.connected()) {
      Blynk.virtualWrite(VPIN_ARM, 0);          // bounce the switch back
      Blynk.virtualWrite(VPIN_STATE, "DISARMED");
    }
    return;
  }
  armed      = true;
  alarmed    = false;
  openStreak = 0;
  for (int i = 0; i < 3; i++) { trigAt[i] = 0; liveLatch[i] = false; }
  altOldFired = altWinFired = false;            // fresh verdict for this arm cycle
  lastPreAlarmMs = 0;
  persistArmed(true);
  armChirp();
  Serial.println("[arm] ARMED");
  {
    char d[48];
    snprintf(d, sizeof(d), "b=%.0f/%.0f/%.0f thr=%.0f sos=%.1f",
             baseline[0], baseline[1], baseline[2], thresholdCm, 20000.0f / usPerCm);
    csvEvent("arm", d);
  }
  if (Blynk.connected()) {
    Blynk.virtualWrite(VPIN_ARM, 1);             // keep the app switch in sync
    Blynk.virtualWrite(VPIN_STATE, "ARMED");
  }
}

void disarm(const char *src) {
  bool wasActive = armed || alarmed;
  armed      = false;
  alarmed    = false;
  openStreak = 0;
  sirenMuted = false;
  sirenOff();
  persistArmed(false);
  persistAlarmed(false);
  if (wasActive) {
    // Verdict for the arm cycle just ending: what each rule would have done on
    // its own. This is the line tools/analyze.py turns into the old-vs-new
    // detection comparison.
    char v[48];
    snprintf(v, sizeof(v), "old=%d win=%d risk=%u",
             (int)altOldFired, (int)altWinFired, (unsigned)riskScore);
    csvEvent("verdict", v);
    Serial.printf("[disarm] by %s  (%s)\n", src, v);
    disarmChirp();
  }
  altOldFired = altWinFired = false;
  for (int i = 0; i < 3; i++) { trigAt[i] = 0; liveLatch[i] = false; }
  if (Blynk.connected()) {
    Blynk.virtualWrite(VPIN_ARM, 0);
    Blynk.virtualWrite(VPIN_STATE, "DISARMED");
  }
}

// --------------------------------------------------------------------------
// Blynk handlers
// --------------------------------------------------------------------------
BLYNK_WRITE(VPIN_ARM) {
  if (param.asInt() == 1) doArm();
  else                    disarm("app");
}

BLYNK_WRITE(VPIN_TRIAL_F) {
  if (param.asInt() == 1) startTrial(true, "fast");
}

BLYNK_WRITE(VPIN_TRIAL_H) {
  if (param.asInt() == 1) startTrial(true, "held");
}

BLYNK_WRITE(VPIN_TRIAL_N) {
  if (param.asInt() == 1) startTrial(false, "neg");
}

BLYNK_WRITE(VPIN_WINDOW) {
  uint32_t v = (uint32_t)param.asInt();
  if (v < WINDOW_MIN_MS) v = WINDOW_MIN_MS;
  if (v > WINDOW_MAX_MS) v = WINDOW_MAX_MS;
  windowMs = v;
  Serial.printf("[cfg] coincidence window = %lu ms\n", (unsigned long)windowMs);
  char d[32];
  snprintf(d, sizeof(d), "window=%lu", (unsigned long)windowMs);
  csvEvent("cfg", d);
}

BLYNK_WRITE(VPIN_PERSIST) {
  int v = param.asInt();
  if (v < 2) v = 2;
  if (v > 8) v = 8;
  persistN = (uint8_t)v;
  Serial.printf("[cfg] persistence = %u cycles\n", persistN);
  char d[24];
  snprintf(d, sizeof(d), "persistN=%u", persistN);
  csvEvent("cfg", d);
}

BLYNK_WRITE(VPIN_TEST) {
  if (param.asInt() != 1) return;
  if (alarmed) {                                // never talk over a real alarm
    Serial.println("[test] ignored — alarm is active");
    return;
  }
  Serial.println("[test] siren test");
  csvEvent("siren_test");
  for (int f = 800; f <= 1600; f += 200) { tone(PIEZO_PIN, f, 120); delay(140); }
  noTone(PIEZO_PIN);
  currentToneHz = 0;
}

BLYNK_WRITE(VPIN_SILENCE) {
  if (param.asInt() != 1) return;
  if (!alarmed) return;
  sirenMuted = true;                            // LATCH STAYS SET — this is not a disarm
  sirenOff();
  Serial.println("[silence] siren muted; alarm still latched");
  csvEvent("siren_muted");
}

BLYNK_WRITE(VPIN_THRESH) {
  float v = param.asFloat();
  if (v < THRESH_MIN_CM) v = THRESH_MIN_CM;
  if (v > THRESH_MAX_CM) v = THRESH_MAX_CM;
  thresholdCm = v;
  Serial.printf("[cfg] trip threshold = %.1f cm\n", thresholdCm);
  {
    char d[32];
    snprintf(d, sizeof(d), "thr=%.1f", thresholdCm);
    csvEvent("cfg", d);
  }
}

BLYNK_CONNECTED() {
  Serial.println("[blynk] connected");
  csvEvent("blynk_up");
  Blynk.syncVirtual(VPIN_THRESH);               // pull the slider values from the server
  Blynk.syncVirtual(VPIN_PERSIST);
  Blynk.syncVirtual(VPIN_WINDOW);
  Blynk.virtualWrite(VPIN_ARM,   armed ? 1 : 0);  // device (NVS) is the source of truth for armed
  Blynk.virtualWrite(VPIN_STATE, stateString());
  Blynk.virtualWrite(VPIN_ALARM, alarmed ? 1 : 0);
  if (evqCount) {
    char d[32];
    snprintf(d, sizeof(d), "queued=%u dropped=%u", evqCount, evqDropped);
    Serial.printf("[blynk] flushing offline events (%s)\n", d);
    csvEvent("queue_flush", d);                 // publishStatus drains it, one per second
  }
}

// --------------------------------------------------------------------------
// Serial console
//
// Diagnostics (p/t/b/m/h/?) are always available — they replace the old
// separate hw_check sketch. Arm/disarm over serial (a/d) is a bench aid; the
// spec (PRD §16) arms via the Blynk switch only, so set ALLOW_SERIAL_CONTROL
// = false for a spec-clean build.
// --------------------------------------------------------------------------
static const bool ALLOW_SERIAL_CONTROL = true;

void diagHelp() {
  Serial.println(F("[cmd] ---- control -------------------------------------------"));
  Serial.println(F("[cmd]  a arm      d disarm     s silence siren (latch stays)"));
  Serial.println(F("[cmd]  ? status   h help       X clear persisted armed/alarm"));
  Serial.println(F("[cmd] ---- trials (Req 6) -----------------------------------"));
  Serial.println(F("[cmd]  1 FAST sweep   2 HELD panel   0 disturbance"));
  Serial.println(F("[cmd] ---- settings (type the value, then Enter) ------------"));
  Serial.println(F("[cmd]  T<cm>  trip threshold, e.g. T20      (5-200)"));
  Serial.println(F("[cmd]  N<n>   persistence cycles, e.g. N3   (2-8)"));
  Serial.println(F("[cmd]  W<ms>  coincidence window, e.g. W1200 (300-4000)"));
  Serial.println(F("[cmd] ---- diagnostics --------------------------------------"));
  Serial.println(F("[cmd]  v ping-burst  t self-test  b buzzer  m pinmap"));
  Serial.println(F("[cmd]  p bring-up view   c CSV log   w WiFi/Blynk on-off"));
  Serial.println(F("[cmd] Everything here works with no Blynk and no network."));
}

// One-screen readout of everything the Blynk dashboard would show. This is
// what makes the device fully testable with no app and no network.
void diagStatus() {
  uint32_t up = millis() / 1000;
  Serial.println(F("[status] -------------------------------------------------"));
  Serial.printf("  state      %-12s armed=%d alarm=%d muted=%d\n",
                stateString(), (int)armed, (int)alarmed, (int)sirenMuted);
  Serial.printf("  sensors    %-12s h=%d%d%d  d=%.0f/%.0f/%.0f cm\n",
                faultActive ? "FAULT" : "OK",
                healthy[0], healthy[1], healthy[2], dist[0], dist[1], dist[2]);
  Serial.printf("  trip       %-12.1f persistence=%u cycles  streak=%u\n",
                thresholdCm, (unsigned)persistN, (unsigned)openStreak);
  Serial.printf("  window     %-12lu latches=%d%d%d   would-fire: persistence=%d window=%d\n",
                (unsigned long)windowMs, liveLatch[0], liveLatch[1], liveLatch[2],
                (int)altOldFired, (int)altWinFired);
  Serial.printf("  risk       %-12u %s  (shadow — never triggers)\n",
                (unsigned)riskScore, riskBand(riskScore));
  Serial.printf("  door       %-12s sound=%-7s events=%lu\n",
                doorOpen ? "OPEN" : "closed", soundActive ? "ACTIVE" : "quiet",
                (unsigned long)soundEventCount);
  if (dhtStale) Serial.println(F("  env        STALE        DHT not reporting"));
  else          Serial.printf("  env        %.1f C / %.0f %%\n", lastTempC, lastHumidity);
  {
    char sb[16];
    snprintf(sb, sizeof(sb), "%.1f m/s", 20000.0f / usPerCm);
    Serial.printf("  ranging    %-12s %s\n", sb,
                  (TEMP_COMPENSATE && !isnan(lastTempC)) ? "speed of sound from the DHT11"
                                                         : "fixed 58 us/cm (no DHT reading)");
  }
  Serial.printf("  net        wifi=%-9s blynk=%-7s queued=%u dropped=%u\n",
                netEnabled ? (WiFi.status() == WL_CONNECTED ? "up" : "down") : "OFF",
                (netEnabled && Blynk.connected()) ? "up" : "down",
                (unsigned)evqCount, (unsigned)evqDropped);
  Serial.printf("  heap       %-12lu uptime=%02lu:%02lu:%02lu\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
                (unsigned long)(up % 60));
  Serial.printf("  csv        %-12s schema=%d\n", csvMode ? "ON" : "OFF", LOG_SCHEMA);
  Serial.println(F("[status] -------------------------------------------------"));
}

void diagPinMap() {
  Serial.printf("[map] ECHO %d/%d/%d  TRIG %d/%d/%d  SOUND %d  DHT %d  LED %d/%d/%d/%d/%d  PIEZO %d\n",
                ECHO_PIN[0], ECHO_PIN[1], ECHO_PIN[2], TRIG_PIN[0], TRIG_PIN[1], TRIG_PIN[2],
                SOUND_DO_PIN, DHT_PIN, LED_PIN[0], LED_PIN[1], LED_PIN[2], LED_PIN[3], LED_PIN[4],
                PIEZO_PIN);
}

void diagBuzzer() {
  Serial.println(F("[out] piezo 1500 / 2200 Hz + sweep"));
  tone(PIEZO_PIN, 1500, 150); delay(200);
  tone(PIEZO_PIN, 2200, 150); delay(200);
  for (int f = 800; f <= 2000; f += 120) { tone(PIEZO_PIN, f, 40); delay(45); }
  noTone(PIEZO_PIN);
  currentToneHz = 0;
}

void diagSelfTest() {
  Serial.println(F("[out] LED sweep 1..5, then all, then piezo"));
  for (int i = 0; i < 5; i++) { digitalWrite(LED_PIN[i], HIGH); delay(300); digitalWrite(LED_PIN[i], LOW); }
  for (int i = 0; i < 5; i++) digitalWrite(LED_PIN[i], HIGH);
  delay(800);
  for (int i = 0; i < 5; i++) digitalWrite(LED_PIN[i], LOW);
  diagBuzzer();
}

// Verbose per-sensor ping — distinguishes faulty from out-of-range.
void diagPingBurst() {
  Serial.println(F("[in ] ping burst:"));
  delay(PING_GAP_MS);                            // clear of the scheduler's last ping
  for (int i = 0; i < 3; i++) {
    float d = pingCm(i);
    if (d < 0.0f)
      Serial.printf("   US%d FAULT — ECHO never asserted (dead / unplugged / stuck HIGH)\n", i + 1);
    else if (d >= DIST_MAX_CM)
      Serial.printf("   US%d alive, nearest surface > %.0f cm (reported as ceiling)\n", i + 1, DIST_MAX_CM);
    else
      Serial.printf("   US%d %.1f cm\n", i + 1, d);
    delay(PING_GAP_MS);
  }
}

// Instant single-key commands. Everything the Blynk dashboard can do is
// reachable here, so the device is fully testable standalone.
void runKey(char c) {
  switch (c) {
    case 'a':
      if (ALLOW_SERIAL_CONTROL) { Serial.println("[serial] arm request"); doArm(); }
      else Serial.println("[serial] a/d disabled (ALLOW_SERIAL_CONTROL=false)");
      break;
    case 'd':
      if (ALLOW_SERIAL_CONTROL) { Serial.println("[serial] disarm request"); disarm("serial"); }
      else Serial.println("[serial] a/d disabled (ALLOW_SERIAL_CONTROL=false)");
      break;
    case 's':                                    // serial twin of the V13 button
      if (!alarmed) { Serial.println("[serial] no alarm to silence"); break; }
      sirenMuted = true;                         // LATCH STAYS SET — not a disarm
      sirenOff();
      Serial.println("[silence] siren muted; alarm still latched");
      csvEvent("siren_muted");
      break;
    case 'w':
      netEnabled = !netEnabled;
      if (netEnabled) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        Serial.println("[net] WiFi/Blynk ENABLED");
      } else {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.println("[net] standalone mode — WiFi/Blynk OFF");
      }
      csvEvent("net_mode", netEnabled ? "on" : "standalone");
      break;
    case '?': diagStatus();   break;
    case 'p':
      diagMode = !diagMode;
      Serial.printf("[serial] bring-up view %s\n", diagMode ? "ON" : "OFF");
      break;
    case 'v': diagPingBurst(); break;
    case 't': diagSelfTest();  break;
    case 'b': diagBuzzer();    break;
    case 'm': diagPinMap();    break;
    case 'X':                                    // escape hatch for a stuck latch
      prefs.putBool("armed", false);
      prefs.putBool("alarmed", false);
      armed = alarmed = false;
      sirenMuted = false;
      sirenOff();
      Serial.println("[serial] persisted state CLEARED — disarmed");
      csvEvent("nvs_cleared");
      break;
    case 'h': diagHelp();      break;

    // Req 6 test battery. Identical path to the V8/V9/V10 buttons, so serial
    // and phone runs produce the same data.
    case '1': startTrial(true,  "fast"); break;
    case '2': startTrial(true,  "held"); break;
    case '0': startTrial(false, "neg");  break;

    case 'c':
      csvMode = !csvMode;
      Serial.printf("[serial] CSV log %s\n", csvMode ? "ON" : "OFF");
      if (csvMode) csvHeader();
      break;
    default: break;
  }
}

// Value commands: T<cm> trip threshold, N<n> persistence. Typed as digits
// after the letter and committed with Enter — the serial twins of the V5 and
// V11 sliders.
void runValueCmd(const char *buf) {
  char key = buf[0];
  if (buf[1] == '\0') {                          // bare letter: report, don't change
    if      (key == 'T') Serial.printf("[cfg] trip threshold = %.1f cm (type e.g. T20)\n", thresholdCm);
    else if (key == 'N') Serial.printf("[cfg] persistence = %u cycles (type e.g. N3)\n", (unsigned)persistN);
    else                 Serial.printf("[cfg] coincidence window = %lu ms (type e.g. W1200)\n",
                                       (unsigned long)windowMs);
    return;
  }
  float v = atof(buf + 1);
  if (key == 'W') {
    uint32_t w = (uint32_t)v;
    if (w < WINDOW_MIN_MS) w = WINDOW_MIN_MS;
    if (w > WINDOW_MAX_MS) w = WINDOW_MAX_MS;
    windowMs = w;
    Serial.printf("[cfg] coincidence window = %lu ms\n", (unsigned long)windowMs);
    char d[32];
    snprintf(d, sizeof(d), "window=%lu", (unsigned long)windowMs);
    csvEvent("cfg", d);
  } else if (key == 'T') {
    if (v < THRESH_MIN_CM) v = THRESH_MIN_CM;
    if (v > THRESH_MAX_CM) v = THRESH_MAX_CM;
    thresholdCm = v;
    Serial.printf("[cfg] trip threshold = %.1f cm\n", thresholdCm);
    char d[32];
    snprintf(d, sizeof(d), "thr=%.1f", thresholdCm);
    csvEvent("cfg", d);
  } else {
    int iv = (int)v;
    if (iv < 2) iv = 2;
    if (iv > 8) iv = 8;
    persistN = (uint8_t)iv;
    Serial.printf("[cfg] persistence = %u cycles\n", (unsigned)persistN);
    char d[32];
    snprintf(d, sizeof(d), "persistN=%u", (unsigned)persistN);
    csvEvent("cfg", d);
  }
}

void handleSerialControl() {
  static char     buf[16];
  static uint8_t  len = 0;
  static uint32_t lastMs = 0;

  if (len && millis() - lastMs > 4000) {         // abandon a half-typed value
    len = 0;
    Serial.println("[cmd] input timed out");
  }

  while (Serial.available()) {
    char c = Serial.read();
    lastMs = millis();

    if (len) {                                   // collecting digits after T/N
      if (c == '\r' || c == '\n') {
        buf[len] = '\0';
        runValueCmd(buf);
        len = 0;
      } else if (len < sizeof(buf) - 1) {
        buf[len++] = c;
      }
      continue;
    }
    if (c == 'T' || c == 'N' || c == 'W') { buf[0] = c; len = 1; continue; }
    if (c == '\r' || c == '\n') continue;
    runKey(c);
  }
}

// --------------------------------------------------------------------------
// Easter egg — boot sequence: LED sweep, a jingle with a music-synced LED
// dance, and a finale flourish. Runs once, first thing, before anything else.
// The tune is a short ice-cream-van-style jingle (a stand-in for the Wall's
// cart theme); edit BOOT_JINGLE[] to taste. Set PLAY_BOOT_EGG = false to skip.
// --------------------------------------------------------------------------
static const bool PLAY_BOOT_EGG = true;

struct BootNote { uint16_t hz; uint16_t ms; };
static const BootNote BOOT_JINGLE[] = {
  {659,150},{784,150},{1047,300},{988,150},{784,150},{880,300},{0,50},
  {659,150},{784,150},{1047,300},{1175,150},{1047,150},{988,340},{0,70},
  {880,150},{988,150},{1047,150},{784,150},{659,450},
};

void bootLedFrame(int n) {
  int mode = (n / 4) % 4;
  for (int i = 0; i < 5; i++) {
    bool on = false;
    switch (mode) {
      case 0: on = (i == n % 5);                       break;  // single runner
      case 1: on = ((i + n) % 2 == 0);                 break;  // alternating
      case 2: on = (i == 2 - (n % 3) || i == 2 + (n % 3)); break;  // out/in from centre
      case 3: on = ((n * 7 + i * 3) % 5 < 2);          break;  // sparkle
    }
    digitalWrite(LED_PIN[i], on ? HIGH : LOW);
  }
}

void bootSequence() {
  if (!PLAY_BOOT_EGG) {                                // just a quick "alive" blip
    for (int i = 0; i < 5; i++) { digitalWrite(LED_PIN[i], HIGH); delay(60); digitalWrite(LED_PIN[i], LOW); }
    return;
  }
  Serial.println("[boot] \xE2\x99\xAA");

  // 1. sweep up, then back down
  for (int i = 0; i < 5; i++) { digitalWrite(LED_PIN[i], HIGH); delay(70); }
  for (int i = 4; i >= 0; i--) { digitalWrite(LED_PIN[i], LOW);  delay(70); }

  // 2. jingle + LED dance, one frame per note
  const int N = sizeof(BOOT_JINGLE) / sizeof(BOOT_JINGLE[0]);
  for (int n = 0; n < N; n++) {
    bootLedFrame(n);
    if (BOOT_JINGLE[n].hz) tone(PIEZO_PIN, BOOT_JINGLE[n].hz);
    else                   noTone(PIEZO_PIN);
    delay(BOOT_JINGLE[n].ms);
    noTone(PIEZO_PIN);
    delay(12);                                         // articulate repeated notes
  }

  // 3. finale — all blink twice with two high pips
  for (int k = 0; k < 2; k++) {
    for (int i = 0; i < 5; i++) digitalWrite(LED_PIN[i], HIGH);
    tone(PIEZO_PIN, 1568, 70); delay(100);
    for (int i = 0; i < 5; i++) digitalWrite(LED_PIN[i], LOW);
    delay(60);
  }
  noTone(PIEZO_PIN);
  currentToneHz = 0;
}

// --------------------------------------------------------------------------
// setup / loop
// --------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] Door Intrusion Detection");
  csvHeader();

  for (int i = 0; i < 3; i++) {
    pinMode(TRIG_PIN[i], OUTPUT);
    digitalWrite(TRIG_PIN[i], LOW);
    pinMode(ECHO_PIN[i], INPUT);                // divider holds the level; no internal pull needed
  }
  pinMode(SOUND_DO_PIN, INPUT_PULLUP);          // back up the module's onboard pull-up (DO is active-low)
  attachInterrupt(digitalPinToInterrupt(SOUND_DO_PIN), soundIsr,
                  SOUND_ACTIVE_LOW ? FALLING : RISING);   // catches pulses shorter than the UI tick
  for (int i = 0; i < 5; i++) {
    pinMode(LED_PIN[i], OUTPUT);
    digitalWrite(LED_PIN[i], LOW);
  }
  pinMode(PIEZO_PIN, OUTPUT);
  digitalWrite(PIEZO_PIN, LOW);

  bootSequence();                                // Easter egg: sweep + jingle + dance

  prefs.begin("door", false);
  armed      = prefs.getBool("armed", false);
  alarmed    = prefs.getBool("alarmed", false);   // a latched alarm survives power loss
  openStreak = 0;
  if (alarmed) {
    armed      = true;                            // an alarm implies the system was armed
    sirenStart = millis();                        // fresh siren budget; see persistAlarmed()
    Serial.println("[boot] ALARM latch restored from NVS — siren resuming");
  }

  dht.begin();

  // Non-blocking network: associate here, let loop() + netWatchdog bring the
  // Blynk session up. Never Blynk.begin() — it blocks setup() until connected.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Blynk.config(BLYNK_AUTH_TOKEN);

  if (armed) {
    Serial.println("[boot] NVS armed flag set — restoring (door assumed shut)");
    if (captureBaseline()) Serial.println("[boot] baseline restored — ARMED");
    else Serial.println("[boot] baseline incomplete — still ARMED (faulty sensors won't trip)");
  }

  timer.setInterval(CYCLE_MS,    startRound);    // pings run from servicePings() in loop()
  timer.setInterval(UI_MS,       uiTick);
  timer.setInterval(DHT_MS,      environmentTick);  // deferred while a round is in flight
  timer.setInterval(PUBLISH_MS,  publishStatus);
  timer.setInterval(NETCHECK_MS, netWatchdog);

  Serial.printf("[boot] ready — state %s\n", stateString());
  {
    char d[48];
    snprintf(d, sizeof(d), "%s reset=%s", stateString(), resetReasonStr());
    csvEvent("boot", d);                          // reset reason turns crashes into data
  }
  diagHelp();
}

void loop() {
  handleSerialControl();                           // TEST AID — see ALLOW_SERIAL_CONTROL
  if (netEnabled && WiFi.status() == WL_CONNECTED) Blynk.run();  // gated: an offline run() must not stall the loop
  timer.run();
  servicePings();                                  // one ping per pass, never a delay()
}
