// ===========================================================================
// Door Intrusion Detection — ESP32-S3 firmware
//
// A door alarm that only fires inside a user-armed window, so routine daily
// entry/exit does not cause alert fatigue.
//
// Sensing geometry (PRD §8.1): 3 ultrasonic sensors taped to the frame jamb,
// angled across the doorway. Closed door -> the beam clears the leaf and hits
// the far wall (a large, stable baseline). Opening leaf swings INTO the beam ->
// the reading DROPS. So door-open = distance decrease from baseline.
// (Flip DOOR_OPEN_IS_FARTHER for the alternate mount: sensor facing the closed
// leaf at ~15 cm, where opening makes the reading jump up.)
// The three are fired sequentially ~60 ms apart to avoid crosstalk.
//
// Arm  : auto-baseline captured now (door must be shut). Alarm on deviation
//        from each sensor's own baseline.
// Alarm: all 3 sensors must deviate > delta from their baseline, sustained for
//        PERSIST_N consecutive cycles. LATCHES until disarmed from the app.
//        Piezo is capped at 3 minutes; the latched state stays visible.
// Sound: LM393 DO is a WARNING tier only — never a precondition for the alarm
//        (break-ins can be silent). Muted while the alarm is latched.
//
// Fully autonomous offline: non-blocking Blynk, millis-based BlynkTimer loop,
// armed flag persisted to NVS. Siren still fires with no network; the intrusion
// notification is queued and sent on reconnect.
// ===========================================================================

#define BLYNK_PRINT Serial
#include "secrets.h"          // BLYNK_TEMPLATE_ID/NAME/AUTH_TOKEN — must precede Blynk

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
#define VPIN_DELTA  V5   // slider    — Deviation delta in cm (PERSIST_N is a constant)

// (GPIO map lives in include/pins.h — shared with the hw_check bring-up test)

// --------------------------------------------------------------------------
// Timing / thresholds
// --------------------------------------------------------------------------
static const uint32_t CYCLE_MS         = 300;     // main sensor + decision cycle
static const uint32_t UI_MS            = 50;      // LED blink / siren / sound sampling
static const uint32_t DHT_MS           = 2000;    // DHT11 needs >= 2 s between reads
static const uint32_t PUBLISH_MS       = 1000;    // push telemetry to Blynk
static const uint32_t NETCHECK_MS      = 5000;    // WiFi reconnect watchdog

static const uint16_t PING_GAP_MS      = 60;      // spacing between the 3 pings (anti-crosstalk)
static const uint32_t ECHO_TIMEOUT_US  = 25000;   // ~4.3 m ceiling (closed-door baseline is the far wall)
static const uint8_t  BASELINE_SAMPLES = 5;       // readings per sensor at arm time -> median
static const uint8_t  PERSIST_N        = 3;       // consecutive all-open cycles -> alarm (~0.9 s)

// Mounting geometry (PRD §8.1). false = our build: sensor angled across the
// doorway, closed door reads the far wall, an opening leaf swings into the beam
// so the reading DROPS -> door-open = (baseline - dist) > delta.
// true = alternate mount: sensor faces the closed leaf at ~15 cm, opening makes
// the reading jump up -> door-open = (dist - baseline) > delta.
static const bool     DOOR_OPEN_IS_FARTHER = false;

static const float    DELTA_DEFAULT_CM = 4.0f;    // until the V5 slider syncs from the server
static const float    DELTA_MIN_CM     = 2.0f;
static const float    DELTA_MAX_CM     = 20.0f;
static const float    BASELINE_EMA_ALPHA = 0.10f; // disarmed-only free-run baseline tracking

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

float baseline[3] = {0, 0, 0};
float dist[3]     = {0, 0, 0};
bool  healthy[3]  = {false, false, false};
bool  openFlag[3] = {false, false, false};
bool  doorOpen    = false;             // V3: all three healthy AND open

bool     soundSinceCycle = false;      // set in uiTick, consumed by sensorCycle
bool     soundActive     = false;      // stretched view, drives LED4
uint32_t soundLedUntil   = 0;

float lastTempC     = NAN;
float lastHumidity  = NAN;
float deltaCm       = DELTA_DEFAULT_CM;

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

