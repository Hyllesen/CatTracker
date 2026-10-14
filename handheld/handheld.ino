#include <Arduino.h>
#include <RadioLib.h>
#include <esp_partition.h>
#include <Preferences.h>

// ---- Radio hardware config (ESP32-C3 SuperMini + RA-02 / SX1278) ----
// SuperMini-safe pins: GPIO2/8/9 are ESP32-C3 strapping pins, 18/19 are USB,
// 20/21 are UART0. GPIO4-7 and GPIO10 are free for SPI + IRQ.
const uint8_t PIN_NSS  = 7;
const uint8_t PIN_DIO0 = 3;
const uint8_t PIN_RST  = 10;

// Keep a handle on the Module so we can read raw registers for diagnostics.
Module* mod = new Module(PIN_NSS, PIN_DIO0, PIN_RST);
SX1278 radio = mod;

const uint8_t PIN_LED = 8;   // onboard status LED (active low)
const uint8_t PIN_BTN = 9;   // BOOT button, waypoint marker
const uint8_t PIN_TONE = 21; // passive buzzer via LEDC PWM (UART0 TX, unused on USB CDC)

Preferences prefs;
bool walkArmed = false;

// The LED is driven from tracked state, never digitalRead() on an output pin
// (which returns a constant and made the "blink" look steady). Pulses are
// non-blocking so the radio is never idle waiting on a delay().
bool ledLit = false;
uint32_t ledOffAt = 0;
void ledPulse(uint16_t ms) {
  digitalWrite(PIN_LED, LOW);
  ledLit = true;
  ledOffAt = millis() + ms;
}
void ledService() {
  if (ledLit && (int32_t)(millis() - ledOffAt) >= 0) {
    digitalWrite(PIN_LED, HIGH);
    ledLit = false;
  }
}

// ---- LoRa air settings - MUST match the collar ----
const float   FREQ_MHZ   = 433.0;
const float   BANDWIDTH  = 125.0;
const uint8_t SPREADING  = 9;
const uint8_t CODING_R   = 7;
const uint8_t SYNC_WORD  = 0x12;
const int8_t  TX_POWER   = 17;
const uint16_t PREAMBLE   = 8;

// ---- Signal-quality config ----
// Calibrated from an actual measured walk, not from theory. The original
// -45 dBm ceiling was datasheet "point blank" and pegged the bar at 100% for
// everything within ~10m, leaving the whole usable range compressed into the
// top 20% of the bar (and duty 233..255, which is inaudible on a fixed-pitch
// active buzzer). These bounds are the measured desk (-34) to detection edge.
const float RSSI_CEIL  = -40.0;   // ~arm's length
const float RSSI_FLOOR = -75.0;   // below this the signal is not trustworthy
// 12 packets ~= 96s of collar pings at 8s. Measured noise floor is only ~0.7dB,
// so this smooths multipath spikes without feeling laggy when you start
// walking.
const uint8_t SMOOTH_N  = 12;
// Must comfortably exceed one collar ping interval, or a couple of missed
// packets trip "signal lost" while the collar is perfectly fine. At 8s pings
// the old 8000 would fire on almost every cycle.
const uint32_t STALE_MS = 15000;

// ---- Audio feedback ----
// Two buzzer types need different handling, so the mode is switchable at
// runtime with 't' (test which one you have before committing):
//   PASSIVE - has no oscillator; you drive a tone, so pitch can track RSSI.
//   ACTIVE  - has its own oscillator (most 2-wire "DC 3-24V" buzzers). It
//             makes a fixed pitch, so we chop the supply instead and vary the
//             beep rate.
// Plain constants rather than an enum: the Arduino preprocessor hoists
// function prototypes above type definitions, which breaks enum parameters.
const uint8_t TONE_OFF = 0;
const uint8_t TONE_PASSIVE = 1;
const uint8_t TONE_ACTIVE = 2;
uint8_t toneMode = TONE_ACTIVE;

const uint32_t TONE_LO_HZ    = 320;    // passive: far away
const uint32_t TONE_HI_HZ    = 2200;   // passive: close
// Active buzzer: chop the supply at 10 Hz and vary duty. Weak signal = rare
// faint tick, strong signal = near-continuous buzz.
const uint32_t TONE_CARRIER  = 10;
const uint32_t TONE_DUTY_MIN = 32;     // ~0.8% duty at 12-bit
const uint32_t TONE_DUTY_MAX = 4095;   // 100% duty at 12-bit

