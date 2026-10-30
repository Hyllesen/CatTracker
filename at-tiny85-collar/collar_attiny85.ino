// ---------------------------------------------------------------------------
// Cat Tracker collar - ATtiny85 + Ra-02 (SX1278), LoRa 433 MHz
//
// Port of collar/collar.ino (nRF52840) to an ATtiny85. Same air settings, same
// 6-byte frame, so the existing handheld, phone app and monitor.py work
// unchanged. Register values are copied from the nRF52840 build.
//
// Build (ATTinyCore, ATtiny85, 1 MHz internal, BOD disabled, millis disabled):
//
//   arduino-cli compile --fqbn "ATTinyCore:avr:attinyx5:chip=85,clock=1internal"
//       --build-property "compiler.cpp.extra_flags=-DCOLLAR_ID=1 -DPING_TICKS=1"
//       --output-dir /tmp/collar85 collar_attiny85
//   arduino-cli upload --programmer usbasp
//       --fqbn "ATTinyCore:avr:attinyx5:chip=85,clock=1internal" --input-dir /tmp/collar85
//   (each command is one line; they are wrapped here for readability)
//
// (Run `arduino-cli board details -b ATTinyCore:avr:attinyx5` to confirm the
// option names; --input-dir pins the exact build so the id cannot get mixed up.)
//
// Wiring (ATtiny85 pin -> Ra-02):
//   pin 5  PB0  -> MOSI
//   pin 6  PB1  -> MISO
//   pin 7  PB2  -> SCK
//   pin 2  PB3  -> NSS (chip select)
//   pin 3  PB4  <- battery divider midpoint (1M to VBAT, 330k to GND, 100nF to GND)
//   pin 1  PB5  -> RESET (keep as reset; 10k pull-up to VCC recommended)
//   Ra-02 RESET -> VCC through 10k (no MCU pin for it). DIO0 not connected.
//   VCC = 3.0 V from an MCP1700-3002 LDO, 100 uF low-ESR cap next to the Ra-02.
//
// The SPI pins are also the ISP programming pins (MOSI/MISO/SCK). That is fine
// because NSS idles high, so the Ra-02 ignores the bus while you flash.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include <util/delay.h>

// ---------------------------------------------------------------------------
// Build settings
// ---------------------------------------------------------------------------
// Each collar needs its own id, set at flash time with -DCOLLAR_ID=n.
#ifndef COLLAR_ID
#define COLLAR_ID 0
#endif
#ifndef MAX_COLLARS
#define MAX_COLLARS 3
#endif
#if COLLAR_ID < 0 || COLLAR_ID >= MAX_COLLARS
#error "COLLAR_ID must be between 0 and MAX_COLLARS-1"
#endif

// Ping interval in units of one watchdog sleep (nominally 8 s each).
//   1 = ~8 s   (range testing / homing; about 4 days on 200 mAh)
//   8 = ~64 s  (about 5 weeks)
// The watchdog oscillator is not accurate: 8 s can really be 7-10 s depending
// on temperature and voltage. MEASURE the real period on the bench, and set
// the handheld's COLLAR_PING_MS / STALE_MS from the slowest measured value.
#ifndef PING_TICKS
#define PING_TICKS 1
#endif

// Transmit power in dBm, 2..17 (PA_BOOST path). Lower = longer battery, less range.
#ifndef TX_POWER
#define TX_POWER 17
#endif

// Compile with -DDEV_MODE to read the battery on every ping.
#ifdef DEV_MODE
#define BATT_EVERY_N_PINGS 1
#else
// About every 10 minutes whatever the ping interval is.
#define BATT_EVERY_N_PINGS ((PING_TICKS * 8 >= 600) ? 1 : (600 / (PING_TICKS * 8)))
#endif

// ---------------------------------------------------------------------------
// Pins (PORTB bit numbers)
// ---------------------------------------------------------------------------
#define P_MOSI PB0
#define P_MISO PB1
#define P_SCK  PB2
#define P_NSS  PB3
// Battery sense is on PB4 = ADC2.

// ---------------------------------------------------------------------------
// Bit-banged SPI, mode 0, MSB first
// ---------------------------------------------------------------------------
static uint8_t spiByte(uint8_t out) {
  uint8_t in = 0;
  for (uint8_t i = 0; i < 8; i++) {
    if (out & 0x80) PORTB |= _BV(P_MOSI); else PORTB &= ~_BV(P_MOSI);
    out <<= 1;
    PORTB |= _BV(P_SCK);
    in <<= 1;
    if (PINB & _BV(P_MISO)) in |= 1;
    PORTB &= ~_BV(P_SCK);
  }
  return in;
}

