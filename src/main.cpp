// ===========================================================================
// Door Intrusion Detection — ESP32-S3 firmware
//
// A door alarm that only fires inside a user-armed window, so routine daily
// entry/exit does not cause alert fatigue.
//
// Sensing geometry: 3 ultrasonic sensors mounted on the frame jamb, angled
// across the doorway. Closed door -> the beam clears the leaf and lands on
// the far wall, somewhere well beyond the trip distance. Opening leaf swings
// INTO the beam -> the reading DROPS to roughly where the leaf now sits.
// (Flip DOOR_OPEN_IS_FARTHER for the alternate mount: sensor facing the
// closed leaf at ~10-20cm standoff, where opening makes the reading rise.)
// The three are fired sequentially ~60 ms apart to avoid crosstalk.
//
// Threshold, not deviation: V5 (`thresholdCm`) is an ABSOLUTE distance in cm —
// physically measure how far the leaf is from a sensor when it's swung far
// enough into the beam to count as "opening" (or the standoff gap for the
// alternate mount), and set the slider to that. A sensor reads open once its
// live distance crosses the threshold, full stop — no comparison to a
// per-sensor baseline. This was a deliberate switch away from "deviation from
// an auto-captured baseline": across a doorway the closed-door baseline is
// just whatever is across the room (tens of cm to several metres, entirely
// installation-dependent), so a delta sized relative to it was either
// swamped by real readings or arbitrary. An absolute, physically-measured
// threshold is the more honest fit for this mount.
//
// Arm  : health-checks each sensor (median of 5) and sanity-checks that its
//        closed-door reading doesn't already cross the threshold (door must
//        be shut).
// Alarm: all 3 sensors must be past the threshold, sustained for PERSIST_N
//        consecutive cycles. LATCHES until disarmed from the app.
//        Piezo is capped at 3 minutes; the latched state stays visible.
// Sound: LM393 DO is a WARNING tier only — never a precondition for the alarm
//        (break-ins can be silent). Muted while the alarm is latched.
//
// Fully autonomous offline: non-blocking Blynk, millis-based BlynkTimer loop,
// armed flag persisted to NVS. Siren still fires with no network; the intrusion
// notification is queued and sent on reconnect.
// ===========================================================================

#define BLYNK_PRINT Serial
#include "secrets.h"          // WiFi + BLYNK_TEMPLATE_ID/NAME/AUTH_TOKEN — must precede Blynk

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
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

// Mounting geometry / threshold direction. false = this build: sensor angled
// across the doorway, closed door reads the far wall, an opening leaf swings
// into the beam so the reading FALLS -> door-open = dist <= thresholdCm.
// This is the mount that's actually wired and bench-verified — do not flip
// this without also remounting the sensors to the alternate standoff below.
// true = alternate mount: sensor faces the closed leaf at ~10-20cm standoff,
// opening makes the reading RISE -> door-open = dist >= thresholdCm.
static const bool     DOOR_OPEN_IS_FARTHER = false;

// thresholdCm is an ABSOLUTE distance, physically measured at install time —
// not a deviation from a baseline. Pick values that bracket "how far is the
// leaf from a sensor once it's swung far enough into the beam to count as
// open" for this mount (or the standoff gap for the alternate mount).
static const float    THRESH_DEFAULT_CM = 30.0f;   // until the V5 slider syncs; re-measure per install
static const float    THRESH_MIN_CM     = 5.0f;
static const float    THRESH_MAX_CM     = 200.0f;

static const uint32_t SIREN_MAX_MS     = 3UL * 60UL * 1000UL;  // piezo capped at 3 minutes
static const uint16_t SIREN_SWEEP_MS   = 350;    // two-tone alternation period
static const uint16_t SIREN_LO_HZ      = 800;
static const uint16_t SIREN_HI_HZ      = 1200;

static const uint32_t WARN_COOLDOWN_MS = 60000;  // door_noise_warning rate limit
static const uint16_t SOUND_LED_HOLD_MS = 250;   // stretch momentary DO pulses so LED4 is visible

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
uint32_t soundEventCount   = 0;        // cumulative rising edges (diag view)

float lastTempC     = NAN;
float lastHumidity  = NAN;
bool  dhtOk         = false;           // last DHT read succeeded
float thresholdCm   = THRESH_DEFAULT_CM;  // absolute trip distance, V5

// Serial view: false = deployment (`[cyc]` + `[env]` lines), true = bring-up
// (one hwcheck-style status line, LED1-3 = raw echo health). Toggle with 'p'.
bool  diagMode = false;

bool     pendingIntrusionEvent = false;  // logEvent failed while offline -> retry on connect
uint32_t lastWarnMs = 0;