// Measured on this C3 (see ledc_probe/): the LEDC divider is the limiting
// factor, not the duty bits. Minimum frequency reachable per resolution is
// roughly: 8-bit ~156 Hz, 10-bit ~39 Hz, 11-bit ~20 Hz, 12-bit ~10 Hz.
// So 8-bit is fine for 320..2200 Hz tones, but the 10 Hz beep gating needs
// 12-bit. Asking for 10 Hz at 8 or 10 bit makes ledcAttach() fail, and
// ledcWrite() to an unattached pin is a silent no-op - which looks exactly
// like a buzzer that ignores your changes.
const uint8_t TONE_RES_PASSIVE = 8;    // min ~156 Hz, fine for tones
const uint8_t TONE_RES_ACTIVE  = 12;   // min ~10 Hz, needed for 10 Hz gating

uint32_t toneHz = 0;
uint32_t toneDuty = 0;
bool toneAttached = false;

uint8_t rssiToPercent(float rssi);   // defined below

void toneInit() {
  toneSetMode(TONE_ACTIVE);
}

void toneSetMode(uint8_t m) {
  // Re-attaching with different params needs a detach first, otherwise
  // ledcAttach() fails with "pin already attached to LEDC".
  if (toneAttached) {
    ledcDetach(PIN_TONE);
    toneAttached = false;
  }

  if (m == TONE_OFF) { toneMode = TONE_OFF; return; }

  uint8_t  res = (m == TONE_PASSIVE) ? TONE_RES_PASSIVE : TONE_RES_ACTIVE;
  uint32_t f0  = (m == TONE_PASSIVE) ? 2000 : TONE_CARRIER;

  // Never let a failed attach pass silently - ledcWrite() to an unattached
  // pin is a no-op and reads as "the buzzer ignores my changes".
  toneAttached = ledcAttach(PIN_TONE, f0, res);
  if (!toneAttached) {
    toneMode = TONE_OFF;
    Serial.println(F("!! LEDC attach FAILED - buzzer disabled"));
    return;
  }
  toneMode = m;
  ledcWriteTone(PIN_TONE, 0);
  toneHz = 0;
  toneDuty = 0;
}

const char* toneModeName(uint8_t m) {
  switch (m) {
    case TONE_OFF:    return "OFF";
    case TONE_PASSIVE: return "PASSIVE (pitch = distance)";
    default:           return "ACTIVE (beep rate = distance)";
  }
}

void updateTone(bool haveSignal, float rssi) {
  if (toneMode == TONE_OFF || !haveSignal) {
    ledcWriteTone(PIN_TONE, 0);
    toneHz = 0;
    toneDuty = 0;
    return;
  }
  uint8_t pct = rssiToPercent(rssi);
  if (toneMode == TONE_PASSIVE) {
    uint32_t hz = TONE_LO_HZ + (uint32_t)(TONE_HI_HZ - TONE_LO_HZ) * pct / 100;
    ledcWriteTone(PIN_TONE, hz);
    toneHz = hz;
    toneDuty = 255;
  } else {
    uint32_t duty = TONE_DUTY_MIN +
                    (uint32_t)(TONE_DUTY_MAX - TONE_DUTY_MIN) * pct / 100;
    ledcWrite(PIN_TONE, duty);
    toneHz = 0;
    toneDuty = duty;
  }
}

float rssiHistory[SMOOTH_N];
uint8_t rssiIdx = 0, rssiCount = 0;
float rssiSmoothed = 0.0, rssiPeak = -999.0;
uint32_t lastPacketMs = 0, packetCount = 0;
bool everHeard = false;

// ---- Walk-test log (flash backed) ----
// Logging must survive being unplugged from the power bank and replugged into
// the laptop, which resets the chip and wipes RAM. Samples go to the spare
// "spiffs" data partition (1.375MB on the default 4MB scheme).
//
// Layout: a sequence of 4KB pages. Each page starts with a uint32 sample count
// for that page; erased flash reads 0xFFFFFFFF, which marks the end of the log.
// Self-describing, so no separate header sector to keep in sync and one sector
// erase per page write instead of two.
const uint32_t SAMPLE_INTERVAL_MS = 500;
// Keep the radio in RX for far longer than the sample period. A short window
// makes each receive() cycle back through standby, and the collar's ~200ms
// packets then land in the dead time - measured at only ~20% getting through.
const uint32_t RX_TIMEOUT_MS      = 1000;

