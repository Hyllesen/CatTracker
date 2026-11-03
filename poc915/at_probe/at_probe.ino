/*
 * at_probe.ino — is the RAK3172 answering, yes or no? Nothing else.
 *
 * No RA-02 code, no P2P setup, no probe loop with retries, no state machine. It
 * sends "AT" once a second on UART2 and prints every byte that comes back, raw and
 * in hex. A module that is wired and powered answers "OK" every time.
 *
 *   -DBAUD=115200  (default) or -DBAUD=9600
 *
 * Console keys: none. Just watch it. Every print is gated on Serial.isPlugged().
 *
 * Board: esp32:esp32:esp32c3:CDCOnBoot=cdc
 */

#ifndef BAUD
#define BAUD 115200
#endif

#define PIN_RAK_RX 20   // C3 RX  <- module TX pad
#define PIN_RAK_TX 21   // C3 TX  -> module RX pad

static uint32_t sent = 0, got = 0;

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  Serial1.begin(BAUD, SERIAL_8N1, PIN_RAK_RX, PIN_RAK_TX);
  delay(1500);
  if (Serial.isPlugged()) {
    Serial.print("\nAT PROBE  baud ");
    Serial.print(BAUD);
    Serial.print("  C3 RX=GPIO");
    Serial.print(PIN_RAK_RX);
    Serial.print(" (module TX pad)  C3 TX=GPIO");
    Serial.print(PIN_RAK_TX);
    Serial.println(" (module RX pad)");
    Serial.println("Sending AT once a second. Expect 'OK' after each one.");
    Serial.println("Nothing at all back = no electrical activity on the line.");
    Serial.println("Garbage bytes      = wrong baud, or the two wires are swapped.\n");
  }
}

void loop() {
  // Send AT, then listen for 900 ms.
  Serial1.print("AT\r\n");
  Serial1.flush();
  sent++;

  uint32_t t0 = millis();
  bool any = false;
  while (millis() - t0 < 900) {
    while (Serial1.available()) {
      uint8_t b = (uint8_t)Serial1.read();
      got++;
      any = true;
      if (Serial.isPlugged()) {
        if (b >= 32 && b < 127) Serial.write(b);
        else if (b == '\r') ;                 // ignore CR, LF ends the line
        else if (b == '\n') Serial.println();
        else { Serial.print("<"); Serial.print(b, HEX); Serial.print(">"); }
      }
    }
    delay(1);
  }

  if (!any && Serial.isPlugged()) {
    Serial.print("[no reply]  sent=");
    Serial.print(sent);
    Serial.print("  bytes received=");
    Serial.println(got);
  }
}
