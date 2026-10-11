// Probe which LEDC frequencies the ESP32-C3 can actually synthesise.
// Temporary diagnostic - not part of the tracker.
// Reports attach() success only; duty readback is omitted because register
// offsets vary and that muddies the result.

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("ESP32-C3 LEDC frequency probe");
  Serial.println("X = attach OK, . = not achievable");
  Serial.println();

  for (uint8_t res = 6; res <= 14; res++) {
    Serial.print("res=");
    Serial.print(res);
    Serial.print("  ");
    uint16_t okCount = 0;
    for (uint32_t f = 1; f <= 40; f++) {
      bool ok = ledcAttach(1, f, res);
      Serial.write(ok ? 'X' : '.');
      if (ok) { okCount++; ledcDetach(1); }
    }
    Serial.print("   ok=");
    Serial.println(okCount);
    delay(30);
  }

  Serial.println();
  Serial.println("Achievable rates (Hz) per resolution:");
  for (uint8_t res = 6; res <= 14; res++) {
    Serial.print("res=");
    Serial.print(res);
    Serial.print(": ");
    for (uint32_t f = 1; f <= 40; f++) {
      if (ledcAttach(1, f, res)) {
        Serial.print(f);
        Serial.write(' ');
        ledcDetach(1);
      }
    }
    Serial.println();
    delay(30);
  }
  Serial.println("done");
}

void loop() {}