const size_t   PAGE_BYTES    = 4096;
const size_t   PAGE_COUNT_BYTES = 4;
const size_t   SAMPLE_BYTES  = 12;
const size_t   SAMPLES_PAGE  = (PAGE_BYTES - PAGE_COUNT_BYTES) / SAMPLE_BYTES;  // 341
const uint32_t FLUSH_EVERY   = 32;   // samples between page writes (~16s)

// record: [0..3] tMs LE | [4..5] rssi*10 LE | [6..7] snr*10 LE | [8] flags | [9..11] reserved
const uint8_t FLAG_PACKET  = 0x01;
const uint8_t FLAG_MARKER  = 0x02;
const uint8_t FLAG_TIMEOUT = 0x04;
const uint8_t FLAG_ERROR   = 0x08;

const esp_partition_t* logPart = nullptr;
uint32_t logCount = 0;                 // total records stored
uint8_t  pageBuf[PAGE_BYTES];
size_t   pageFill = 0;
uint32_t pageIndex = 0;
bool     walkMode = false;

float lastRssi = 0.0f, lastSnr = 0.0f;
bool  haveRssi = false;

void packSample(uint8_t* b, uint32_t t, float rssi, float snr, uint8_t flags, float rawRssi) {
  int16_t r10 = (int16_t)(rssi * 10.0f);
  int16_t s10 = (int16_t)(snr * 10.0f);
  int16_t w10 = (int16_t)(rawRssi * 10.0f);
  b[0] = t & 0xFF;         b[1] = (t >> 8) & 0xFF;
  b[2] = (t >> 16) & 0xFF; b[3] = (t >> 24) & 0xFF;
  b[4] = r10 & 0xFF;       b[5] = (r10 >> 8) & 0xFF;
  b[6] = s10 & 0xFF;       b[7] = (s10 >> 8) & 0xFF;
  b[8] = flags;
  b[9] = w10 & 0xFF;       b[10] = (w10 >> 8) & 0xFF;
  b[11] = 0;
}

void unpackSample(const uint8_t* b, uint32_t& t, float& rssi, float& snr, uint8_t& flags, float& rawRssi) {
  t = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  int16_t r10 = (int16_t)((uint16_t)b[4] | ((uint16_t)b[5] << 8));
  int16_t s10 = (int16_t)((uint16_t)b[6] | ((uint16_t)b[7] << 8));
  int16_t w10 = (int16_t)((uint16_t)b[9] | ((uint16_t)b[10] << 8));
  rssi = r10 / 10.0f;
  snr  = s10 / 10.0f;
  rawRssi = w10 / 10.0f;
  flags = b[8];
}

// Number of samples in a written page, or 0xFFFFFFFF if the page is unwritten.
uint32_t pageCountAt(uint32_t page) {
  uint32_t n = 0xFFFFFFFF;
  esp_partition_read(logPart, (size_t)page * PAGE_BYTES, (uint8_t*)&n, PAGE_COUNT_BYTES);
  return n;
}

bool pageValid(uint32_t page) {
  uint32_t n = pageCountAt(page);
  return (n >= 1 && n <= SAMPLES_PAGE);
}

// Rewrite the current page. Only advance to the next page when it is full -
// advancing on every flush would burn a whole 4KB sector per handful of
// samples and cap the log at minutes rather than hours.
void flushPage(bool advance) {
  if (pageFill == 0) return;
  uint32_t n = (uint32_t)(pageFill / SAMPLE_BYTES);
  size_t off = (size_t)pageIndex * PAGE_BYTES;
  esp_partition_erase_range(logPart, off, PAGE_BYTES);
  esp_partition_write(logPart, off, (uint8_t*)&n, PAGE_COUNT_BYTES);
  esp_partition_write(logPart, off + PAGE_COUNT_BYTES, pageBuf, pageFill);
  if (advance) { pageIndex++; pageFill = 0; }
}

void addSample(uint32_t t, float rssi, float snr, uint8_t flags, float rawRssi) {
  if (pageFill + SAMPLE_BYTES > PAGE_BYTES - PAGE_COUNT_BYTES) {
    flushPage(true);
    pageFill = 0;
  }
  packSample(pageBuf + pageFill, t, rssi, snr, flags, rawRssi);
  pageFill += SAMPLE_BYTES;
  logCount++;
  // Checkpoint periodically so an unplug can't lose more than a few seconds.
  if ((flags & FLAG_MARKER) || (logCount % FLUSH_EVERY == 0)) flushPage(false);
}

