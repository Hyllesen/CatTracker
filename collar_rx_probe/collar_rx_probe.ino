/*
 * collar_rx_probe.ino — Phase 0a: can the collar hear, and how fast does the
 * handheld answer?
 *
 * Nothing in this repository has ever *received* on a collar. Both collar
 * builds are transmit-only, and the RA-02 there has no DIO0 wire, so the whole
 * two-speed plan rests on an unproven assumption: that the SX1278 can be put
 * into RX continuous and have its IRQ flags polled over the bit-banged SPI bus
 * fast enough to catch a packet. This sketch tests exactly that and nothing
 * else.
 *
 * The cycle is deliberately the Phase 1 cycle, not a bench-only one:
 *
 *   1. radioInit()   (from sleep, like collar/collar.ino)
 *   2. transmit the normal 6-byte ping (0xC1 ...)
 *   3. arm RX continuous IMMEDIATELY, four register writes, no re-init
 *   4. poll REG_IRQ_FLAGS and REG_MODEM_STAT until RxDone or the window closes
 *
 * So the number it prints — `wait=` — is the real end-to-end latency from the
 * collar's TxDone to the collar's RxDone, and the handheld's share of it (how
 * long it took to key up after decoding) is what decides whether a 100 ms
 * listen window is enough. The command frame's own airtime is known (§3 of
 * docs/plans/find-mode-two-speed-beacon.md: ~239 ms with a 32-symbol preamble),
 * so `wait - airtime` is the turnaround.
 *
 * WHAT IT PRINTS
 *
 * On a good catch:
 *   [RX] wait=252.4ms bytes=6 crc=ok rssi=-98.0 snr=5.75 data=C2 02 01 0F 00 00
 * On a closed window:
 *   [RX] none polls=4829 msfirst=0x0E@3ms mslast=0x04 irq=0x00
 * `msfirst` is the first non-zero REG_MODEM_STAT value seen and when. That is
 * the evidence for the mask question: the plan says the documented bits are
 * 3 = signal detected, 2 = synchronised, 1 = RX on-going (mask 0x0E), and the
 * first draft said 0x0B. If a real preamble produces 0x04 or 0x08 but never
 * 0x02, the mask is wrong and the extension logic must be dropped for a fixed
 * window.
 *
 * WHAT IT DOES NOT DO
 *
 * No find mode, no state machine, no battery, no BLE. Byte 3 of the ping is
 * 0xFF (battery unknown) and byte 5 is 0, so this is a perfectly ordinary ping
 * as far as the handheld and the base station are concerned.
 *
 * CONSOLE (every print is gated on a host being on the cable)
 *
 *   v   toggle logging        V   logging ON (idempotent)   w   logging OFF
 *   p   ping and listen right now
 *   r   dump the registers    l   30 s window for the next ping
 *   s   listen-only 30 s window, no ping, every packet reported - the
 *       instrument that answers "is that transmitter radiating at all".
 *
 * Nothing is printed by default, per the AGENTS rule that a collar must never
 * print unconditionally: with no host on the CDC port a blocked Serial.write
 * stops the radio, and a dead collar is indistinguishable from a flat cell. The
 * single "ready" line in setup() is the exception that proves it — one line,
 * only when TinyUSBDevice.mounted() is already true, so it cannot fill a buffer
 * that nobody is draining.
 *
 * BUILD
 *
 *   arduino-cli compile --fqbn "adafruit:nrf52:feather52840" \
 *     --build-property "compiler.cpp.extra_flags=-DCOLLAR_ID=2" \
 *     --output-dir /tmp/collar_rx_probe collar_rx_probe
 *   arduino-cli upload --fqbn "adafruit:nrf52:feather52840" \
 *     --input-dir /tmp/collar_rx_probe -p /dev/cu.usbmodemXXXX
 *
 * Note `-DCOLLAR_ID`: the handheld replies to the id in the ping, so the probe
 * must not share an id with a collar that is still powered on the bench, or the
 * handheld will answer whichever of the two it hears. Two collars with the same
 * id is the one mistake that is silent.
 */

#include <Arduino.h>
// Required to link the USB CDC implementation behind Serial on the nRF52 core.
// Arduino.h declares Serial when USE_TINYUSB is set, but without this the
// Adafruit_TinyUSB library is never added to the build and the link fails with
// "undefined reference to Adafruit_USBD_CDC". Same reason as collar/collar.ino.
#include <Adafruit_TinyUSB.h>
#include <nrf.h>

