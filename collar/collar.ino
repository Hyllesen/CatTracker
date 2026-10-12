#include <Arduino.h>
// Required to link the USB CDC implementation behind Serial on the nRF52 core.
// Arduino.h declares Serial when USE_TINYUSB is set, but without this the
// Adafruit_TinyUSB library is never added to the build and the link fails with
// "undefined reference to Adafruit_USBD_CDC". The core's own examples do this too.
#include <Adafruit_TinyUSB.h>
#include <nrf.h>

// ---------------------------------------------------------------------------
// Cat collar transmitter - TENSTAR ROBOT nRF52840 Pro Micro + RA-02 (SX1278)
//
// Uses bit-banged SPI on the pins already wired to the RA-02. No DIO0 is
// required: transmit completion is detected by polling the IRQ flags register
// over SPI. Register values below match RadioLib's SX1278 driver exactly, so
// these air settings interoperate with the handheld's RadioLib receiver.
// ---------------------------------------------------------------------------

struct GPin { NRF_GPIO_Type* port; uint8_t n; };

// Silkscreen "111" = P1.11, "002" = P0.02, etc.
const GPin P_SCK  = { NRF_P1, 11 };
const GPin P_MISO = { NRF_P0,  2 };
const GPin P_MOSI = { NRF_P1, 15 };
const GPin P_CS   = { NRF_P1, 13 };
const GPin P_RST  = { NRF_P0, 31 };

inline void outMode(GPin p) { p.port->DIRSET = 1UL << p.n; }
inline void inMode(GPin p)  { p.port->PIN_CNF[p.n] = 0; }  // input, buffer connected
inline void wr(GPin p, bool v) {
  if (v) p.port->OUTSET = 1UL << p.n; else p.port->OUTCLR = 1UL << p.n;
}
inline bool rd(GPin p) { return (p.port->IN >> p.n) & 1; }

uint8_t spiByte(uint8_t out) {          // SPI mode 0, MSB first
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--) {
    wr(P_MOSI, out & (1 << i));
    delayMicroseconds(2);
    wr(P_SCK, 1);
    delayMicroseconds(2);
    in = (in << 1) | rd(P_MISO);
    wr(P_SCK, 0);
  }
  return in;
}

uint8_t readReg(uint8_t addr) {
  wr(P_CS, 0);
  spiByte(addr & 0x7F);                 // MSB 0 = read
  uint8_t v = spiByte(0x00);
  wr(P_CS, 1);
  return v;
}

void writeReg(uint8_t addr, uint8_t value) {
  wr(P_CS, 0);
  spiByte(addr | 0x80);                 // MSB 1 = write
  spiByte(value);
  wr(P_CS, 1);
}

// ---- SX1278 registers ----
const uint8_t REG_FIFO               = 0x00;
const uint8_t REG_OP_MODE            = 0x01;
const uint8_t REG_FRF_MSB            = 0x06;
const uint8_t REG_FRF_MID            = 0x07;
const uint8_t REG_FRF_LSB            = 0x08;
const uint8_t REG_PA_CONFIG          = 0x09;
const uint8_t REG_OCP                = 0x0B;
const uint8_t REG_LNA                = 0x0C;
const uint8_t REG_FIFO_TX_BASE_ADDR  = 0x0E;
const uint8_t REG_FIFO_TX_CURR_ADDR  = 0x0D;
const uint8_t REG_IRQ_FLAGS_MASK     = 0x11;
const uint8_t REG_IRQ_FLAGS          = 0x12;
const uint8_t REG_MODEM_CONFIG_1     = 0x1D;
const uint8_t REG_MODEM_CONFIG_2     = 0x1E;
const uint8_t REG_MODEM_CONFIG_3     = 0x26;
const uint8_t REG_PREAMBLE_MSB       = 0x20;
const uint8_t REG_PREAMBLE_LSB       = 0x21;
const uint8_t REG_PAYLOAD_LENGTH     = 0x22;
const uint8_t REG_HOP_PERIOD         = 0x24;
const uint8_t REG_DETECT_OPTIMIZE    = 0x31;
const uint8_t REG_INVERT_IQ          = 0x33;
const uint8_t REG_INVERT_IQ2         = 0x3B;
const uint8_t REG_DETECTION_THRESH   = 0x37;
const uint8_t REG_SYNC_WORD          = 0x39;
const uint8_t REG_PA_DAC             = 0x4D;  // SX1278-specific

