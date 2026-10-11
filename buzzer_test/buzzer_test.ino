// Buzzer test pattern - verifies wiring AND that the PWM actually reaches
// GPIO21, without needing to walk anywhere.
//
// Why this exists: on the ESP32-C3 the LEDC divider limits the LOWEST
// frequency you can synthesise, and it is lower than intuition suggests.
// Measured on this chip (see ledc_probe/):
//     8-bit -> min ~156 Hz   10-bit -> min ~39 Hz
//     11-bit -> min ~20 Hz   12-bit -> min ~10 Hz
// So the 10 Hz gating needed by an active buzzer requires 12-bit resolution.
// At 8 or 10 bit ledcAttach() fails, ledcWrite() becomes a silent no-op, and
// the buzzer just reacts to a floating pin - which looks like a buzzer that
// ignores your changes.
//
// 8-bit is still correct for 320..2200 Hz tones, so passive mode keeps it.

#include <Arduino.h>

const uint8_t PIN_BTN  = 9;
const uint8_t PIN_LED  = 8;
const uint8_t PIN_TONE = 21;   // same pin the handheld uses

const uint32_t CARRIER_HZ = 10;    // beep cycles per second
const uint8_t  TONE_RES   = 12;    // required for 10 Hz on ESP32-C3
const uint32_t TONE_MAX   = (1u << TONE_RES) - 1;   // 4095

const uint32_t STEP_MS = 1600;

const uint32_t DUTY_STEPS[] = {0, 32, 205, 1024, 2048, 3071, 4095};
const char*    DUTY_LABEL[] = {
  "SILENT",       "0.8% very faint", "5%   faint",
  "25%  light",   "50%  half",       "75%  high",
  "100% solid"
};
const uint8_t N_STEPS = sizeof(DUTY_STEPS) / sizeof(DUTY_STEPS[0]);

// LEDC low-speed group 0 on the ESP32-C3 (from soc/esp32c3/register/soc/
// ledc_reg.h): base 0x60019000, channel 0 duty at +0x08, running duty at
// +0x10. ledcAttach() hands out the lowest free channel and this sketch
// attaches exactly once, so it is always channel 0.
#define LEDC_BASE      0x60019000
#define LEDC_CH0_DUTY  (LEDC_BASE + 0x08)
volatile uint32_t* dutyReg = (volatile uint32_t*)LEDC_CH0_DUTY;

bool     attached = false;
uint32_t stepStart = 0;
uint8_t  stepIdx  = 0;
uint32_t cycles   = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  pinMode(PIN_BTN, INPUT_PULLUP);

  Serial.println();
  Serial.println("=====================================");
  Serial.println(" Buzzer Test - no walking required");
  Serial.println("=====================================");
  Serial.println();

  attached = ledcAttach(PIN_TONE, CARRIER_HZ, TONE_RES);
  if (!attached) {
    Serial.println("!! ledcAttach() FAILED - GPIO21 is not being driven.");
    Serial.println("!! Anything you hear is not real. Fix this first.");
    while (true) delay(1000);
  }
  ledcWrite(PIN_TONE, 0);

  Serial.print("LEDC attached OK: ");
  Serial.print(CARRIER_HZ);
  Serial.print(" Hz @ ");
  Serial.print(TONE_RES);
  Serial.print("-bit (max duty ");
  Serial.print(TONE_MAX);
  Serial.println(")");
  Serial.println();
  Serial.println("step | want | hw duty (read from chip) | expected");
  Serial.println("-----+------+-------------------------+----------");
  Serial.println("Listen as it cycles. Press BOOT to restart.");
  Serial.println();

  stepStart = millis();
}

void loop() {
  if (digitalRead(PIN_BTN) == LOW) {
    delay(50);
    while (digitalRead(PIN_BTN) == LOW) delay(10);
    stepIdx = 0;
    cycles++;
    stepStart = millis();
  }

  if (millis() - stepStart < STEP_MS) return;
  stepStart = millis();

  uint32_t want = DUTY_STEPS[stepIdx];
  ledcWrite(PIN_TONE, want);
  delay(5);                       // let the hardware latch
  // The LEDC duty register carries 4 fractional bits, so the stored value is
// the duty shifted left by 4. Reading it back and comparing (after the same
// shift) is the proof that the PWM really is being programmed.
uint32_t hw = (*dutyReg) & 0x1FFFF;
uint32_t expected = (want >= TONE_MAX) ? ((TONE_MAX + 1) << 4) : (want << 4);

  digitalWrite(PIN_LED, want > (TONE_MAX / 2) ? LOW : HIGH);

  Serial.print("  ");
  Serial.print(stepIdx);
  Serial.print("  |  ");
  Serial.print(want);
  Serial.print("   |  ");
  Serial.print(hw);
  Serial.print("                     | ");
  Serial.println(DUTY_LABEL[stepIdx]);

  if (hw != expected) {
    Serial.print("      ^^ MISMATCH: hw=");
    Serial.print(hw);
    Serial.print(" expected ");
    Serial.println(expected);
  } else {
    Serial.println("      ok - hardware duty matches request");
  }

  stepIdx++;
  if (stepIdx >= N_STEPS) { stepIdx = 0; cycles++; }
}