void clearLog() {
  pageFill = 0; pageIndex = 0; logCount = 0;
  esp_partition_erase_range(logPart, 0, PAGE_BYTES * 8);
}

void initLog() {
  logPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (logPart == nullptr) { walkMode = false; return; }

  // Walk forward to the first unwritten page; that is where new samples go.
  uint32_t maxPages = logPart->size / PAGE_BYTES;
  logCount = 0;
  pageIndex = 0;
  for (; pageIndex < maxPages; pageIndex++) {
    if (!pageValid(pageIndex)) break;
    logCount += pageCountAt(pageIndex);
  }
  if (pageIndex >= maxPages) { logCount = 0; pageIndex = 0; }

  // If the last page is partially filled, load it so we append to it.
  pageFill = 0;
  if (pageIndex > 0) {
    uint32_t prev = pageCountAt(pageIndex - 1);
    if (prev < SAMPLES_PAGE) {
      pageIndex--;
      pageFill = (size_t)prev * SAMPLE_BYTES;
      esp_partition_read(logPart, (size_t)pageIndex * PAGE_BYTES + PAGE_COUNT_BYTES,
                         pageBuf, pageFill);
    }
  }
}

void dumpLog() {
  Serial.println(F("t_ms,rssi_dbm,snr_db,flags,raw_dbm"));
  uint32_t maxPages = logPart->size / PAGE_BYTES;
  uint32_t total = 0;
  uint8_t buf[SAMPLE_BYTES];
  for (uint32_t p = 0; p < maxPages; p++) {
    uint32_t n = pageCountAt(p);
    if (n == 0xFFFFFFFF || n == 0 || n > SAMPLES_PAGE) break;
    for (uint32_t i = 0; i < n; i++) {
      esp_partition_read(logPart,
                         (size_t)p * PAGE_BYTES + PAGE_COUNT_BYTES + i * SAMPLE_BYTES,
                         buf, SAMPLE_BYTES);
      uint32_t t; float r, s, wr; uint8_t fl;
      unpackSample(buf, t, r, s, fl, wr);
      Serial.print(t); Serial.print(',');
      Serial.print(r, 1); Serial.print(',');
      Serial.print(s, 1); Serial.print(',');
      Serial.print(fl); Serial.print(',');
      Serial.println(wr, 1);
    }
    total += n;
  }
  Serial.print(F("# records: "));
  Serial.println(total);
}

void printBar(uint8_t percent) {
  const uint8_t WIDTH = 20;
  uint8_t filled = (uint8_t)((percent * WIDTH + 50) / 100);
  Serial.print(F("["));
  for (uint8_t i = 0; i < WIDTH; i++) Serial.write(i < filled ? '#' : '.');
  Serial.print(F("] "));
  Serial.print(percent);
  Serial.print(F("%"));
}

void pushRssi(float rssi) {
  rssiHistory[rssiIdx] = rssi;
  rssiIdx = (rssiIdx + 1) % SMOOTH_N;
  if (rssiCount < SMOOTH_N) rssiCount++;
  float sum = 0.0;
  for (uint8_t i = 0; i < rssiCount; i++) sum += rssiHistory[i];
  rssiSmoothed = sum / rssiCount;
  if (rssi > rssiPeak) rssiPeak = rssi;
}

uint8_t rssiToPercent(float rssi) {
  if (rssi <= RSSI_FLOOR) return 0;
  if (rssi >= RSSI_CEIL) return 100;
  return (uint8_t)((rssi - RSSI_FLOOR) * 100.0 / (RSSI_CEIL - RSSI_FLOOR));
}

bool buttonPressed() {
  if (digitalRead(PIN_BTN) != LOW) return false;
  delay(40);
  if (digitalRead(PIN_BTN) != LOW) return false;
  while (digitalRead(PIN_BTN) == LOW) delay(10);
  delay(40);
  return true;
}

bool initRadio() {
  int16_t state = radio.begin(FREQ_MHZ, BANDWIDTH, SPREADING, CODING_R,
                              SYNC_WORD, TX_POWER, PREAMBLE);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print(F("FATAL: SX1278 init failed, code "));
    Serial.println(state);
    return false;
  }
  return true;
}

