// Test transmitter - simulates a cat collar so the handheld can be verified
// before the real collar firmware exists. Runs on a second ESP32-C3 + RA-02.
//
// Press BOOT to send one ping immediately, or leave it running for auto-pings.

#include <Arduino.h>
#include <RadioLib.h>

// ---- Radio hardware config - matches handheld ----
const uint8_t PIN_NSS  = 7;
const uint8_t PIN_DIO0 = 3;
const uint8_t PIN_RST  = 10;

SX1278 radio = new Module(PIN_NSS, PIN_DIO0, PIN_RST);

// ---- LoRa air settings - MUST match the handheld ----
const float   FREQ_MHZ   = 433.0;
const float   BANDWIDTH  = 125.0;
const uint8_t SPREADING  = 9;
const uint8_t CODING_R   = 7;
const uint8_t SYNC_WORD  = 0x12;
const int8_t  TX_POWER   = 17;  // dBm - keep legal for your region (max 20 on SX1278)
const uint16_t PREAMBLE   = 8;

const uint32_t PING_INTERVAL_MS = 5000;  // auto-ping cadence

uint32_t lastPingMs = 0;
uint32_t pingCount  = 0;

void sendPing() {
  // Stand-in payload. The real collar will send its own ID + battery here.
  char payload[24];
  snprintf(payload, sizeof(payload), "CAT-01;bat=100%%");

  pingCount++;
  int16_t state = radio.transmit(payload, strlen(payload));

  if (state == RADIOLIB_ERR_NONE) {
    Serial.print(F("[TX] ping #"));
    Serial.print(pingCount);
    Serial.print(F(" -> \""));
    Serial.print(payload);
    Serial.println(F("\""));
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

  Serial.println(F("Radio online. Pinging every 5s (or press BOOT to ping now)."));
  Serial.println();

  pinMode(0, INPUT_PULLUP);  // BOOT button
}

void loop() {
  // Manual ping on BOOT press
  if (digitalRead(0) == LOW) {
    delay(50);  // debounce
    while (digitalRead(0) == LOW) delay(10);  // wait for release
    sendPing();
    lastPingMs = millis();
  }

  // Auto-ping
  if (millis() - lastPingMs >= PING_INTERVAL_MS) {
    sendPing();
    lastPingMs = millis();
  }
}