static uint8_t readReg(uint8_t addr) {
  PORTB &= ~_BV(P_NSS);
  spiByte(addr & 0x7F);                 // MSB 0 = read
  uint8_t v = spiByte(0x00);
  PORTB |= _BV(P_NSS);
  return v;
}

static void writeReg(uint8_t addr, uint8_t value) {
  PORTB &= ~_BV(P_NSS);
  spiByte(addr | 0x80);                 // MSB 1 = write
  spiByte(value);
  PORTB |= _BV(P_NSS);
}

// Read-modify-write of bits [startBit..endBit]; same semantics as RadioLib's
// SPIsetRegValue. `value` is already positioned at its bit offset. This keeps
// the chip's power-on default bits, which a full-byte write would zero.
static void setRegBits(uint8_t reg, uint8_t value, uint8_t startBit, uint8_t endBit) {
  uint8_t mask = (uint8_t)~((0xFF << (startBit + 1)) | (0xFF >> (8 - endBit)));
  uint8_t cur  = readReg(reg);
  writeReg(reg, (uint8_t)((cur & ~mask) | (value & mask)));
}

// ---------------------------------------------------------------------------
// SX1278 registers
// ---------------------------------------------------------------------------
#define REG_FIFO              0x00
#define REG_OP_MODE           0x01
#define REG_FRF_MSB           0x06
#define REG_FRF_MID           0x07
#define REG_FRF_LSB           0x08
#define REG_PA_CONFIG         0x09
#define REG_OCP               0x0B
#define REG_LNA               0x0C
#define REG_FIFO_TX_CURR_ADDR 0x0D
#define REG_FIFO_TX_BASE_ADDR 0x0E
#define REG_IRQ_FLAGS         0x12
#define REG_MODEM_CONFIG_1    0x1D
#define REG_MODEM_CONFIG_2    0x1E
#define REG_PREAMBLE_MSB      0x20
#define REG_PREAMBLE_LSB      0x21
#define REG_PAYLOAD_LENGTH    0x22
#define REG_HOP_PERIOD        0x24
#define REG_MODEM_CONFIG_3    0x26
#define REG_DETECT_OPTIMIZE   0x31
#define REG_INVERT_IQ         0x33
#define REG_DETECTION_THRESH  0x37
#define REG_SYNC_WORD         0x39
#define REG_INVERT_IQ2        0x3B
#define REG_VERSION           0x42
#define REG_PA_DAC            0x4D

// ---- Air settings - MUST match the handheld (433 MHz, SF9, BW125, CR 4/7) ----
// FRF = freq_hz * 2^19 / 32e6 = freq_MHz * 16384, so 433 MHz is exact in integers.
static const uint32_t FRF        = 433UL * 16384UL;   // 0x6C4000
static const uint8_t  SYNC_WORD  = 0x12;
static const uint16_t PREAMBLE   = 8;
static const uint8_t  BW_125K_RAW = 0x70;   // ModemConfig1 bits 7:4
static const uint8_t  SF9_RAW     = 0x90;   // ModemConfig2 bits 7:4
static const uint8_t  CR_4_7_RAW  = 0x06;   // ModemConfig1 bits 3:1

static bool radioPresent = false;

static void radioInit() {
  writeReg(REG_OP_MODE, 0x80 | 0x00);   // LoRa | sleep
  _delay_ms(1);
  writeReg(REG_OP_MODE, 0x80 | 0x01);   // LoRa | standby
  _delay_ms(1);

  writeReg(REG_FRF_MSB, (FRF >> 16) & 0xFF);
  writeReg(REG_FRF_MID, (FRF >> 8) & 0xFF);
  writeReg(REG_FRF_LSB, FRF & 0xFF);
  _delay_ms(1);

  writeReg(REG_HOP_PERIOD, 0x00);                          // no frequency hopping
  writeReg(REG_PA_CONFIG, 0x80 | 0x70 | (TX_POWER - 2));   // PA_BOOST
  writeReg(REG_PA_DAC, 0x04);                              // boost off
  writeReg(REG_OCP, 0x20 | 3);                             // OCP on, 60 mA

  setRegBits(REG_MODEM_CONFIG_1, BW_125K_RAW, 7, 4);
  setRegBits(REG_MODEM_CONFIG_1, CR_4_7_RAW,  3, 1);
  setRegBits(REG_MODEM_CONFIG_1, 0x04,        2, 2);       // payload CRC on
  setRegBits(REG_MODEM_CONFIG_1, 0x00,        0, 0);       // explicit header

  setRegBits(REG_MODEM_CONFIG_2, SF9_RAW, 7, 3);
  setRegBits(REG_MODEM_CONFIG_2, 0x04,    2, 2);           // CRC aggregation on

  setRegBits(REG_DETECT_OPTIMIZE, 0x03, 2, 0);
  writeReg(REG_DETECTION_THRESH, 0x0A);

  // Same IQ settings as RadioLib's invertIQ(false). Getting bit 0 wrong makes
  // TX "succeed" while the receiver never decodes it.
  setRegBits(REG_INVERT_IQ, 0x00, 6, 6);
  setRegBits(REG_INVERT_IQ, 0x01, 0, 0);
  writeReg(REG_INVERT_IQ2, 0x1D);

  setRegBits(REG_MODEM_CONFIG_3, 0x04, 2, 2);              // AGC on

  writeReg(REG_PREAMBLE_MSB, (PREAMBLE >> 8) & 0xFF);
  writeReg(REG_PREAMBLE_LSB, PREAMBLE & 0xFF);
  writeReg(REG_SYNC_WORD, SYNC_WORD);

  writeReg(REG_FIFO_TX_BASE_ADDR, 0x00);
  setRegBits(REG_LNA, 0x03, 1, 0);                         // LNA boost
}