void printRegCheck() {
  uint32_t frf = ((uint32_t)mod->SPIreadRegister(0x06) << 16) |
                 ((uint32_t)mod->SPIreadRegister(0x07) << 8) |
                  (uint32_t)mod->SPIreadRegister(0x08);
  Serial.print(F("  Freq  "));
  Serial.print((float)frf * 32000000.0f / 524288.0f / 1000000.0f, 3);
  Serial.println(F(" MHz"));
  Serial.print(F("  MC1=0x")); Serial.println(mod->SPIreadRegister(0x1D), HEX);
  Serial.print(F("  MC2=0x")); Serial.println(mod->SPIreadRegister(0x1E), HEX);
  Serial.print(F("  MC3=0x")); Serial.println(mod->SPIreadRegister(0x26), HEX);
  Serial.print(F("  Sync=0x")); Serial.println(mod->SPIreadRegister(0x39), HEX);
  Serial.print(F("  InvIQ=0x")); Serial.println(mod->SPIreadRegister(0x33), HEX);
  Serial.print(F("  IQ2=0x")); Serial.println(mod->SPIreadRegister(0x3B), HEX);
  Serial.print(F("  DetOp=0x")); Serial.println(mod->SPIreadRegister(0x31), HEX);
  Serial.print(F("  DThrs=0x")); Serial.println(mod->SPIreadRegister(0x37), HEX);
}

// Static buffer: allocating a String (and RadioLib's internal buffer) every
// iteration fragments the heap within a few thousand iterations, after which
// receive() starts failing and packets are silently missed.
uint8_t rxBuf[64];

void walkTestLoop() {
  uint32_t nextSample = millis();
  uint8_t pendingFlags = 0;
  // Distinct double-blink so "is it recording?" is obvious at a glance.
  ledPulse(150);
  while (true) {
    ledService();
    int16_t state = radio.receive(rxBuf, sizeof(rxBuf), RX_TIMEOUT_MS);
    if (state == RADIOLIB_ERR_NONE) {
      pendingFlags |= FLAG_PACKET;
      lastRssi = radio.getRSSI();
      lastSnr  = radio.getSNR();
      haveRssi = true;
      pushRssi(lastRssi);
      lastPacketMs = millis();
    } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
      pendingFlags |= FLAG_TIMEOUT;
    } else {
      pendingFlags |= FLAG_ERROR;
    }

    if (millis() < nextSample) continue;
    nextSample = millis() + SAMPLE_INTERVAL_MS;

    // While a host is attached we still need to get out and read the log.
    if (Serial.available()) {
      char c = (char)Serial.read();
      if (c == 'x' || c == 'X') {
        flushPage(false);
        prefs.remove("armed");
        Serial.println(F("Left walk-test mode, disarmed."));
        return;
      } else if (c == 'd' || c == 'D') {
        flushPage(false);
        dumpLog();
      } else if (c == 'c' || c == 'C') {
        flushPage(false);
        clearLog();
        Serial.println(F("Log cleared."));
      }
    }

    uint8_t flags = pendingFlags;
    pendingFlags = 0;
    if (buttonPressed()) {
      flags |= FLAG_MARKER;
      ledPulse(400);
    }

    // Log a point every tick so the CSV has an even time base. RSSI is held
    // between packets - the collar only pings every second or so.
    float r = haveRssi ? rssiSmoothed : -127.0f;
    addSample(millis(), r, haveRssi ? lastSnr : -127.0f, flags,
              haveRssi ? lastRssi : -127.0f);
    ledPulse(80);   // heartbeat: one flash per logged sample
    updateTone(haveRssi, rssiSmoothed);
  }
}

static String incoming;

void consoleLoop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'd' || c == 'D') { dumpLog(); }
    else if (c == 'c' || c == 'C') { clearLog(); Serial.println(F("Log cleared.")); }
