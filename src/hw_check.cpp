// ===========================================================================
// hw_check — hardware bring-up test for Door Intrusion Detection
//
// Build/flash this INSTEAD of the firmware to prove the wiring in isolation:
//
//   pio run -e hwcheck -t upload && pio device monitor -e hwcheck
//
// No WiFi, no Blynk, no NVS — just the pins. If a sensor misbehaves here the
// problem is wiring/power/the part; if it works here but not in the firmware,
// the problem is the firmware.
//
// On boot it runs an OUTPUT self-test (LEDs 1..5 in sequence, then a piezo
// chirp), then loops ~2x/second printing every input. Single-key serial
// commands (send 'h' for the list) let you poke individual outputs.
// ===========================================================================

#include <Arduino.h>
#include <DHT.h>
#include "pins.h"

static const uint32_t ECHO_TIMEOUT_US = 30000;   // ~5 m — generous, so "no echo" means no echo
static const uint16_t PING_GAP_MS     = 60;      // same sequential spacing the firmware uses
static const uint32_t LOOP_MS         = 500;
static const uint32_t DHT_MS          = 2000;

DHT dht(DHT_PIN, DHT11);

bool     ledState[5] = {false, false, false, false, false};
uint32_t soundEvents = 0;
bool     lastSound   = false;
uint32_t lastDhtMs   = 0;
float    tC = NAN, rh = NAN;
bool     dhtOk = false;

// --------------------------------------------------------------------------
void printPinMap() {
  Serial.println();
  Serial.println(F("=== Door Intrusion Detection — HARDWARE CHECK ==="));
  Serial.println(F("Board: ESP32-S3-WROOM-1   Serial: UART0 @ 115200"));
  Serial.println(F("--------------------------------------------------"));
  Serial.printf ("  ECHO 1/2/3 : GPIO %d / %d / %d   (each behind a 1k+2k divider)\n",
                 ECHO_PIN[0], ECHO_PIN[1], ECHO_PIN[2]);
  Serial.printf ("  TRIG 1/2/3 : GPIO %d / %d / %d\n", TRIG_PIN[0], TRIG_PIN[1], TRIG_PIN[2]);
  Serial.printf ("  SOUND DO   : GPIO %d           (module powered from 3V3)\n", SOUND_DO_PIN);
  Serial.printf ("  DHT11 DATA : GPIO %d\n", DHT_PIN);
  Serial.printf ("  LED 1..5   : GPIO %d / %d / %d / %d / %d   (each via ~330 ohm)\n",
                 LED_PIN[0], LED_PIN[1], LED_PIN[2], LED_PIN[3], LED_PIN[4]);
  Serial.printf ("  PIEZO      : GPIO %d           (passive)\n", PIEZO_PIN);
  Serial.println(F("--------------------------------------------------"));
}

void printHelp() {
  Serial.println(F("commands:  1-5 toggle LED   a all-on   z all-off"));
  Serial.println(F("           b buzzer test    p single ping (verbose)   h help"));
}

// --------------------------------------------------------------------------
void setLed(int i, bool on) {
  ledState[i] = on;
  digitalWrite(LED_PIN[i], on ? HIGH : LOW);
}

void buzzerTest() {
  Serial.println(F("[out] piezo: 1500 Hz, 2200 Hz, sweep"));
  tone(PIEZO_PIN, 1500, 150); delay(200);
  tone(PIEZO_PIN, 2200, 150); delay(200);
  for (int f = 800; f <= 2000; f += 120) { tone(PIEZO_PIN, f, 40); delay(45); }
  noTone(PIEZO_PIN);
}

void outputSelfTest() {
  Serial.println(F("[out] LED sweep 1..5 (watch each LED light in turn)"));
  for (int i = 0; i < 5; i++) { setLed(i, true); delay(300); setLed(i, false); }
  Serial.println(F("[out] all LEDs on for 1 s"));
  for (int i = 0; i < 5; i++) setLed(i, true);
  delay(1000);
  for (int i = 0; i < 5; i++) setLed(i, false);
  buzzerTest();
  Serial.println(F("[out] self-test done"));
}