// ---------------------------------------------------------------------------
// Build-time knobs
// ---------------------------------------------------------------------------
#ifndef COLLAR_ID
#define COLLAR_ID 2
#endif

// The ping period. 8000 matches production; shorten it (-DPING_MS=2000) when
// watching the terminal, since a bench session with an 8 s cadence is tedious.
#ifndef PING_MS
#define PING_MS 8000
#endif

// How long RX stays armed after a ping. This is NOT LISTEN_MS: the probe wants
// to catch a command even if the handheld is late, and it reports the latency
// rather than the miss. Once the real listen window exists, LISTEN_MS is what
// matters and the number to compare against `wait` here.
#ifndef RX_WINDOW_MS
#define RX_WINDOW_MS 5000
#endif

// The one-shot window that 'l' arms for the next ping. A hand-typed `t` from
// the handheld arrives at an arbitrary moment, so it needs a window wide enough
// to be typed into; `a` (auto-reply) is what makes the timing measurement
// repeatable.
#ifndef LONG_WINDOW_MS
#define LONG_WINDOW_MS 30000
#endif

// RxResult is defined here, above every function, because the Arduino
// preprocessor inserts its own prototypes at the top of the file: a struct
// declared below the first function is not yet visible to a prototype that
// takes a reference to it, and the build fails with "'RxResult' has not been
// declared".
struct RxResult {
  bool     got;
  bool     crcError;
  uint8_t  len;
  uint8_t  data[16];
  uint8_t  irq;
  bool     sawActivity;
  uint8_t  msFirst;
  uint32_t msFirstAtMs;
  uint8_t  msLast;
  uint8_t  rssiMin;     // raw RegRssiValue over the window (0x1B)
  uint8_t  rssiMax;     // - the "is anything radiating at all" instrument
  uint8_t  irqBaseline; // first IRQ_FLAGS reading, usually the leftover TxDone
  uint8_t  irqAny;      // OR of every IRQ_FLAGS value seen in the window
  uint8_t  irqDistinct; // OR of flags that differed from the first reading
  float    latencyMs;
  float    rssi;
  float    snr;
  uint32_t polls;
};

// ---------------------------------------------------------------------------
// Bit-banged SPI + register access - identical to collar/collar.ino
// ---------------------------------------------------------------------------
// Silkscreen numbers are raw port.pin, so "011" is P0.11 and NOT P1.11. With
// SCK on a dead pad the radio is never clocked, every register reads 0x00, and
// the board looks exactly like a flat battery.
struct GPin { NRF_GPIO_Type* port; uint8_t n; };

const GPin P_SCK  = { NRF_P0, 11 };
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

// Serial output costs two bit-banged bytes of nothing - keep it out of the
// poll loop, which is the only place timing matters.
void printHex8(uint8_t v) {
  if (v < 0x10) Serial.print('0');
  Serial.print(v, HEX);
}

// ---- SX1278 registers (LoRa mode) ----
// The FIFO pointer block is worth getting right, because it is where the first
// draft of the plan was wrong: RegFifoAddrPtr is 0x0D (collar.ino calls it
// REG_FIFO_TX_CURR_ADDR, a misnomer, but it writes the same 0 either way),
// RegFifoTxBaseAddr 0x0E, RegFifoRxBaseAddr 0x0F, RegFifoRxCurrentAddr 0x10.
const uint8_t REG_FIFO               = 0x00;
const uint8_t REG_OP_MODE            = 0x01;
const uint8_t REG_FRF_MSB            = 0x06;
const uint8_t REG_FRF_MID            = 0x07;
const uint8_t REG_FRF_LSB            = 0x08;
const uint8_t REG_PA_CONFIG          = 0x09;
const uint8_t REG_OCP                = 0x0B;
const uint8_t REG_LNA                = 0x0C;
const uint8_t REG_FIFO_ADDR_PTR      = 0x0D;
const uint8_t REG_FIFO_TX_BASE_ADDR  = 0x0E;
const uint8_t REG_FIFO_RX_BASE_ADDR  = 0x0F;
const uint8_t REG_FIFO_RX_CURR_ADDR  = 0x10;
const uint8_t REG_IRQ_FLAGS_MASK     = 0x11;
const uint8_t REG_IRQ_FLAGS          = 0x12;
const uint8_t REG_RX_NB_BYTES        = 0x13;
const uint8_t REG_MODEM_STAT         = 0x18;
const uint8_t REG_PKT_SNR            = 0x19;
const uint8_t REG_PKT_RSSI           = 0x1A;
const uint8_t REG_RSSI               = 0x1B;
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