else if (c == 't' || c == 'T') {
      toneSetMode((toneMode + 1) % 3);
      Serial.print(F("Buzzer mode: "));
      Serial.println(toneModeName(toneMode));
    }
    else if (c == '?' || c == 'h' || c == 'H') {
      Serial.println(F("d=dump  c=clear  r=regs  w=walk  x=stop  t=tone mode"));
    }
    else if (c == 'w' || c == 'W') {
      // Arm so the mode survives unplugging into the power bank.
      prefs.putBool("armed", true);
      Serial.println(F("Walk mode ARMED and running. Unplug to power bank."));
      Serial.println(F("LED double-blinks on start, then flashes per sample."));
      walkTestLoop();
    }
    else if (c == 'r' || c == 'R') { printRegCheck(); }
  }

  incoming = "";
  int16_t state = radio.receive(incoming, 0, 400);

  if (state == RADIOLIB_ERR_NONE) {
    float rssi = radio.getRSSI();
    float snr  = radio.getSNR();
    pushRssi(rssi);
    lastPacketMs = millis();
    everHeard = true;
    packetCount++;
    updateTone(true, rssiSmoothed);

    Serial.print(F("[+] pkt #"));
    Serial.print(packetCount);
    Serial.print(F("  payload="));
    Serial.println(incoming);

    Serial.print(F("    RSSI raw "));
    Serial.print(rssi, 1);
    Serial.println(F(" dBm | SNR "));
    Serial.print(snr, 1);
    Serial.println(F(" dB | smoothed "));
    Serial.print(rssiSmoothed, 1);
    Serial.print(F(" dBm | peak "));
    Serial.print(rssiPeak, 1);
    Serial.println(F(" dBm"));

    Serial.print(F("    signal "));
    printBar(rssiToPercent(rssiSmoothed));
    Serial.print(F("   tone "));
    if (toneMode == TONE_PASSIVE) {
      Serial.print(toneHz);
      Serial.println(F(" Hz"));
    } else if (toneMode == TONE_ACTIVE) {
      Serial.print(toneDuty);
      Serial.print(F("/4095 duty @ 10Hz"));
    } else {
      Serial.println(F("(muted)"));
    }

    uint8_t pct = rssiToPercent(rssiSmoothed);
    if (pct >= 80)      Serial.println(F("    very close - check under bushes"));
    else if (pct >= 55) Serial.println(F("    getting warmer"));
    else if (pct >= 30) Serial.println(F("    faint signal - keep walking"));
    else                Serial.println(F("    very weak - move closer or reposition antenna"));
    Serial.println();

  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    if (everHeard && (millis() - lastPacketMs) > STALE_MS) {
      Serial.println(F("[-] No collar signal (stale). Last heard above."));
      Serial.println(F("    Walking further away, or collar is asleep."));
      Serial.println();
      everHeard = false;
    }
    if (!everHeard) updateTone(false, 0.0f);
  } else if (state != RADIOLIB_ERR_RX_TIMEOUT) {
    Serial.print(F("[!] Receive error, code "));
    Serial.println(state);
  }
}

void setup() {
  Serial.begin(115200);

  // If a USB host attaches we run interactively; with only a power bank (no
  // host) we fall through into walk-test logging.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2500) delay(10);

  walkArmed = prefs.begin("walk", false);
  if (walkArmed) walkArmed = prefs.getBool("armed", false);

  // Walk mode must be *armed*, not merely "no host seen": many power banks
  // enumerate as a USB host, which made the board sit in console mode and log
  // nothing. Arming persists across the unplug/replug that starts a walk.
  walkMode = walkArmed || !Serial;

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);          // LED is active low
  ledLit = false;
  pinMode(PIN_BTN, INPUT_PULLUP);

  if (!initRadio()) {
    while (true) delay(1000);
  }

  toneInit();
  toneSetMode(TONE_ACTIVE);
  Serial.print(F("Buzzer on GPIO21 - "));
  Serial.println(toneModeName(toneMode));
  Serial.println(F("Press 't' to cycle OFF / PASSIVE / ACTIVE."));
  initLog();

  if (walkMode) {
    if (Serial) {
      Serial.println(F("WALK-TEST MODE (armed). Logging to flash."));
      Serial.println(F("  'x' to exit, 'd' to dump now, 'c' to clear."));
    }
    Serial.print(F("Stored records: "));
    Serial.println(logCount);
    walkTestLoop();
  }

  // A host is present and we are in console mode, so disarm for next time.
  if (walkArmed) prefs.remove("armed");

  Serial.println();
  Serial.println(F("====================================="));
  Serial.println(F(" Cat Tracker - Handheld Homing Device"));
  Serial.println(F(" ESP32-C3 + RA-02 (SX1278 433MHz)"));
  Serial.println(F("====================================="));
  Serial.print(F("Radio online. Freq="));
  Serial.print(FREQ_MHZ, 1);
  Serial.print(F("MHz SF"));
  Serial.print(SPREADING);
  Serial.print(F(" sync=0x"));
  Serial.print(SYNC_WORD, HEX);
  Serial.println();
  Serial.print(F("Stored records: "));
  Serial.println(logCount);
  Serial.println(F("Commands: d=dump  c=clear  r=regs  w=arm walk  t=tone mode"));
  Serial.println(F("Listening for collar pings..."));
  Serial.println();

  consoleLoop();
}

void loop() {
  consoleLoop();
}