// One HC-SR04-style ping. Returns distance in cm, or -1 if no echo returned
// within the timeout (out of range OR disconnected ECHO — both mean unhealthy).
float pingCm(int i) {
  digitalWrite(TRIG_PIN[i], LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG_PIN[i], HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN[i], LOW);

  uint32_t us = pulseIn(ECHO_PIN[i], HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return -1.0f;
  return us / 58.0f;                            // 58 us per cm, round trip
}

// Health-gate + median baseline for all three sensors. Returns false if any
// sensor did not return a usable majority of echoes (door assumed shut).
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
    Serial.printf("[arm] baseline[%d] = %.1f cm  (%d/%d echoes)\n", i + 1, baseline[i], n, BASELINE_SAMPLES);
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
      if (!armed) {                             // free-run baseline only while disarmed
        if (baseline[i] <= 0.0f) baseline[i] = d;
        else baseline[i] += (d - baseline[i]) * BASELINE_EMA_ALPHA;
      }
      float deviation = DOOR_OPEN_IS_FARTHER ? (d - baseline[i])   // leaf swings out of beam
                                             : (baseline[i] - d);  // leaf swings into beam (our mount)
      openFlag[i] = deviation > deltaCm;
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
      Serial.println("[ALARM] intrusion — all 3 sensors deviated, sustained");
      raiseIntrusionEvent();
    } else if (sound && noneOpen) {
      maybeWarn();                              // warning tier — no siren
    }
  }

  Serial.printf("[cyc] d=%.0f/%.0f/%.0f  b=%.0f/%.0f/%.0f  h=%d%d%d  o=%d%d%d  snd=%d  strk=%u  %s\n",
                dist[0], dist[1], dist[2],
                baseline[0], baseline[1], baseline[2],
                healthy[0], healthy[1], healthy[2],
                openFlag[0], openFlag[1], openFlag[2],
                sound ? 1 : 0, openStreak, stateString());
}

// --------------------------------------------------------------------------
// UI tick — LEDs, siren, sound sampling  (every UI_MS)
// --------------------------------------------------------------------------
void uiTick() {
  uint32_t now = millis();

  int  raw = digitalRead(SOUND_DO_PIN);
  bool s   = SOUND_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
  if (s) {
    soundSinceCycle = true;
    soundLedUntil   = now + SOUND_LED_HOLD_MS;
  }
  soundActive = (now < soundLedUntil);

  bool bf = (now / BLINK_FAST_MS) & 1;
  bool bs = (now / BLINK_SLOW_MS) & 1;

  // LED1-3: off = fault, solid = valid echo, blink = that sensor reads door-open
  for (int i = 0; i < 3; i++) {
    uint8_t level;
    if (!healthy[i])     level = LOW;
    else if (openFlag[i]) level = bf ? HIGH : LOW;
    else                 level = HIGH;
    digitalWrite(LED_PIN[i], level);
  }

  // LED4: sound activity
  digitalWrite(LED_SOUND, soundActive ? HIGH : LOW);

  // LED5: solid disarmed / slow blink armed / fast blink alarm
  uint8_t mode;
  if (alarmed)    mode = bf ? HIGH : LOW;
  else if (armed) mode = bs ? HIGH : LOW;
  else            mode = HIGH;
  digitalWrite(LED_MODE, mode);

  serviceSiren(now);
}

// --------------------------------------------------------------------------
// DHT11  (every DHT_MS)
// --------------------------------------------------------------------------
void readEnvironment() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) lastTempC    = t;
  if (!isnan(h)) lastHumidity = h;
  Serial.printf("[env] t=%.1f C  h=%.0f %%%s\n", lastTempC, lastHumidity,
                (isnan(t) || isnan(h)) ? "   (DHT read failed)" : "");
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

BLYNK_WRITE(VPIN_DELTA) {
  float v = param.asFloat();
  if (v < DELTA_MIN_CM) v = DELTA_MIN_CM;
  if (v > DELTA_MAX_CM) v = DELTA_MAX_CM;
  deltaCm = v;
  Serial.printf("[cfg] deviation delta = %.1f cm\n", deltaCm);
}

BLYNK_CONNECTED() {
  Serial.println("[blynk] connected");
  Blynk.syncVirtual(VPIN_DELTA);                // pull the slider value from the server
  Blynk.virtualWrite(VPIN_ARM,   armed ? 1 : 0);  // device (NVS) is the source of truth for armed
  Blynk.virtualWrite(VPIN_STATE, stateString());
  if (pendingIntrusionEvent) {
    Blynk.logEvent("intrusion_alarm");
    pendingIntrusionEvent = false;
  }
}

// --------------------------------------------------------------------------
// Serial control — TEST AID ONLY
//
// The spec (PRD §16) is "arm via the Blynk switch only". This hook lets you
// arm/disarm from the USB serial monitor so the alarm path can be bench-tested
// without standing up Blynk. Set ALLOW_SERIAL_CONTROL = false for a spec-clean
// build.  Keys:  a = arm    d = disarm    ? = print state
// --------------------------------------------------------------------------
static const bool ALLOW_SERIAL_CONTROL = true;

void handleSerialControl() {
  if (!ALLOW_SERIAL_CONTROL) return;
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'a': Serial.println("[serial] arm request");    doArm();          break;
      case 'd': Serial.println("[serial] disarm request"); disarm("serial"); break;
      case '?': Serial.printf("[serial] state=%s armed=%d alarm=%d streak=%u\n",
                              stateString(), (int)armed, (int)alarmed, openStreak); break;
      default: break;
    }
  }
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
}

void loop() {
  handleSerialControl();                           // TEST AID — see ALLOW_SERIAL_CONTROL
  if (WiFi.status() == WL_CONNECTED) Blynk.run();  // gated so an offline run() can't stall the loop
  timer.run();
}