// --------------------------------------------------------------------------
// Forward declarations
// --------------------------------------------------------------------------
void doArm();
void disarm(const char *src);
const char *stateString();

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
  return (micros() - rise) / 58.0f;                     // 58 us per cm, round trip
}

// Health-gate + closed-door sanity check for all three sensors. Returns false
// if any sensor is unhealthy, OR its closed-door reading already crosses the
// threshold (arming would immediately read that channel as "open").
bool captureBaseline() {
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
  bool want = alarmed && (now - sirenStart < SIREN_MAX_MS);
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
// --------------------------------------------------------------------------
void raiseIntrusionEvent() {
  if (Blynk.connected()) {
    Blynk.logEvent("intrusion_alarm");
    pendingIntrusionEvent = false;
  } else {
    pendingIntrusionEvent = true;               // resume on reconnect
  }
}

void maybeWarn() {
  uint32_t now = millis();
  if (now - lastWarnMs < WARN_COOLDOWN_MS) return;
  lastWarnMs = now;
  Serial.println("[warn] door_noise_warning");
  if (Blynk.connected()) Blynk.logEvent("door_noise_warning");
}

// --------------------------------------------------------------------------
// Persistence (armed flag only — Blynk re-syncs the slider server-side)
// --------------------------------------------------------------------------
void persistArmed(bool v) {
  prefs.putBool("armed", v);
}

// --------------------------------------------------------------------------
// Main sensor + decision cycle  (every CYCLE_MS)
// --------------------------------------------------------------------------
void sensorCycle() {
  bool sound = soundSinceCycle;
  soundSinceCycle = false;

  for (int i = 0; i < 3; i++) {
    if (i) delay(PING_GAP_MS);                  // sequential firing, anti-crosstalk
    float d = pingCm(i);
    healthy[i] = (d > 0.0f);
    if (healthy[i]) {
      dist[i] = d;
      // absolute threshold, not baseline deviation — see header comment
      openFlag[i] = DOOR_OPEN_IS_FARTHER ? (d >= thresholdCm)   // leaf swings out of beam
                                          : (d <= thresholdCm); // leaf swings into beam (this build)
    } else {
      openFlag[i] = false;                      // an unhealthy sensor cannot vote "open"
    }
  }

  bool allHealthy = healthy[0] && healthy[1] && healthy[2];
  bool allOpen    = allHealthy && openFlag[0] && openFlag[1] && openFlag[2];
  bool noneOpen   = !openFlag[0] && !openFlag[1] && !openFlag[2];
  doorOpen = allOpen;

  if (armed && !alarmed) {
    openStreak = allOpen ? (openStreak + 1) : 0;
    if (openStreak >= PERSIST_N) {
      alarmed    = true;                        // LATCHED
      sirenStart = millis();
      Serial.println("[ALARM] intrusion — all 3 sensors past threshold, sustained");
      raiseIntrusionEvent();
    } else if (sound && noneOpen) {
      maybeWarn();                              // warning tier — no siren
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
}

// --------------------------------------------------------------------------
// UI tick — LEDs, siren, sound sampling  (every UI_MS)
// --------------------------------------------------------------------------
void uiTick() {
  uint32_t now = millis();

  int  raw = digitalRead(SOUND_DO_PIN);
  bool s   = SOUND_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
  if (s) {
    soundSinceCycle   = true;
    soundSincePublish = true;
    soundLedUntil      = now + SOUND_LED_HOLD_MS;
  }
  static bool lastS = false;
  if (s && !lastS) soundEventCount++;             // rising edge, for the diag line
  lastS = s;
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
  if (!isnan(t)) lastTempC    = t;
  if (!isnan(h)) lastHumidity = h;
  if (!diagMode)                                  // diag view carries t/h on its status line
    Serial.printf("[env] t=%.1f C  h=%.0f %%%s\n", lastTempC, lastHumidity,
                  dhtOk ? "" : "   (DHT read failed)");
}

// --------------------------------------------------------------------------
// Telemetry push  (every PUBLISH_MS)
// --------------------------------------------------------------------------
void publishStatus() {
  if (!Blynk.connected()) return;
  if (!isnan(lastTempC))    Blynk.virtualWrite(VPIN_TEMP, lastTempC);
  if (!isnan(lastHumidity)) Blynk.virtualWrite(VPIN_HUM,  lastHumidity);
  Blynk.virtualWrite(VPIN_DOOR,  doorOpen ? 1 : 0);
  Blynk.virtualWrite(VPIN_STATE, stateString());
  Blynk.virtualWrite(VPIN_SOUND, soundSincePublish ? 1 : 0);
  soundSincePublish = false;
  Blynk.virtualWrite(VPIN_ALARM, alarmed ? 1 : 0);
}

// --------------------------------------------------------------------------
// WiFi reconnect watchdog  (every NETCHECK_MS)
// --------------------------------------------------------------------------
void netWatchdog() {
  if (WiFi.status() == WL_CONNECTED) return;
  static uint32_t lastKick = 0;
  uint32_t now = millis();
  if (now - lastKick < 15000) return;
  lastKick = now;
  Serial.println("[net] WiFi down — reconnecting");
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
    if (Blynk.connected()) {
      Blynk.virtualWrite(VPIN_ARM, 0);          // bounce the switch back
      Blynk.virtualWrite(VPIN_STATE, "DISARMED");
    }
    return;
  }
  armed      = true;
  alarmed    = false;
  openStreak = 0;
  persistArmed(true);
  armChirp();
  Serial.println("[arm] ARMED");
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
  sirenOff();
  persistArmed(false);
  if (wasActive) {
    Serial.printf("[disarm] by %s\n", src);
    disarmChirp();
  }
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

BLYNK_WRITE(VPIN_THRESH) {
  float v = param.asFloat();
  if (v < THRESH_MIN_CM) v = THRESH_MIN_CM;
  if (v > THRESH_MAX_CM) v = THRESH_MAX_CM;
  thresholdCm = v;
  Serial.printf("[cfg] trip threshold = %.1f cm\n", thresholdCm);
}

BLYNK_CONNECTED() {
  Serial.println("[blynk] connected");
  Blynk.syncVirtual(VPIN_THRESH);               // pull the slider value from the server
  Blynk.virtualWrite(VPIN_ARM,   armed ? 1 : 0);  // device (NVS) is the source of truth for armed
  Blynk.virtualWrite(VPIN_STATE, stateString());
  Blynk.virtualWrite(VPIN_ALARM, alarmed ? 1 : 0);
  if (pendingIntrusionEvent) {
    Blynk.logEvent("intrusion_alarm");
    pendingIntrusionEvent = false;
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
  Serial.println(F("[cmd] p toggle bring-up view | v ping-burst  t self-test  b buzzer  m pinmap"));
  Serial.println(F("[cmd] a arm  d disarm  ? state  h help"));
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

void handleSerialControl() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'a':
        if (ALLOW_SERIAL_CONTROL) { Serial.println("[serial] arm request"); doArm(); }
        else Serial.println("[serial] a/d disabled (ALLOW_SERIAL_CONTROL=false)");
        break;
      case 'd':
        if (ALLOW_SERIAL_CONTROL) { Serial.println("[serial] disarm request"); disarm("serial"); }
        else Serial.println("[serial] a/d disabled (ALLOW_SERIAL_CONTROL=false)");
        break;
      case '?': Serial.printf("[serial] state=%s armed=%d alarm=%d streak=%u threshold=%.1fcm\n",
                              stateString(), (int)armed, (int)alarmed, openStreak, thresholdCm); break;
      case 'p':
        diagMode = !diagMode;
        Serial.printf("[serial] bring-up view %s\n", diagMode ? "ON  (US/SND/DHT line)" : "OFF (back to [cyc])");
        break;
      case 'v': diagPingBurst(); break;
      case 't': diagSelfTest();  break;
      case 'b': diagBuzzer();    break;
      case 'm': diagPinMap();    break;
      case 'h': diagHelp();      break;
      default: break;
    }
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

  for (int i = 0; i < 3; i++) {
    pinMode(TRIG_PIN[i], OUTPUT);
    digitalWrite(TRIG_PIN[i], LOW);
    pinMode(ECHO_PIN[i], INPUT);                // divider holds the level; no internal pull needed
  }
  pinMode(SOUND_DO_PIN, INPUT_PULLUP);          // back up the module's onboard pull-up (DO is active-low)
  for (int i = 0; i < 5; i++) {
    pinMode(LED_PIN[i], OUTPUT);
    digitalWrite(LED_PIN[i], LOW);
  }
  pinMode(PIEZO_PIN, OUTPUT);
  digitalWrite(PIEZO_PIN, LOW);

  bootSequence();                                // Easter egg: sweep + jingle + dance

  prefs.begin("door", false);
  armed      = prefs.getBool("armed", false);
  alarmed    = false;
  openStreak = 0;

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

  timer.setInterval(CYCLE_MS,    sensorCycle);
  timer.setInterval(UI_MS,       uiTick);
  timer.setInterval(DHT_MS,      readEnvironment);
  timer.setInterval(PUBLISH_MS,  publishStatus);
  timer.setInterval(NETCHECK_MS, netWatchdog);

  Serial.printf("[boot] ready — state %s\n", stateString());
  diagHelp();
}

void loop() {
  handleSerialControl();                           // TEST AID — see ALLOW_SERIAL_CONTROL
  if (WiFi.status() == WL_CONNECTED) Blynk.run();  // gated so an offline run() can't stall the loop
  timer.run();
}