// ---- Air settings - MUST match the handheld ----
const float   FREQ_MHZ   = 433.0;
const float   BANDWIDTH  = 125.0;
const uint8_t SPREADING  = 9;
const uint8_t CODING_R   = 7;    // 4/7
const uint8_t SYNC_WORD  = 0x12;
const int8_t  TX_POWER   = 17;   // dBm (PA_BOOST path)
const uint16_t PREAMBLE   = 8;

// Derived encodings
const uint8_t BW_125K_RAW = 0x70;  // ModemConfig1 bits 7:4
const uint8_t SF9_RAW     = 0x90;  // ModemConfig2 bits 7:4
const uint8_t CR_4_7_RAW  = 0x06;  // ModemConfig1 bits 3:1

// 1s while bench-testing RSSI: at 5s the handheld only gets one sample per 5m
// of walking. Put this back to 5000 before running on battery - it quadruples
// transmit energy use, which matters on a 150mAh collar.
const uint32_t PING_INTERVAL_MS = 5000;

// ---------------------------------------------------------------------------
// Battery sense
// ---------------------------------------------------------------------------
// Wire the cell's POSITIVE terminal to BATT_PIN through a resistor divider.
// Two equal resistors give the 1:2 below, which keeps a 4.2V cell (3.0V ADC
// reference) comfortably inside range and costs only ~2uA, against ~10mA for
// the radio. Use 1M + 1M: a 10k pair would draw 210uA and cost ~3mAh over a
// 16h run, which is real on a 150mAh cell.
//
// BATT_PIN is an ARDUINO PIN INDEX, not the raw port number printed on the
// silkscreen. analogRead() maps the index through the core's pin table, and
// anything that does not resolve to AIN0..AIN7 falls through to `return 0` -
// a silent hardcoded zero, not a wrong reading.
//
// The pad marked "029" is P0.29, which is Arduino index 20 on this core
// (named A6 in the Adafruit variant - note the variant's A0 is P0.04, not
// P0.02, so the letter aliases are not a safe way to reason about it).
//
// Verified by the reading, not by the pin name: a healthy 4.1V cell through
// a 1:2 divider should read ~2.05V at the pin. 0V means the pin is floating
// or wrong; 3.3V means the divider is missing and the full cell is reaching
// the ADC.
const uint8_t BATT_PIN = 20;          // Arduino index for P0.29 (silkscreen "029")
const float   BATT_DIVIDER = 2.0f;    // equal high/low resistors
const float   VREF_MV = 3300.0f;
const float   ADC_MAX = 4095.0f;      // 12-bit
const uint32_t BATT_SAMPLE_MS = 60000;

// Charge curve for a single-cell LiPo, open-circuit, at a light load. A cell
// sagging under load reads lower than these points, which is the safe
// direction - it warns early rather than late.
struct BattPoint { float mv; uint8_t pct; };
const BattPoint BATT_CURVE[] = {
  {4200, 100}, {4000, 85}, {3800, 55}, {3700, 25},
  {3600, 10},  {3400, 0},
};
const uint8_t BATT_CURVE_N = sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0]);

uint32_t lastBattMs = 0;
uint8_t batteryPct = 0;
bool batteryValid = false;
float batteryMv = 0.0f;

uint8_t readBatteryPercent() {
  uint32_t raw = analogRead(BATT_PIN);
  batteryMv = (raw * VREF_MV / ADC_MAX) * BATT_DIVIDER;

  // A pin that is not wired reads the pin's own leakage floor; treat anything
  // below a plausible cell voltage as "no reading" rather than 0%.
  if (batteryMv < 2000.0f) {
    batteryValid = false;
    return 0;
  }
  batteryValid = true;
  float mv = batteryMv;

  if (mv >= BATT_CURVE[0].mv) return BATT_CURVE[0].pct;
  for (uint8_t i = 1; i < BATT_CURVE_N; i++) {
    if (mv >= BATT_CURVE[i].mv) {
      float span = BATT_CURVE[i - 1].mv - BATT_CURVE[i].mv;
      float frac = (BATT_CURVE[i - 1].mv - mv) / span;
      float pct = BATT_CURVE[i - 1].pct + frac * (BATT_CURVE[i].pct - BATT_CURVE[i - 1].pct);
      return (uint8_t)(pct + 0.5f);
    }
  }
  return 0;
}