// Sleep draws ~0.2 uA; standby with the crystal running is 1.6 mA.
static void radioSleep() {
  writeReg(REG_OP_MODE, 0x80 | 0x00);
}

// Transmit one packet; true if TxDone was seen. Polls the IRQ flags, so DIO0
// does not need a pin. The frame is ~140 ms on air; the limit below is ~600 ms.
static bool sendPacket(const uint8_t* payload, uint8_t len) {
  writeReg(REG_OP_MODE, 0x80 | 0x01);          // standby
  writeReg(REG_FIFO_TX_CURR_ADDR, 0x00);
  writeReg(REG_FIFO_TX_BASE_ADDR, 0x00);
  writeReg(REG_PAYLOAD_LENGTH, len);
  for (uint8_t i = 0; i < len; i++) writeReg(REG_FIFO, payload[i]);
  writeReg(REG_IRQ_FLAGS, 0xFF);
  writeReg(REG_OP_MODE, 0x80 | 0x03);          // transmit

  for (uint16_t i = 0; i < 300; i++) {
    if (readReg(REG_IRQ_FLAGS) & 0x08) {       // TxDone
      writeReg(REG_IRQ_FLAGS, 0xFF);
      writeReg(REG_OP_MODE, 0x80 | 0x01);
      return true;
    }
    _delay_ms(1);
  }
  writeReg(REG_OP_MODE, 0x80 | 0x01);
  return false;
}

// ---------------------------------------------------------------------------
// Battery sense: 1M (top) + 330k (bottom) divider into ADC2 against the
// internal ~1.1 V reference. 4.2 V -> 1.04 V at the pin, so it fits.
// The divider and its 100 nF cap are permanently connected, so the node is
// already settled when the ADC samples it; no wait is needed.
//
// CALIBRATE: the 1.1 V reference is only +-10%. Measure the real cell voltage
// with a multimeter, compare with the value reported, and adjust BATT_VREF_MV
// (new = old * measured / reported) before relying on the percentage.
// ---------------------------------------------------------------------------
#ifndef BATT_VREF_MV
#define BATT_VREF_MV 1100UL
#endif
#define BATT_R_TOP_K    1000UL
#define BATT_R_BOT_K    330UL

static uint16_t readBatteryMv() {
  power_adc_enable();
  ADMUX  = _BV(REFS1) | 0x02;                       // 1.1 V reference, ADC2 (PB4)
  ADCSRA = _BV(ADEN) | _BV(ADPS1) | _BV(ADPS0);     // clk/8 = 125 kHz at 1 MHz
  _delay_ms(2);                                     // let the reference settle

  uint32_t sum = 0;
  for (uint8_t i = 0; i < 9; i++) {
    ADCSRA |= _BV(ADSC);
    while (ADCSRA & _BV(ADSC)) { }
    if (i > 0) sum += ADC;                          // first conversion is discarded
  }
  ADCSRA = 0;
  power_adc_disable();

  uint32_t raw = sum / 8;
  // mV at the cell = raw/1024 * Vref * (Rtop+Rbot)/Rbot; fits in 32 bits.
  return (uint16_t)((raw * BATT_VREF_MV * (BATT_R_TOP_K + BATT_R_BOT_K)) /
                    (1024UL * BATT_R_BOT_K));
}

// Same single-cell LiPo curve as collar.ino.
static const uint16_t CURVE_MV[]  = { 4200, 4000, 3800, 3700, 3600, 3400 };
static const uint8_t  CURVE_PCT[] = {  100,   85,   55,   25,   10,    0 };
#define CURVE_N 6