// ---- IRQ flag bits ----
const uint8_t IRQ_RX_TIMEOUT   = 0x80;
const uint8_t IRQ_RX_DONE      = 0x40;
const uint8_t IRQ_CRC_ERROR    = 0x20;
const uint8_t IRQ_VALID_HEADER = 0x10;
const uint8_t IRQ_TX_DONE      = 0x08;

// ---- Modem status bits, the 0x0E-vs-0x0B question ----
// Documented: bit3 signal detected, bit2 signal synchronised, bit1 RX on-going.
// Printed raw so the bench decides, not this comment.
const uint8_t MODEM_STAT_ACTIVITY = 0x0E;

// ---- Air settings - MUST match the handheld ----
const float   FREQ_MHZ   = 433.0;
const float   BANDWIDTH  = 125.0;
const uint8_t SPREADING  = 9;
const uint8_t CODING_R   = 7;
const uint8_t SYNC_WORD  = 0x12;
const int8_t  TX_POWER   = 17;
const uint16_t PREAMBLE  = 8;

const uint8_t BW_125K_RAW = 0x70;
const uint8_t SF9_RAW     = 0x90;
const uint8_t CR_4_7_RAW  = 0x06;

// ---- Frame ----
const uint8_t FRAME_MAGIC = 0xC1;
const uint8_t FRAME_LEN   = 6;

uint8_t seqCounter = 0;

void buildFrame(uint8_t* out) {
  out[0] = FRAME_MAGIC;
  out[1] = seqCounter++;
  out[2] = COLLAR_ID;
  out[3] = 0xFF;   // battery unknown - this sketch does not sample the cell
  out[4] = 0x00;   // flags
  out[5] = 0x00;   // minutes of find mode left
}

// Read-modify-write of bits [startBit..endBit], as RadioLib does. A full-byte
// write zeroes the chip's power-on default bits (CRC aggregation, InvertIQ
// bits 5:3, DetectOptimize bits 7:3) and the handheld then reports
// RADIOLIB_ERR_LORA_HEADER_DAMAGED.
void setRegBits(uint8_t reg, uint8_t value, uint8_t startBit, uint8_t endBit) {
  uint8_t mask = (uint8_t)~((0xFF << (startBit + 1)) | (0xFF >> (8 - endBit)));
  uint8_t cur  = readReg(reg);
  writeReg(reg, (uint8_t)((cur & ~mask) | (value & mask)));
}

void radioInit() {
  writeReg(REG_OP_MODE, 0x80 | 0x00);   // LoRa | sleep
  delay(1);
  writeReg(REG_OP_MODE, 0x80 | 0x01);   // LoRa | standby
  delay(1);

  uint32_t frf = (uint32_t)((FREQ_MHZ * 1000000.0f * 524288.0f) / 32000000.0f);
  writeReg(REG_FRF_MSB, (frf & 0xFF0000) >> 16);
  writeReg(REG_FRF_MID, (frf & 0x00FF00) >> 8);
  writeReg(REG_FRF_LSB, frf & 0x0000FF);
  delay(1);

  writeReg(REG_HOP_PERIOD, 0x00);

  writeReg(REG_PA_CONFIG, 0x80 | 0x70 | (TX_POWER - 2));
  writeReg(REG_PA_DAC, 0x04);
  writeReg(REG_OCP, 0x20 | 3);

  setRegBits(REG_MODEM_CONFIG_1, BW_125K_RAW, 7, 4);
  setRegBits(REG_MODEM_CONFIG_1, CR_4_7_RAW,  3, 1);
  setRegBits(REG_MODEM_CONFIG_1, 0x04,       2, 2);   // payload CRC on
  setRegBits(REG_MODEM_CONFIG_1, 0x00,       0, 0);   // explicit header

  setRegBits(REG_MODEM_CONFIG_2, SF9_RAW | 0x00, 7, 3);
  setRegBits(REG_MODEM_CONFIG_2, 0x04, 2, 2);         // CRC aggregation on

  setRegBits(REG_DETECT_OPTIMIZE, 0x03, 2, 0);
  writeReg(REG_DETECTION_THRESH, 0x0A);

  // IQ inversion "off": RX path normal (bit6=0), TX path ON (bit0=1). This
  // inverted-sounding combination is what RadioLib's invertIQ(false) does and
  // bit 0 is what makes the handheld decode the pings - see RadioLib issue
  // #778. Do not "tidy" this into 0x66.
  setRegBits(REG_INVERT_IQ, 0x00, 6, 6);
  setRegBits(REG_INVERT_IQ, 0x01, 0, 0);
  writeReg(REG_INVERT_IQ2, 0x1D);

  setRegBits(REG_MODEM_CONFIG_3, 0x04, 2, 2);   // AGC loop on

  writeReg(REG_PREAMBLE_MSB, (PREAMBLE >> 8) & 0xFF);
  writeReg(REG_PREAMBLE_LSB, PREAMBLE & 0xFF);
  writeReg(REG_SYNC_WORD, SYNC_WORD);

  writeReg(REG_FIFO_TX_BASE_ADDR, 0x00);
  setRegBits(REG_LNA, 0x03, 1, 0);
}

