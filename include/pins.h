#pragma once
#include <Arduino.h>

// ===========================================================================
// GPIO map — ESP32-S3-WROOM-1 (ESP32-S3-DevKitC-1)
//
// Single source of truth for the wiring, used by src/main.cpp. (Bring-up
// checks used to live in a separate hw_check.cpp; they're now serial
// commands in main.cpp — see diagHelp().)
//
// The S3 has no input-only pins — every GPIO is full bidirectional with
// pull-ups/pull-downs. Pins below are picked from the universally-safe low
// band and avoid:
//   - strapping pins        : GPIO0, GPIO3, GPIO45, GPIO46
//   - native USB / JTAG     : GPIO19, GPIO20
//   - UART0 console         : GPIO43 (TX), GPIO44 (RX)   <- the "UART" USB-C port
//   - SPI flash / PSRAM     : GPIO26..GPIO37
//   - on-board RGB LED      : GPIO38 / GPIO48
//
// The S3 is still NOT 5V tolerant: each ultrasonic ECHO keeps its 1k+2k
// divider (ECHO idles at 5V).
// ===========================================================================

static const uint8_t ECHO_PIN[3]  = {4, 5, 6};        // input, each behind a 1k+2k divider
static const uint8_t TRIG_PIN[3]  = {15, 16, 17};     // output, one per sensor (never shared)
static const uint8_t SOUND_DO_PIN = 7;                // LM393 DO; power the module from 3V3
static const uint8_t DHT_PIN      = 18;               // DHT11 data
static const uint8_t LED_PIN[5]   = {8, 9, 10, 11, 12};  // LED1..LED5, each via ~330 ohm
static const uint8_t PIEZO_PIN    = 13;               // passive piezo, tone() / LEDC

// LM393 DO idles HIGH and is pulled LOW when the level crosses the pot
// threshold. Flip to false if your module is wired the other way.
static const bool SOUND_ACTIVE_LOW = true;

#define LED_SOUND  LED_PIN[3]   // LED4 — sound activity
#define LED_MODE   LED_PIN[4]   // LED5 — system mode