static uint8_t percentFromMv(uint16_t mv) {
  if (mv >= CURVE_MV[0]) return 100;
  for (uint8_t i = 1; i < CURVE_N; i++) {
    if (mv >= CURVE_MV[i]) {
      uint16_t span = CURVE_MV[i - 1] - CURVE_MV[i];
      uint16_t into = CURVE_MV[i - 1] - mv;
      uint8_t  drop = CURVE_PCT[i - 1] - CURVE_PCT[i];
      return CURVE_PCT[i - 1] - (uint8_t)(((uint32_t)into * drop + span / 2) / span);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Packet format - MUST match the handheld (identical to collar.ino)
//   byte 0  0xC1  magic / protocol version
//   byte 1  seq   wrapping 0..255
//   byte 2  id    0..MAX_COLLARS-1
//   byte 3  battery 0..100, 0xFF = unknown
//   byte 4  flags bit0 = LOW_BATT (<15%)
//   byte 5  reserved
// ---------------------------------------------------------------------------
#define FRAME_MAGIC   0xC1
#define FRAME_LEN     6
#define FLAG_LOW_BATT 0x01
#define BATT_UNKNOWN  0xFF

static uint8_t seqCounter   = 0;
static uint8_t batteryPct   = 0;
static bool    batteryValid = false;
static uint8_t battCountdown = 0;

static void buildFrame(uint8_t* out) {
  out[0] = FRAME_MAGIC;
  out[1] = seqCounter++;
  out[2] = COLLAR_ID;
  out[3] = batteryValid ? batteryPct : BATT_UNKNOWN;
  out[4] = (batteryValid && batteryPct < 15) ? FLAG_LOW_BATT : 0x00;
  out[5] = 0x00;
}

// ---------------------------------------------------------------------------
// Sleep: watchdog interrupt wakes the chip from power-down (~5 uA).
// ---------------------------------------------------------------------------
ISR(WDT_vect) { }   // must exist, or the wake-up interrupt resets the chip

static void sleepFor(uint8_t wdto) {        // WDTO_1S or WDTO_8S
  uint8_t bits = ((wdto & 0x08) ? _BV(WDP3) : 0) | (wdto & 0x07);
  cli();
  wdt_reset();
  MCUSR = 0;
  WDTCR |= _BV(WDCE) | _BV(WDE);            // timed sequence
  WDTCR = _BV(WDIE) | bits;                 // interrupt only, no reset
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  sei();
  sleep_cpu();
  sleep_disable();
  wdt_disable();                            // otherwise it keeps interrupting
}

// ---------------------------------------------------------------------------
void setup() {
  ACSR |= _BV(ACD);                         // analog comparator off
  DIDR0 = _BV(ADC2D);                       // no digital input buffer on the battery pin
  power_timer0_disable();                   // millis() is not used
  power_timer1_disable();
  power_usi_disable();
  power_adc_disable();

  DDRB  = _BV(P_MOSI) | _BV(P_SCK) | _BV(P_NSS);   // MISO and PB4 stay inputs
  PORTB = _BV(P_NSS) | _BV(P_MISO);                // NSS high; pull-up keeps MISO from floating

  _delay_ms(20);                            // Ra-02 power-on reset
  radioPresent = (readReg(REG_VERSION) == 0x12);
  if (radioPresent) radioSleep();
}

void loop() {
  if (radioPresent) {
    if (battCountdown == 0) {
      uint16_t mv = readBatteryMv();        // read before TX so the cell is not sagging
      batteryValid = (mv >= 2000);
      batteryPct = batteryValid ? percentFromMv(mv) : 0;
      battCountdown = BATT_EVERY_N_PINGS;
    }
    battCountdown--;

    radioInit();                            // sleep does not retain FIFO/modem state
    uint8_t frame[FRAME_LEN];
    buildFrame(frame);
    sendPacket(frame, FRAME_LEN);
    radioSleep();
  } else {
    // No radio answered at boot: re-check each cycle (wiring fixed, radio woke late).
    radioPresent = (readReg(REG_VERSION) == 0x12);
  }

  // Idle pins low-power during the sleep.
  PORTB &= ~(_BV(P_MOSI) | _BV(P_SCK));

  for (uint8_t i = 0; i < PING_TICKS; i++) sleepFor(WDTO_8S);
  // Each collar adds one extra second per id so two collars cannot stay locked
  // together; the watchdog's own drift pulls them apart further.
  for (uint8_t i = 0; i < COLLAR_ID; i++) sleepFor(WDTO_1S);
}