// Build "CAT-01;bat=NN". Built into a buffer rather than a literal so the
// reported figure tracks the cell instead of being a hardcoded 100.
void buildPayload(char* out, size_t cap) {
  if (batteryValid) {
    snprintf(out, cap, "CAT-01;bat=%u", batteryPct);
  } else {
    strcpy(out, "CAT-01;bat=--");
  }
}

// Read-modify-write of bits [startBit..endBit] in a register. RadioLib writes
// modem registers this way, which preserves the chip's power-on default bits
// (e.g. RegModemConfig2 bit2 CRC aggregation, RegInvertIQ bits 5:3, RegDetect-
// Optimize bits 7:3). Full-byte writes zero those and break interop - the
// receiver then reports RADIOLIB_ERR_LORA_HEADER_DAMAGED (-24).
void setRegBits(uint8_t reg, uint8_t value, uint8_t startBit, uint8_t endBit) {
  // Same semantics as RadioLib's Module::SPIsetRegValue: `value` is already
  // positioned at its bit offset, and bits [endBit..startBit] are replaced.
  uint8_t mask = (uint8_t)~((0xFF << (startBit + 1)) | (0xFF >> (8 - endBit)));
  uint8_t cur  = readReg(reg);
  writeReg(reg, (uint8_t)((cur & ~mask) | (value & mask)));
}

void radioInit() {
  // LoRa modem select (bit 7 = 1), then standby
  writeReg(REG_OP_MODE, 0x80 | 0x00);   // LoRa | sleep
  delay(1);
  writeReg(REG_OP_MODE, 0x80 | 0x01);   // LoRa | standby
  delay(1);

  // Frequency: FRF = (freq_hz * 2^19) / 32e6
  uint32_t frf = (uint32_t)((FREQ_MHZ * 1000000.0f * 524288.0f) / 32000000.0f);
  writeReg(REG_FRF_MSB, (frf & 0xFF0000) >> 16);
  writeReg(REG_FRF_MID, (frf & 0x00FF00) >> 8);
  writeReg(REG_FRF_LSB, frf & 0x0000FF);
  delay(1);

  // Frequency hopping off
  writeReg(REG_HOP_PERIOD, 0x00);

  // PA config: PA_BOOST, MaxPower | (power - 2), PA_DAC boost off
  writeReg(REG_PA_CONFIG, 0x80 | 0x70 | (TX_POWER - 2));
  writeReg(REG_PA_DAC, 0x04);           // default PA ramp time, boost off

  // Over-current protection on at 60 mA (raw = (60-45)/5 = 3)
  writeReg(REG_OCP, 0x20 | 3);

  // Modem config 1: bandwidth (7:4), coding rate (3:1), payload CRC on (2), explicit header (0)
  setRegBits(REG_MODEM_CONFIG_1, BW_125K_RAW, 7, 4);
  setRegBits(REG_MODEM_CONFIG_1, CR_4_7_RAW,  3, 1);
  setRegBits(REG_MODEM_CONFIG_1, 0x04,       2, 2);   // payload CRC on
  setRegBits(REG_MODEM_CONFIG_1, 0x00,       0, 0);   // explicit header

  // Modem config 2: spreading factor + single-TX mode (7:3), CRC mode (2)
  setRegBits(REG_MODEM_CONFIG_2, SF9_RAW | 0x00, 7, 3);
  setRegBits(REG_MODEM_CONFIG_2, 0x04, 2, 2);        // CRC aggregation on

  // SF7-SF12 detection optimization + threshold
  setRegBits(REG_DETECT_OPTIMIZE, 0x03, 2, 0);
  writeReg(REG_DETECTION_THRESH, 0x0A);

  // IQ inversion "off": Rx path normal (bit6=0), Tx path ON (bit0=1).
  // This is inverted-sounding but it is what RadioLib's invertIQ(false) does -
  // see RadioLib issue #778. Getting bit 0 wrong means TX "succeeds" but the
  // receiver never decodes it.
  setRegBits(REG_INVERT_IQ, 0x00, 6, 6);
  setRegBits(REG_INVERT_IQ, 0x01, 0, 0);
  writeReg(REG_INVERT_IQ2, 0x1D);

  // AGC loop enabled (gain = 0), matching RadioLib's default setGain(0)
  setRegBits(REG_MODEM_CONFIG_3, 0x04, 2, 2);   // AGC loop on

  // Preamble length
  writeReg(REG_PREAMBLE_MSB, (PREAMBLE >> 8) & 0xFF);
  writeReg(REG_PREAMBLE_LSB, PREAMBLE & 0xFF);

  // Sync word (private network)
  writeReg(REG_SYNC_WORD, SYNC_WORD);

  // FIFO base at 0, LNA boost on (bits 1:0)
  writeReg(REG_FIFO_TX_BASE_ADDR, 0x00);
  setRegBits(REG_LNA, 0x03, 1, 0);
}