// --------------------------------------------------------------------------
// One HC-SR04 ping. Returns echo width in microseconds, or 0 on timeout.
uint32_t ping(int i, bool verbose) {
  if (digitalRead(ECHO_PIN[i]) == HIGH && verbose)
    Serial.printf("   US%d: ECHO already HIGH before trigger — check divider / wiring\n", i + 1);

  digitalWrite(TRIG_PIN[i], LOW);  delayMicroseconds(3);
  digitalWrite(TRIG_PIN[i], HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN[i], LOW);

  uint32_t us = pulseIn(ECHO_PIN[i], HIGH, ECHO_TIMEOUT_US);
  if (verbose) {
    if (us == 0) Serial.printf("   US%d: NO ECHO (timeout %lu us)\n", i + 1, ECHO_TIMEOUT_US);
    else         Serial.printf("   US%d: echo %lu us  ->  %.1f cm\n", i + 1, us, us / 58.0f);
  }
  return us;
}

void verbosePingAll() {
  Serial.println(F("[in ] manual ping burst:"));
  for (int i = 0; i < 3; i++) { ping(i, true); delay(PING_GAP_MS); }
}

// --------------------------------------------------------------------------
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '5') {
      int i = c - '1';
      setLed(i, !ledState[i]);
      Serial.printf("[out] LED%d -> %s\n", i + 1, ledState[i] ? "ON" : "OFF");
    } else if (c == 'a') {
      for (int i = 0; i < 5; i++) setLed(i, true);
      Serial.println(F("[out] all LEDs ON"));
    } else if (c == 'z') {
      for (int i = 0; i < 5; i++) setLed(i, false);
      Serial.println(F("[out] all LEDs OFF"));
    } else if (c == 'b') {
      buzzerTest();
    } else if (c == 'p') {
      verbosePingAll();
    } else if (c == 'h') {
      printHelp();
    }
  }
}

// --------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  for (int i = 0; i < 3; i++) {
    pinMode(TRIG_PIN[i], OUTPUT);
    digitalWrite(TRIG_PIN[i], LOW);
    pinMode(ECHO_PIN[i], INPUT);
  }
  pinMode(SOUND_DO_PIN, INPUT_PULLUP);
  for (int i = 0; i < 5; i++) { pinMode(LED_PIN[i], OUTPUT); digitalWrite(LED_PIN[i], LOW); }
  pinMode(PIEZO_PIN, OUTPUT); digitalWrite(PIEZO_PIN, LOW);

  dht.begin();

  printPinMap();
  printHelp();
  outputSelfTest();
  Serial.println(F("\n[loop] live readings — LED1-3 = echo OK, LED4 = sound, LED5 = heartbeat\n"));
}

// --------------------------------------------------------------------------
void loop() {
  handleSerial();

  static uint32_t lastLoop = 0;
  uint32_t now = millis();
  if (now - lastLoop < LOOP_MS) return;
  lastLoop = now;

  // ---- ultrasonics (sequential, 60 ms apart) ----
  uint32_t us[3];
  bool     echoOk[3];
  for (int i = 0; i < 3; i++) {
    us[i]     = ping(i, false);
    echoOk[i] = (us[i] > 0);
    if (i < 2) delay(PING_GAP_MS);
  }

  // ---- sound DO ----
  int  raw   = digitalRead(SOUND_DO_PIN);
  bool sound = SOUND_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
  if (sound && !lastSound) soundEvents++;
  lastSound = sound;

  // ---- DHT11 (>= 2 s apart) ----
  if (now - lastDhtMs >= DHT_MS) {
    lastDhtMs = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    dhtOk = !isnan(t) && !isnan(h);
    if (!isnan(t)) tC = t;
    if (!isnan(h)) rh = h;
  }

  // ---- LED feedback ----
  for (int i = 0; i < 3; i++) digitalWrite(LED_PIN[i], echoOk[i] ? HIGH : LOW);
  digitalWrite(LED_SOUND, sound ? HIGH : LOW);
  static bool hb = false; hb = !hb;
  digitalWrite(LED_MODE, hb ? HIGH : LOW);

  // ---- report ----
  char d[3][12];
  for (int i = 0; i < 3; i++) {
    if (echoOk[i]) snprintf(d[i], sizeof(d[i]), "%.1fcm", us[i] / 58.0f);
    else           snprintf(d[i], sizeof(d[i]), "no-echo");
  }
  char env[20];
  if (dhtOk) snprintf(env, sizeof(env), "%.1fC %.0f%%", tC, rh);
  else       snprintf(env, sizeof(env), "READ FAILED");

  Serial.printf("US1 %-9s US2 %-9s US3 %-9s | SND %-5s ev=%lu | DHT %s\n",
                d[0], d[1], d[2], sound ? "SOUND" : "quiet",
                (unsigned long)soundEvents, env);
}
