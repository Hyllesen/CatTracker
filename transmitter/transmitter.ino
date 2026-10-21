// Test transmitter - simulates a cat collar so the handheld can be verified
// before the real collar firmware exists. Runs on a second ESP32-C3 + RA-02.
//
// Press BOOT to send one ping immediately, or leave it running for auto-pings.
//
// It speaks the same binary frame as collar/collar.ino, so it is a drop-in
// stand-in for a real collar - including the per-collar ping period, which is
// what makes multi-collar behaviour testable without three boards.
//
// Fake several collars from one board with -DFAKE_COLLARS=n. That gives n
// collars, each with its own id, its own seq counter and its own period, all
// from one transmitter:
//
//   arduino-cli compile --fqbn "esp32:esp32:esp32c3" \
//     --board-options "CDCOnBoot=cdc" \
//     --build-property "compiler.cpp.extra_flags=-DFAKE_COLLARS=3" \
//     transmitter/transmitter.ino
//
// A single fake collar (the default) is -DFAKE_COLLARS=1 with id 0.

#include <Arduino.h>
#include <RadioLib.h>

// ---- Radio hardware config - matches handheld ----
const uint8_t PIN_NSS  = 7;
const uint8_t PIN_DIO0 = 3;
const uint8_t PIN_RST  = 10;

SX1278 radio = new Module(PIN_NSS, PIN_DIO0, PIN_RST);

// ---- LoRa air settings - MUST match the collar ----
const float   FREQ_MHZ   = 433.0;
const float   BANDWIDTH  = 125.0;
const uint8_t SPREADING  = 9;
const uint8_t CODING_R   = 7;
const uint8_t SYNC_WORD  = 0x12;
const int8_t  TX_POWER   = 17;  // dBm - keep legal for your region (max 20 on SX1278)
const uint16_t PREAMBLE   = 8;

// ---- Packet format - MUST match collar/collar.ino ----
const uint8_t FRAME_MAGIC        = 0xC1;
const uint8_t FRAME_LEN          = 6;
const uint8_t FRAME_FLAG_LOW_BATT = 0x01;

// ---- How many collars this board pretends to be ----
#ifndef FAKE_COLLARS
#define FAKE_COLLARS 1
#endif
#ifndef MAX_COLLARS
#define MAX_COLLARS 3
#endif

#if FAKE_COLLARS < 1 || FAKE_COLLARS > MAX_COLLARS
#error "FAKE_COLLARS must be 1..MAX_COLLARS"
#endif

// The collar's nominal ping interval and the per-id offset. Each fake collar
// keeps its own period, so faking three collars reproduces the real thing's
// timing - including the slow drift between collars that keeps two of them
// from being permanently stuck on top of each other.
const uint32_t PING_INTERVAL_MS    = 8000;
const uint32_t PING_PERIOD_STEP_MS = 137;

struct FakeCollar {
  uint8_t  id;
  uint8_t  seq;
  uint8_t  batteryPct;
  uint32_t nextDueMs;
  uint32_t periodMs;
};
FakeCollar fake[FAKE_COLLARS];

void buildFrame(uint8_t* out, uint8_t id, uint8_t seq, uint8_t batt) {
  out[0] = FRAME_MAGIC;
  out[1] = seq;
  out[2] = id;
  out[3] = batt;
  out[4] = (batt < 15) ? FRAME_FLAG_LOW_BATT : 0x00;
  out[5] = 0x00;
}

void sendPing(FakeCollar& c) {
  uint8_t frame[FRAME_LEN];
  buildFrame(frame, c.id, c.seq, c.batteryPct);
  c.seq++;

  int16_t state = radio.transmit(frame, FRAME_LEN);

  if (state == RADIOLIB_ERR_NONE) {
    Serial.print(F("[TX] id "));
    Serial.print(c.id);
    Serial.print(F(" seq "));
    Serial.print(c.seq - 1);
    Serial.print(F(" -> "));
    for (uint8_t i = 0; i < FRAME_LEN; i++) {
      Serial.print(F("0x"));
      if (frame[i] < 0x10) Serial.print('0');
      Serial.print(frame[i], HEX);
      Serial.print(' ');
    }
    Serial.println();
  } else {
    Serial.print(F("[TX] transmit failed, code "));
    Serial.println(state);
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);

  Serial.println();
  Serial.println(F("====================================="));
  Serial.println(F(" Cat Tracker - Test Transmitter"));
  Serial.println(F(" ESP32-C3 + RA-02 (SX1278 433MHz)"));
  Serial.println(F("====================================="));

  int16_t state = radio.begin(FREQ_MHZ, BANDWIDTH, SPREADING, CODING_R,
                              SYNC_WORD, TX_POWER, PREAMBLE);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print(F("FATAL: SX1278 init failed, code "));
    Serial.println(state);
    while (true) delay(1000);
  }

  // Stagger the fake collars so they do not all fire at t=0. The first one
  // goes immediately; the rest are spread over their first period.
  for (uint8_t i = 0; i < FAKE_COLLARS; i++) {
    fake[i].id = i;
    fake[i].seq = 0;
    fake[i].batteryPct = 100 - i * 10;   // different levels, so they are easy to tell apart
    fake[i].periodMs = PING_INTERVAL_MS + i * PING_PERIOD_STEP_MS;
    fake[i].nextDueMs = millis() + i * fake[i].periodMs / FAKE_COLLARS;
  }

  Serial.print(F("Radio online. Faking "));
  Serial.print(FAKE_COLLARS);
  Serial.println(F(" collar(s):"));
  for (uint8_t i = 0; i < FAKE_COLLARS; i++) {
    Serial.print(F("  id "));
    Serial.print(fake[i].id);
    Serial.print(F("  every "));
    Serial.print(fake[i].periodMs);
    Serial.print(F(" ms  battery "));
    Serial.print(fake[i].batteryPct);
    Serial.println(F("%"));
  }
  Serial.println();

  pinMode(0, INPUT_PULLUP);  // BOOT button
}

void loop() {
  // Manual ping on BOOT press - always collar 0, so the button stays useful
  // with several fake collars running.
  if (digitalRead(0) == LOW) {
    delay(50);  // debounce
    while (digitalRead(0) == LOW) delay(10);  // wait for release
    sendPing(fake[0]);
    fake[0].nextDueMs = millis() + fake[0].periodMs;
  }

  // Auto-ping, each fake collar on its own schedule.
  uint32_t now = millis();
  for (uint8_t i = 0; i < FAKE_COLLARS; i++) {
    if ((int32_t)(now - fake[i].nextDueMs) >= 0) {
      sendPing(fake[i]);
      fake[i].nextDueMs = now + fake[i].periodMs;
    }
  }

  delay(10);
}