// Print key registers so config problems are visible without a scope.
void printRegCheck() {
  uint32_t frf = ((uint32_t)readReg(REG_FRF_MSB) << 16) |
                 ((uint32_t)readReg(REG_FRF_MID) << 8)  |
                  (uint32_t)readReg(REG_FRF_LSB);
  float actualMHz = (float)frf * 32000000.0f / 524288.0f / 1000000.0f;

  Serial.print(F("  FRF   0x"));
  Serial.println(frf, HEX);
  Serial.print(F("  Freq  "));
  Serial.print(actualMHz, 3);
  Serial.println(F(" MHz"));
  Serial.print(F("  MC1   0x"));
  Serial.println(readReg(REG_MODEM_CONFIG_1), HEX);
  Serial.print(F("  MC2   0x"));
  Serial.println(readReg(REG_MODEM_CONFIG_2), HEX);
  Serial.print(F("  Sync  0x"));
  Serial.println(readReg(REG_SYNC_WORD), HEX);
  Serial.print(F("  InvIQ 0x"));
  Serial.println(readReg(REG_INVERT_IQ), HEX);
  Serial.print(F("  PA    0x"));
  Serial.println(readReg(REG_PA_CONFIG), HEX);
}

// Transmit one packet; returns true if TxDone was observed before timeout.
bool sendPacket(const char* payload, uint32_t timeoutMs = 3000) {
  uint8_t len = strlen(payload);

  writeReg(REG_OP_MODE, 0x80 | 0x01);   // LoRa | standby

  // FIFO pointer to base
  writeReg(REG_FIFO_TX_CURR_ADDR, 0x00);
  writeReg(REG_FIFO_TX_BASE_ADDR, 0x00);

  // Payload length + payload into FIFO
  writeReg(REG_PAYLOAD_LENGTH, len);
  for (uint8_t i = 0; i < len; i++) {
    writeReg(REG_FIFO, (uint8_t)payload[i]);
  }

  // Clear any stale IRQ flags
  writeReg(REG_IRQ_FLAGS, 0xFF);

  // Start TX
  writeReg(REG_OP_MODE, 0x80 | 0x03);   // LoRa | transmit

  // Poll IRQ flags register for TxDone (bit 3); no DIO0 pin needed
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (readReg(REG_IRQ_FLAGS) & 0x08) {
      writeReg(REG_IRQ_FLAGS, 0xFF);    // clear flags
      writeReg(REG_OP_MODE, 0x80 | 0x01);  // back to standby
      return true;
    }
    delay(1);
  }

  // Timed out - recover to standby
  writeReg(REG_OP_MODE, 0x80 | 0x01);
  return false;
}

uint32_t lastPingMs = 0;

// Set true with 'v' on the serial port when debugging.
//
// MUST default to false. The collar runs off a LiPo with no USB attached, and
// in that state nothing drains the CDC buffer. An unguarded Serial.print
// blocks, and because it blocks inside the transmit path the collar stops
// beaconing entirely - the failure looks exactly like a dead battery. Both
// checks matter: verboseLog is what we asked for, mounted() is what is
// physically possible.
static bool verboseLog = false;

bool sx1278Present = false;
uint8_t regVersion = 0;