// Park the radio in sleep between pings: standby with the crystal running is
// 1.6 mA continuous, sleep is 0.2 uA. (Same reasoning as collar/collar.ino.)
void radioSleep() {
  writeReg(REG_OP_MODE, 0x80 | 0x00);
}

// Transmit one packet. Assumes the radio is awake. Returns TxDone time in
// micros(), or 0 on timeout, so the caller can measure the listen latency from
// the instant the carrier actually stops.
uint32_t sendPacket(const uint8_t* payload, uint8_t len) {
  const uint32_t timeoutMs = 3000;
  writeReg(REG_OP_MODE, 0x80 | 0x01);   // standby

  writeReg(REG_FIFO_ADDR_PTR, 0x00);
  writeReg(REG_FIFO_TX_BASE_ADDR, 0x00);

  writeReg(REG_PAYLOAD_LENGTH, len);
  for (uint8_t i = 0; i < len; i++) {
    writeReg(REG_FIFO, payload[i]);
  }

  writeReg(REG_IRQ_FLAGS, 0xFF);

  writeReg(REG_OP_MODE, 0x80 | 0x03);   // TX

  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE) {
      uint32_t doneUs = micros();
      writeReg(REG_IRQ_FLAGS, 0xFF);
      writeReg(REG_OP_MODE, 0x80 | 0x01);   // standby
      return doneUs;
    }
    delay(1);
  }
  writeReg(REG_OP_MODE, 0x80 | 0x01);
  return 0;
}

// ---------------------------------------------------------------------------
// The RX window
// ---------------------------------------------------------------------------
// Four register writes and no re-init: radioInit() costs ~35 writes plus three
// 1 ms delays, and none of that is needed to go from TX to RX. The FIFO
// pointers must be reset because the modem fills the RX FIFO from
// RegFifoRxBaseAddr and a stale pointer would hand back last packet's bytes.
void armRx() {
  writeReg(REG_IRQ_FLAGS, 0xFF);        // clear TxDone and anything stale
  writeReg(REG_FIFO_RX_BASE_ADDR, 0x00);
  writeReg(REG_FIFO_ADDR_PTR, 0x00);
  writeReg(REG_OP_MODE, 0x80 | 0x05);   // LoRa | RX continuous
}