bool logOk() {
  return verboseLog && TinyUSBDevice.mounted();
}
uint32_t pingCount  = 0;

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);

  outMode(P_SCK); outMode(P_MOSI); outMode(P_CS); outMode(P_RST);
  inMode(P_MISO);
  wr(P_SCK, 0); wr(P_CS, 1);

  // Hardware reset
  wr(P_RST, 0); delay(10);
  wr(P_RST, 1); delay(20);

  uint8_t v = readReg(0x42);
  sx1278Present = (v == 0x12);
  regVersion = v;

  // Detection stays silent. Printing happens before the radio is usable, so a
  // blocked write here would stop the collar beaconing altogether rather than
  // merely quieting it. 'v' prints the report afterwards.
  if (sx1278Present) {
    radioInit();
    analogReadResolution(12);
  }

  pinMode(0, INPUT_PULLUP);   // BOOT button -> ping now
}

// Full status report, emitted on demand by the 'v' serial command.
void printConfig() {
  Serial.println();
  Serial.println(F("====================================="));
  Serial.println(F(" Cat Tracker - Collar Transmitter"));
  Serial.println(F(" nRF52840 + RA-02 (SX1278 433MHz)"));
  Serial.println(F("====================================="));

  Serial.print(F("RegVersion = 0x"));
  Serial.println(regVersion, HEX);
  if (!sx1278Present) {
    Serial.println(F("SX1278 NOT detected - check wiring/power"));
    return;
  }
  Serial.println(F("SX1278 detected, wiring OK"));

  Serial.println(F("Config readback:"));
  printRegCheck();

  Serial.print(F("TX ready. Freq="));
  Serial.print(FREQ_MHZ, 1);
  Serial.print(F("MHz SF"));
  Serial.print(SPREADING);
  Serial.print(F(" sync=0x"));
  Serial.print(SYNC_WORD, HEX);
  Serial.println();
  Serial.print(F("Pinging every "));
  Serial.print(PING_INTERVAL_MS / 1000);
  Serial.println(F("s"));
  Serial.print(F("Battery pin: Arduino index "));
  Serial.print(BATT_PIN);
  Serial.println(F(" (P0.29, the pad marked \"029\")"));
  Serial.println(F("'v' again to silence."));
  Serial.println();
}

void loop() {
  // Serial commands are read even with logging off, so a host can turn
  // diagnostics on over the very cable that is supplying power.
  while (Serial.available()) {
    char c = (char)Serial.read();
    if ((c == 'v' || c == 'V') && TinyUSBDevice.mounted()) {
      verboseLog = !verboseLog;
      if (verboseLog) {
        printConfig();
      }
    }
  }

  // Manual ping on BOOT press
  if (digitalRead(0) == LOW) {
    delay(50);
    while (digitalRead(0) == LOW) delay(10);
    pingCount++;
    char payload[24];
    buildPayload(payload, sizeof(payload));
    bool ok = sendPacket(payload);
    if (logOk()) {
      Serial.print(F("[TX] manual ping #"));
      Serial.print(pingCount);
      Serial.println(ok ? F(" sent") : F(" TIMEOUT"));
    }
    lastPingMs = millis();
    return;
  }

  if (millis() - lastPingMs >= PING_INTERVAL_MS) {
    pingCount++;

    // Sample the cell once a minute rather than every ping. The SAADC is left
    // idle in between, and the cell cannot change meaningfully in 5s.
    if (millis() - lastBattMs >= BATT_SAMPLE_MS) {
      lastBattMs = millis();
      batteryPct = readBatteryPercent();
      if (logOk()) {
        Serial.print(F("[BATT] "));
        Serial.print(batteryPct);
        Serial.print(F("%  "));
        Serial.print(batteryMv, 0);
        Serial.print(F("mV"));
        if (!batteryValid) {
          Serial.print(F("  (no reading - check pin/wiring)"));
        }
        Serial.println();
      }
    }

    char payload[24];
    buildPayload(payload, sizeof(payload));
    bool ok = sendPacket(payload);
    if (logOk()) {
      Serial.print(F("[TX] ping #"));
      Serial.print(pingCount);
      Serial.println(ok ? F(" sent") : F(" TIMEOUT"));
    }
    lastPingMs = millis();
  }
}