void listenFor(uint32_t windowMs, uint32_t txDoneUs, RxResult& r) {
  r.got = false;
  r.crcError = false;
  r.len = 0;
  r.irq = 0;
  r.sawActivity = false;
  r.msFirst = 0;
  r.msFirstAtMs = 0;
  r.msLast = 0;
  r.irqAny = 0;
  r.irqDistinct = 0;
  r.irqBaseline = 0;
  r.rssiMin = 0xFF;
  r.rssiMax = 0x00;
  r.latencyMs = 0;
  r.rssi = 0;
  r.snr = 0;
  r.polls = 0;

  uint32_t t0 = micros();

  for (;;) {
    uint8_t irq = readReg(REG_IRQ_FLAGS);
    uint8_t ms  = readReg(REG_MODEM_STAT);
    // Instantaneous channel RSSI. It rises for any carrier at all, decoded or
    // not, which is how "this transmitter radiates nothing" is told apart from
    // "this receiver is being overloaded". HF port offset is 157.
    uint8_t rssi = readReg(REG_RSSI);
    r.polls++;
    r.msLast = ms;
    // Two guards, both learned the hard way: the first poll can land while the
    // modem is still settling (raw 0xFF -> a nonsense +96 dBm), and anything
    // above ~200 is not a level this receiver can physically see. An empty
    // channel reads about 37 (-120 dBm) and a collar in the house about 78.
    if (r.polls > 2 && rssi < 200) {
      if (rssi < r.rssiMin) r.rssiMin = rssi;
      if (rssi > r.rssiMax) r.rssiMax = rssi;
    }

    // OR everything, because a single reading can land between two packets.
    // VALID_HEADER (0x10) appearing without RX_DONE is the signature of "a
    // preamble arrived but the header did not decode" - the difference between
    // "nothing on the air" and "something on the air we cannot read".
    if (r.polls == 1) {
      r.irqAny = irq;
      r.irqBaseline = irq;
    }
    r.irqAny |= irq;
    r.irqDistinct |= (uint8_t)(irq & ~r.irqBaseline);

    if (!r.sawActivity && (ms & MODEM_STAT_ACTIVITY)) {
      r.sawActivity = true;
      r.msFirst = ms;
      r.msFirstAtMs = (uint32_t)((micros() - t0) / 1000);
    }

    if (irq & IRQ_RX_DONE) {
      r.got = true;
      r.irq = irq;
      r.crcError = (irq & IRQ_CRC_ERROR) != 0;
      r.latencyMs = (float)(micros() - txDoneUs) / 1000.0f;

      // Read the FIFO while still in RX: the payload is there, and the four
      // clocks the packet arrived in are the ones the modem has just finished
      // using.
      uint8_t n = readReg(REG_RX_NB_BYTES);
      if (n > sizeof(r.data)) n = sizeof(r.data);
      r.len = n;
      writeReg(REG_FIFO_ADDR_PTR, readReg(REG_FIFO_RX_CURR_ADDR));
      for (uint8_t i = 0; i < n; i++) {
        r.data[i] = readReg(REG_FIFO);
      }

      // RSSI and SNR are "live" registers: read mid-reception they return
      // values from part-way through a packet. Stop the radio first, so the
      // numbers describe the packet and not whatever came next.
      writeReg(REG_OP_MODE, 0x80 | 0x01);         // standby
      uint8_t pktRssi = readReg(REG_PKT_RSSI);
      int8_t  pktSnr  = (int8_t)readReg(REG_PKT_SNR);
      // HF port (433 MHz) offset is 157; the LF-port offset of 164 is for
      // 137-175 MHz and would read 7 dB low here.
      r.rssi = (float)pktRssi - 157.0f;
      r.snr  = (float)pktSnr / 4.0f;
      break;
    }

    if ((uint32_t)((micros() - t0) / 1000) >= windowMs) break;
    delay(1);
  }
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
// Off by default, so a collar on a charger with nothing reading the port can
// never wedge inside a print. 'v' turns it on.
static bool verbose = false;

bool hostOn() { return TinyUSBDevice.mounted(); }
bool logOk()  { return verbose && hostOn(); }

bool sx1278Present = false;
uint8_t regVersion = 0;

void printRegCheck() {
  uint32_t frf = ((uint32_t)readReg(REG_FRF_MSB) << 16) |
                 ((uint32_t)readReg(REG_FRF_MID) << 8)  |
                  (uint32_t)readReg(REG_FRF_LSB);
  Serial.print(F("  FRF 0x"));   Serial.println(frf, HEX);
  Serial.print(F("  MC1 0x"));   Serial.println(readReg(REG_MODEM_CONFIG_1), HEX);
  Serial.print(F("  MC2 0x"));   Serial.println(readReg(REG_MODEM_CONFIG_2), HEX);
  Serial.print(F("  Sync 0x"));  Serial.println(readReg(REG_SYNC_WORD), HEX);
  Serial.print(F("  InvIQ 0x")); Serial.println(readReg(REG_INVERT_IQ), HEX);
  Serial.print(F("  InvIQ2 0x"));Serial.println(readReg(REG_INVERT_IQ2), HEX);
  Serial.print(F("  PA 0x"));    Serial.println(readReg(REG_PA_CONFIG), HEX);
  Serial.print(F("  Preamble "));
  Serial.println((uint16_t)(readReg(REG_PREAMBLE_MSB) << 8) | readReg(REG_PREAMBLE_LSB));
}

void printIdleReport() {
  Serial.println();
  Serial.println(F("====================================="));
  Serial.println(F(" Collar RX probe (Phase 0a)"));
  Serial.println(F(" nRF52840 + RA-02 (SX1278 433MHz)"));
  Serial.println(F("====================================="));
  Serial.print(F("RegVersion = 0x"));
  Serial.println(regVersion, HEX);
  if (!sx1278Present) {
    Serial.println(F("SX1278 NOT detected - check wiring/power"));
    return;
  }
  Serial.print(F("id "));
  Serial.print(COLLAR_ID);
  Serial.print(F("  ping every "));
  Serial.print(PING_MS);
  Serial.print(F(" ms  RX window "));
  Serial.print(RX_WINDOW_MS);
  Serial.println(F(" ms"));
  Serial.print(F("Modem status mask in use: 0x"));
  Serial.println(MODEM_STAT_ACTIVITY, HEX);
  Serial.println(F("'v' silences this. 'p' pings now. 'r' dumps registers."));
}

uint32_t lastPingMs = 0;
uint32_t pingCount = 0;
// One-shot override of the RX window, armed by 'l' and cleared after one use.
uint32_t nextWindowMs = 0;

// 'S' arms a window WITHOUT transmitting first, which is the only way to tell
// "the handheld is not radiating" from "this collar is deaf for a few hundred
// milliseconds after its own transmission". The two look identical in a
// post-ping window, and they have completely different consequences.
uint32_t nextListenOnlyMs = 0;

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);

  outMode(P_SCK); outMode(P_MOSI); outMode(P_CS); outMode(P_RST);
  inMode(P_MISO);
  wr(P_SCK, 0); wr(P_CS, 1);

  wr(P_RST, 0); delay(10);
  wr(P_RST, 1); delay(20);

  uint8_t v = readReg(0x42);
  sx1278Present = (v == 0x12);
  regVersion = v;

  if (sx1278Present) radioInit();

  // One line, only when a host is already on the cable. This is the only print
  // outside logOk(), and it cannot fill a buffer that nobody drains because it
  // is emitted only when somebody is there to drain it.
  if (hostOn()) {
    if (sx1278Present) {
      Serial.println(F("collar_rx_probe ready - 'v' to start logging."));
    } else {
      Serial.println(F("collar_rx_probe: SX1278 NOT detected."));
    }
  }
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (!hostOn()) continue;
    if (c == 'V') {                    // idempotent ON, so a bench script does
      verbose = true;                  // not have to know the current state
      printIdleReport();
    } else if (c == 'w' || c == 'W') {
      verbose = false;
    } else if (c == 'v') {
      verbose = !verbose;
      if (verbose) printIdleReport();
    } else if (c == 'p' || c == 'P') {
      lastPingMs = millis() - PING_MS;   // ping on the next pass through loop()
    } else if (c == 'r' || c == 'R') {
      if (logOk() && sx1278Present) {
        radioInit();          // the SX1278 cannot be read over SPI in sleep
        printRegCheck();
        radioSleep();
      }
    } else if (c == 's' || c == 'S') {
      nextListenOnlyMs = LONG_WINDOW_MS;
      if (logOk()) {
        Serial.print(F("[win] listen-only "));
        Serial.print(nextListenOnlyMs);
        Serial.println(F(" ms, no ping"));
      }
    } else if (c == 'l' || c == 'L') {
      nextWindowMs = LONG_WINDOW_MS;
      if (logOk()) {
        Serial.print(F("[win] next window "));
        Serial.print(nextWindowMs);
        Serial.println(F(" ms"));
      }
    }
  }

  if (nextListenOnlyMs) {
    // A listen-only window must keep listening after the first packet. A single
    // collar in the house pings every 8 s, so a window that returns on the
    // first catch spends most of its life closed - which is exactly how an
    // earlier version of this test produced a false "the handheld is not
    // radiating" result.
    uint32_t windowMs = nextListenOnlyMs;
    nextListenOnlyMs = 0;
    uint32_t t0 = millis();
    uint8_t  peak = 0;
    uint16_t caught = 0;

    radioInit();
    if (logOk()) {
      Serial.print(F("[listen] "));
      Serial.print(windowMs);
      Serial.println(F("ms, no ping - every packet is reported"));
    }

    while (millis() - t0 < windowMs) {
      uint32_t remaining = windowMs - (millis() - t0);
      RxResult r;
      armRx();
      listenFor(remaining, micros(), r);
      if (r.rssiMax > peak) peak = r.rssiMax;
      if (!r.got) break;
      caught++;
      if (logOk()) {
        Serial.print(F("[RX] "));
        Serial.print(r.len);
        Serial.print(r.crcError ? F(" crc=BAD") : F(" crc=ok"));
        Serial.print(F(" rssi="));
        Serial.print(r.rssi, 1);
        Serial.print(F(" snr="));
        Serial.print(r.snr, 2);
        Serial.print(F(" data="));
        for (uint8_t i = 0; i < r.len; i++) {
          printHex8(r.data[i]);
          if (i + 1 < r.len) Serial.print(' ');
        }
        Serial.println();
      }
    }

    if (logOk()) {
      Serial.print(F("[listen] done caught="));
      Serial.print(caught);
      Serial.print(F(" peak="));
      Serial.print((int)peak - 157);
      Serial.println(F("dBm (HF offset 157; -120 dBm is an empty channel)"));
    }
    radioSleep();
    lastPingMs = millis();
    return;
  }

  if (millis() - lastPingMs < PING_MS) {
    // Same trick as collar/collar.ino: yield so the idle task can suppress
    // ticks, which is what takes the core out of System ON running.
    uint32_t remaining = PING_MS - (millis() - lastPingMs);
    delay(remaining < 250 ? remaining : 250);
    return;
  }

  if (!sx1278Present) {
    lastPingMs = millis();
    return;
  }

  // ---- one full Phase 1 cycle ----
  radioInit();

  uint8_t frame[FRAME_LEN];
  buildFrame(frame);
  uint32_t txDoneUs = sendPacket(frame, FRAME_LEN);
  pingCount++;

  RxResult r;
  uint32_t windowMs = nextWindowMs ? nextWindowMs : RX_WINDOW_MS;
  nextWindowMs = 0;

  if (txDoneUs == 0) {
    if (logOk()) {
      Serial.print(F("[TX] #"));
      Serial.print(pingCount);
      Serial.println(F(" TIMEOUT - radio not transmitting"));
    }
    armRx();
    listenFor(windowMs, micros(), r);
  } else {
    if (logOk()) {
      Serial.print(F("[TX] #"));
      Serial.print(pingCount);
      Serial.print(F(" seq="));
      Serial.print(seqCounter - 1);
      Serial.print(F(" id="));
      Serial.print(COLLAR_ID);
      Serial.print(F(" window="));
      Serial.print(windowMs);
      Serial.println(F("ms"));
    }
    armRx();
    listenFor(windowMs, txDoneUs, r);
  }

  if (logOk()) {
    if (r.got) {
      Serial.print(F("[RX] wait="));
      Serial.print(r.latencyMs, 1);
      Serial.print(F("ms bytes="));
      Serial.print(r.len);
      Serial.print(r.crcError ? F(" crc=BAD") : F(" crc=ok"));
      Serial.print(F(" rssi="));
      Serial.print(r.rssi, 1);
      Serial.print(F(" snr="));
      Serial.print(r.snr, 2);
      Serial.print(F(" data="));
      for (uint8_t i = 0; i < r.len; i++) {
        printHex8(r.data[i]);
        if (i + 1 < r.len) Serial.print(' ');
      }
      Serial.print(F(" irq=0x"));
      printHex8(r.irq);
      Serial.println();
      if (r.sawActivity) {
        Serial.print(F("     msfirst=0x"));
        printHex8(r.msFirst);
        Serial.print(F("@"));
        Serial.print(r.msFirstAtMs);
        Serial.println(F("ms"));
      }
    } else {
      Serial.print(F("[RX] none polls="));
      Serial.print(r.polls);
      Serial.print(F(" msfirst="));
      if (r.sawActivity) {
        Serial.print(F("0x"));
        printHex8(r.msFirst);
        Serial.print(F("@"));
        Serial.print(r.msFirstAtMs);
        Serial.print(F("ms"));
      } else {
        Serial.print(F("-"));
      }
      Serial.print(F(" mslast=0x"));
      printHex8(r.msLast);
      Serial.print(F(" irqany=0x"));
      printHex8(r.irqAny);
      Serial.print(F(" rssiwin="));
      Serial.print((int)r.rssiMin - 157);
      Serial.print(F(".."));
      Serial.print((int)r.rssiMax - 157);
      Serial.print(F("dBm"));
      Serial.println();
    }
  }

  radioSleep();
  lastPingMs = millis();
}
