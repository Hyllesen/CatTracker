/*
 * poc915.ino — prove the 915 MHz LoRa P2P hop between two ESP32-C3 + RAK3172 boards.
 * RX parse lives in rak_event.h, host-tested by test_rak_event.cpp.
 *
 * One sketch, three roles, chosen at compile time:
 *   -DROLE=0   LISTENER  (the gateway side): continuous RX, prints every frame + RSSI/SNR
 *   -DROLE=1   TALKER    (the node side):    one report every 8 s
 *   -DROLE=2   NODE      (RA-02 433 RX + RAK 915 TX): hears collars, reports over 915
 *
 * Console is USB CDC (build with CDCOnBoot=cdc). Every print is gated on
 * Serial.isPlugged() — see AGENTS.md: a board with no cable has no Host, prints
 * nothing, and cannot wedge on a blocked write.
 *
 * Console keys: h help, b bridge (type AT commands straight to the module),
 * p readback, r re-arm RX, s send one now, x stop RX.
 */

#ifndef ROLE
#define ROLE 0
#endif
#define ROLE_LISTEN 0
#define ROLE_TALK   1
#define ROLE_NODE   2

#ifndef NODE_ID
#define NODE_ID 1
#endif

// RAK3172 UART2 <-> ESP32-C3. Cross: C3 RX reads the module's TX2 pad.
#define PIN_RAK_RX 20
#define PIN_RAK_TX 21

// 915 link settings. These MUST be identical on both boards — one field off
// is silence with no error, indistinguishable from a dead module.
#ifndef FREQ_HZ
#define FREQ_HZ    915000000UL
#endif
#ifndef SF
#define SF         7
#endif
#ifndef BW
#define BW         125      // 125 kHz
#endif
#ifndef CR
#define CR         0        // 0 = 4/5
#endif
#ifndef PREAMBLE
#define PREAMBLE   8
#endif
#ifndef TX_DBM
#define TX_DBM     14
#endif
#define TALK_MS    8000
#define HEARTBEAT_MS 60000

#include "rak_event.h"

static char p2pSet[64];      // "AT+P2P=915000000:7:125:0:8:14"
static char p2pWant[40];     // the readback we expect, "915000000:7:125:0:8:14"

static uint32_t seq = 0;
static uint32_t txCount = 0, txFail = 0;
static uint32_t rxCount = 0, rxMiss = 0;
static int lastRssi = 0, lastSnr = 0;
static long lastSeq = -1;
static int lastNode = -1;
static uint32_t lastRxMs = 0;
static bool warnedSilent = false;
static String resp;          // last query reply from the module

// RAM log of every frame. The board has no console on the walk — prints are gated
// on Serial.isPlugged() — so the data is kept in RAM and dumped with 'd' when the
// board is plugged back in. RAM survives as long as the power bank keeps the board
// powered; a power cut loses it.
#define LOG_MAX 200
static uint32_t logMs[LOG_MAX];
static long logSeq[LOG_MAX];
static int logRssi[LOG_MAX];
static int logSnr[LOG_MAX];
static int logN = 0;

#if ROLE == ROLE_NODE
#include "collar_frame.h"

// Bit-banged SPI on the pads as wired. The C3's hardware SPI was tried on these
// same pads and read 0x00 from every register, including RegVersion, while the
// bit-bang read the correct values on the same wires — so the SPI1 path is not
// usable here and the register checks have to go through bit-bang.
#define PIN_SCK   4
#define PIN_MISO 5
#define PIN_MOSI 6
#define PIN_NSS   7
#define PIN_RST   1
#define PIN_DIO0  0

#define F433_MHZ     433.0f
#define SF433        9
#define BW433_CODE   0x70   // 125 kHz, ModemConfig1 bits 7:4
#define CR47_CODE    0x06   // 4/7, bits 3:1
#define SYNC433      0x12
#define PREAMBLE433  8

const uint8_t R_FIFO = 0x00, R_OP_MODE = 0x01, R_FRF_MSB = 0x06, R_FRF_MID = 0x07,
            R_FRF_LSB = 0x08, R_PA_CONFIG = 0x09, R_OCP = 0x0B, R_LNA = 0x0C,
            R_FIFO_ADDR_PTR = 0x0D, R_FIFO_TX_BASE = 0x0E, R_FIFO_RX_BASE = 0x0F,
            R_FIFO_RX_CURRENT = 0x10, R_IRQ_FLAGS = 0x12, R_RX_NB_BYTES = 0x13,
            R_PKT_RSSI = 0x1A, R_PKT_SNR = 0x19, R_MODEM1 = 0x1D, R_MODEM2 = 0x1E,
            R_PREAMBLE_MSB = 0x20, R_PREAMBLE_LSB = 0x21, R_PAYLOAD_LEN = 0x22,
            R_MODEM3 = 0x26, R_DETECT_OPT = 0x31, R_THRESH = 0x37, R_INVERT_IQ = 0x33,
            R_INVERT_IQ2 = 0x3B, R_SYNC = 0x39, R_VERSION = 0x42, R_PA_DAC = 0x4D;

// RegOpMode mode bits. These were swapped in the first draft, and the error is
// completely silent: 0x83 is TRANSMIT, not receive, so the radio sat in TX and
// every "detection" the firmware reported was its own TxDone flag.
const uint8_t M_SLEEP = 0x80, M_STDBY = 0x81, M_TX = 0x83, M_RXCONT = 0x85;

// RegIrqFlags bits (LoRa). Also got swapped: RxDone is bit 6, and bit 3 is TxDone.
// Treating ValidHeader (bit 4) as a timeout cleared it and re-armed RX, aborting
// every real packet the instant its header validated.
const uint8_t F_RX_TIMEOUT = 0x80, F_RX_DONE = 0x40, F_CRC_ERROR = 0x20,
              F_VALID_HEADER = 0x10, F_TX_DONE = 0x08;
const uint8_t F_CLEAR_ALL = 0xFF;   // write-1-to-clear, as collar/collar.ino does

// Packet RSSI: offset is -164 below 868 MHz, and a negative SNR is added because a
// spread-spectrum signal is readable below the noise floor. -156 is the HF-port
// constant and reads ~8 dB wrong on 433.
int rssiFrom(uint8_t raw, int snrQuarter) {
  int rssi = -164 + (int8_t)raw;
  int snr = snrQuarter / 4;
  if (snr < 0) rssi += snr;
  return rssi;
}

static ct::Roster rosterNode;
static int heardId = -1, heardSeq = -1, lastRssi433 = 0;
static uint32_t heardMs = 0;
static uint8_t lastPkt[ct::FRAME_LEN];
static int lastPktRssi = 0, lastPktSnr = 0;
static int pktN = 0, rxDrop = 0, aligned = 0, misaligned = 0, crcErr = 0;
static int badRead = 0, dupDropped = 0;

// Same collar id and same seq inside 2 s = the same packet seen twice, not a new one.
// A collar pings every ~8 s, so this cannot hide a real repeat.
bool id_seen_already(const uint8_t *buf, int *lid, int *lseq, uint32_t *lms) {
  if (buf[2] == *lid && buf[1] == *lseq && millis() - *lms < 2000) return true;
  *lid = buf[2]; *lseq = buf[1]; *lms = millis();
  return false;
}
static uint8_t selfSeq = 0;

uint8_t spiByte(uint8_t out) {
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, out & (1 << i));
    delayMicroseconds(2);
    digitalWrite(PIN_SCK, HIGH);
    delayMicroseconds(2);
    in = (in << 1) | (digitalRead(PIN_MISO) ? 1 : 0);
    digitalWrite(PIN_SCK, LOW);
    delayMicroseconds(2);
  }
  return in;
}

uint8_t readReg(uint8_t addr) {
  digitalWrite(PIN_NSS, LOW);
  spiByte(addr & 0x7F);
  uint8_t v = spiByte(0x00);
  digitalWrite(PIN_NSS, HIGH);
  return v;
}

void writeReg(uint8_t addr, uint8_t v) {
  digitalWrite(PIN_NSS, LOW);
  spiByte(addr | 0x80);
  spiByte(v);
  digitalWrite(PIN_NSS, HIGH);
}

void setRegBits(uint8_t reg, uint8_t value, uint8_t hi, uint8_t lo) {
  uint8_t mask = (uint8_t)(((1UL << (hi - lo + 1)) - 1) << lo);
  uint8_t cur = readReg(reg);
  writeReg(reg, (uint8_t)((cur & ~mask) | (value & mask)));
}

// NRESET is released to the internal pull-up, never driven to 3V3: the datasheet
// wants NRESET floating during power-on reset, and tying it high is forbidden.
void reset433() {
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
  delay(2);
  pinMode(PIN_RST, INPUT);
  delay(10);
}

void init433() {
  pinMode(PIN_SCK, OUTPUT); pinMode(PIN_MISO, INPUT);
  pinMode(PIN_MOSI, OUTPUT); pinMode(PIN_NSS, OUTPUT);
  digitalWrite(PIN_NSS, HIGH);
  reset433();
  if (readReg(R_VERSION) != 0x12) {
    log("[433] RegVersion is not 0x12: no SPI. Check SCK/MISO/MOSI/NSS, NRESET, 3.3 V.\n");
    return;
  }

  writeReg(R_OP_MODE, 0x80); delay(1);
  writeReg(R_OP_MODE, 0x81); delay(1);

  uint32_t frf = (uint32_t)((F433_MHZ * 1000000.0f * 524288.0f) / 32000000.0f);
  writeReg(R_FRF_MSB, (frf >> 16) & 0xFF);
  writeReg(R_FRF_MID, (frf >> 8) & 0xFF);
  writeReg(R_FRF_LSB, frf & 0xFF);
  writeReg(0x24, 0x00);                       // hopping off

  writeReg(R_PA_CONFIG, 0xFF);                // PA_BOOST 17 dBm — the healthy value
  writeReg(R_PA_DAC, 0x04);                   // default ramp, boost off, as the collar does
  writeReg(R_OCP, 0x20 | 3);                  // OCP on, 60 mA

  setRegBits(R_MODEM1, BW433_CODE, 7, 4);
  setRegBits(R_MODEM1, CR47_CODE, 3, 1);
  setRegBits(R_MODEM1, 0x04, 2, 2);           // payload CRC ON
  setRegBits(R_MODEM1, 0x00, 0, 0);           // explicit header
  setRegBits(R_MODEM2, (uint8_t)((SF433 << 4) | 0x04), 7, 2);

  setRegBits(R_DETECT_OPT, 0x03, 2, 0);
  writeReg(R_THRESH, 0x0A);
  setRegBits(R_INVERT_IQ, 0x00, 6, 6);        // what RadioLib invertIQ(false) writes
  setRegBits(R_INVERT_IQ, 0x01, 0, 0);
  writeReg(R_INVERT_IQ2, 0x1D);
  setRegBits(R_MODEM3, 0x04, 2, 2);           // AGC loop on

  writeReg(R_PREAMBLE_MSB, (PREAMBLE433 >> 8) & 0xFF);
  writeReg(R_PREAMBLE_LSB, PREAMBLE433 & 0xFF);
  writeReg(R_SYNC, SYNC433);
  writeReg(R_FIFO_TX_BASE, 0x00);
  setRegBits(R_LNA, 0x03, 1, 0);              // LNA boost

  writeReg(R_PAYLOAD_LEN, ct::FRAME_LEN);
  writeReg(R_IRQ_FLAGS, F_CLEAR_ALL);
  writeReg(R_OP_MODE, M_RXCONT);              // LoRa | RX continuous (0x85, NOT 0x83)
}

void check433() {
  uint32_t frf = ((uint32_t)readReg(R_FRF_MSB) << 16) |
                 ((uint32_t)readReg(R_FRF_MID) << 8) | readReg(R_FRF_LSB);
  logf("[433] ver %02X want 12   frf %06X want 6C4000 (433.000 MHz)\n",
       readReg(R_VERSION), frf);
  logf("[433] mc1 %02X want 76   mc2 %02X want 94   sync %02X want 12\n",
       readReg(R_MODEM1), readReg(R_MODEM2), readReg(R_SYNC));
  logf("[433] inviq %02X want 27  pa %02X want FF  flags %02X  opmode %02X\n",
       readReg(R_INVERT_IQ), readReg(R_PA_CONFIG), readReg(R_IRQ_FLAGS),
       readReg(R_OP_MODE));
}

void dump433() {
  // Standby before reading, then re-arm. `check433` reads config registers, which are
  // static, and reads clean values mid-reception. `dump433` reads packet-status
  // registers (RSSI, SNR, FIFO pointers, thresholds), which are live: read while the
  // radio is receiving they return values from part-way through a packet. The first
  // run of this printed `rssi 83 (-304)` and `snr C1 (-63)`, which is not a weak
  // signal, it is the radio's state mid-packet. Stopping RX makes the dump a snapshot.
  writeReg(R_OP_MODE, M_STDBY);
  delay(2);
  check433();
  logf("[433dump] rxcount %d  rxptr %02X  addrptr %02X  rssi %02X (%d)  snr %02X (%d)  thresh %02X  detopt %02X  lna %02X\n",
       readReg(R_RX_NB_BYTES), readReg(R_FIFO_RX_CURRENT), readReg(R_FIFO_ADDR_PTR),
       readReg(R_PKT_RSSI), rssiFrom(readReg(R_PKT_RSSI), (int8_t)readReg(R_PKT_SNR)),
       readReg(R_PKT_SNR), (int8_t)readReg(R_PKT_SNR),
       readReg(R_THRESH), readReg(R_DETECT_OPT), readReg(R_LNA));
  writeReg(R_IRQ_FLAGS, F_CLEAR_ALL);
  writeReg(R_OP_MODE, M_RXCONT);              // re-arm continuous RX
  logf("[433dump] aligned %d  misaligned %d  rxdone %d  drops %d  crc_err %d  unsettling %d  dup %d\n",
       aligned, misaligned, pktN, rxDrop, crcErr, badRead, dupDropped);
}

// Self-loopback: send a real collar frame out of this board's own RA-02. RX and TX
// share the antenna, so a decode proves SPI, settings and the decoder in one act.
void txTestFrame() {
  uint8_t frame[ct::FRAME_LEN] = {ct::FRAME_MAGIC, (uint8_t)selfSeq, 0, 100, 0, 0};
  selfSeq++;
  writeReg(R_OP_MODE, M_STDBY);
  writeReg(R_FIFO_ADDR_PTR, 0x00);
  writeReg(R_FIFO_TX_BASE, 0x00);
  writeReg(R_PAYLOAD_LEN, ct::FRAME_LEN);
  for (int i = 0; i < ct::FRAME_LEN; i++) writeReg(R_FIFO, frame[i]);
  writeReg(R_IRQ_FLAGS, F_CLEAR_ALL);         // stale flags before TX, as the collar does
  writeReg(R_OP_MODE, M_TX);                  // 0x83 = transmit
  uint32_t t0 = millis();
  bool done = false;
  while (millis() - t0 < 3000 && !done) {     // the collar allows 3 s for ~200 ms of air
    if (readReg(R_IRQ_FLAGS) & F_TX_DONE) done = true;
    delay(2);
  }
  writeReg(R_IRQ_FLAGS, F_CLEAR_ALL);
  writeReg(R_OP_MODE, M_RXCONT);
  logf("[selftest] sent %02X %02X %02X %02X %02X %02X  TxDone %s\n",
       frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], done ? "yes" : "NO");
}

// One RX poll. The alignment is RadioLib's sequence: the payload start address is in
// RegFifoRxCurrentAddr, and it has to be written into RegFifoAddrPtr before reading
// the FIFO. Reading the FIFO without seeding the pointer returned the payload shifted
// 2-4 bytes, which the frame decoder correctly rejected. Offset printed on every
// decode: 0 is aligned, anything else is the bug.
void poll433() {
  uint8_t ir = readReg(R_IRQ_FLAGS);
  if (!(ir & F_RX_DONE)) return;              // RxDone = 0x40. NOT 0x08, which is TxDone.
  if (ir & F_CRC_ERROR) {                     // corrupt frame: drop it, never a cat
    writeReg(R_IRQ_FLAGS, F_CLEAR_ALL);
    crcErr++;
    return;
  }
  pktN++;
  uint8_t len = readReg(R_RX_NB_BYTES);
  if (len > ct::FRAME_LEN) len = ct::FRAME_LEN;
  uint8_t addr = readReg(R_FIFO_RX_CURRENT);
  writeReg(R_FIFO_ADDR_PTR, addr);
  uint8_t buf[ct::FRAME_LEN];
  for (int i = 0; i < ct::FRAME_LEN; i++) buf[i] = 0;
  for (int i = 0; i < len; i++) buf[i] = readReg(R_FIFO);
  int snrQ = (int8_t)readReg(R_PKT_SNR);
  int rssi = rssiFrom(readReg(R_PKT_RSSI), snrQ);
  int snr = snrQ / 4;
  writeReg(R_IRQ_FLAGS, F_CLEAR_ALL);         // stay in continuous RX; no re-arm needed

  // Two guards, both learned by seeing the log lie. Every collar packet was reported
  // twice, the first copy with an impossible RSSI (-289 dBm): that copy is read while
  // the packet-status registers are still settling, so the numbers are garbage even
  // though the payload decoded. A real packet is never outside -140..-20 dBm and is
  // never the same (id, seq) twice inside a couple of seconds.
  if (rssi < -140 || rssi > -20) { badRead++; return; }
  static int lastId = -1, lastSeq = -1;
  static uint32_t lastMs = 0;
  if (id_seen_already(buf, &lastId, &lastSeq, &lastMs)) { dupDropped++; return; }

  bool alignedNow = (buf[0] == ct::FRAME_MAGIC);
  if (alignedNow) aligned++; else misaligned++;
  for (int i = 0; i < ct::FRAME_LEN; i++) lastPkt[i] = buf[i];
  lastPktRssi = rssi; lastPktSnr = snr;

  int8_t id = rosterNode.apply(buf, len, rssi, snr, millis());
  if (id >= 0) {
    heardId = id; heardSeq = buf[1]; heardMs = millis(); lastRssi433 = rssi;
    logf("[433] cat %d seq %d  rssi %d snr %d  batt %d  aligned=%d misaligned=%d  pk=%lu unk=%lu\n",
         id, buf[1], rssi, snr, buf[3], aligned, misaligned,
         (unsigned long)rosterNode.packets, (unsigned long)rosterNode.unknown);
  } else {
    rxDrop++;
    if (rxDrop % 25 == 1)
      logf("[433] rejected, drop=%lu  raw: %02X %02X %02X %02X %02X %02X  rssi %d snr %d  aligned=%d misaligned=%d\n",
           (unsigned long)rxDrop, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
           rssi, snr, aligned, misaligned);
  }
}

void printRoster() {
  logf("[last pkt] %02X %02X %02X %02X %02X %02X  rssi %d snr %d  detections %d\n",
       lastPkt[0], lastPkt[1], lastPkt[2], lastPkt[3], lastPkt[4], lastPkt[5],
       lastPktRssi, lastPktSnr, pktN);
  for (int i = 0; i < ct::MAX_COLLARS; i++) {
    const ct::Collar &c = rosterNode.collars[i];
    if (!c.have) continue;
    logf("[roster] cat %d  r10=%d  s2=%d  batt=%d  pk=%lu  miss=%lu  age=%lu ms\n",
         i, (int)(c.rssi_smoothed * 10), (int)(c.snr_last * 2), c.battery_pct,
         (unsigned long)c.packets, (unsigned long)c.missed,
         (unsigned long)(millis() - c.last_ms));
  }
}
#endif

// ---------------------------------------------------------------- console

static bool host() { return Serial.isPlugged(); }

void logf(const char *fmt, ...) {
  if (!host()) return;
  va_list ap;
  va_start(ap, fmt);
  Serial.vprintf(fmt, ap);
  va_end(ap);
}

void log(const char *s) { if (host()) Serial.print(s); }

// ---------------------------------------------------------------- module I/O

void poke() { Serial1.print("\r\n"); }

// One line from the module, CR/LF stripped. Empty on timeout.
String readLine(uint32_t ms) {
  String line = "";
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '\n') { if (line.length()) return line; }
      else if (c != '\r') line += c;
    }
    delay(1);
  }
  return "";
}

bool isStatus(const String &l) {
  return l == "OK" || l.startsWith("AT_");
}

void handleEvent(String e) {
  RakFrame f = parseRxp2p(e.c_str());
  if (f.ok) {
    lastRssi = f.rssi; lastSnr = f.snr; lastRxMs = millis(); warnedSilent = false;
    rxCount++;
    if (f.node >= 0) {
      if (lastSeq < 0 || f.node != lastNode) { lastSeq = f.seq; lastNode = f.node; }
      else if (f.seq > lastSeq + 1) { rxMiss += (uint32_t)(f.seq - lastSeq - 1); lastSeq = f.seq; }
      else lastSeq = f.seq;
      if (logN < LOG_MAX) { logMs[logN] = millis(); logSeq[logN] = f.seq; logRssi[logN] = f.rssi; logSnr[logN] = f.snr; logN++; }
      if (f.collar >= 0)
        logf("[RX] node %d seq %ld  cat %d rssi %d  | 915 rssi %d dBm snr %d dB  ok=%lu miss=%lu\n",
             f.node, f.seq, f.collar, f.collarRssi, f.rssi, f.snr, rxCount, rxMiss);
      else
        logf("[RX] node %d seq %ld  no collar  | 915 rssi %d dBm snr %d dB  ok=%lu miss=%lu\n",
             f.node, f.seq, f.rssi, f.snr, rxCount, rxMiss);
    } else {
      logf("[RX] %s  rssi %d snr %d\n", f.text, f.rssi, f.snr);
    }
  } else if (e.indexOf("TXP2P DONE") >= 0) {
    logf("[TX] done  seq %lu  ok=%lu\n", (unsigned long)(seq - 1), txCount);
  } else if (e.indexOf("RECEIVE TIMEOUT") >= 0) {
    log("[evt] RX window timed out — press r to re-arm\n");
  }
}

// Send one AT command, wait for its status. Query reply lands in `resp`.
bool atCmd(String cmd, uint32_t ms = 4000, int tries = 3) {
  resp = "";
  for (int attempt = 0; attempt < tries; attempt++) {
    poke();
    Serial1.print(cmd); Serial1.print("\r\n");
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
      uint32_t left = ms - (millis() - t0);
      String l = readLine(left > 0 ? left : 1);   // clamp: unsigned wrap would loop forever
      if (!l.length()) continue;
      if (isStatus(l)) return l == "OK";
      if (l.startsWith("+EVT")) { handleEvent(l); continue; }
      resp = l;                      // e.g. "AT+P2P=915000000:7:125:0:8:14"
    }
    if (attempt == tries - 1) logf("[at] %s — no reply\n", cmd.c_str());
  }
  return false;
}

// ---------------------------------------------------------------- bring-up

// RAK encodes bandwidth as a CODE in AT+P2P, not as kHz: 0 = 125, 1 = 250, 2 = 500.
// Typing 125 is rejected (AT_PARAM_ERROR) and the module keeps its own defaults, so
// the string you typed is not the string the module reports. Build both from the code.
static int bwCode(int bw) {
  if (bw == 125) return 0;
  if (bw == 250) return 1;
  if (bw == 500) return 2;
  return -1;
}

void bringUp() {
  int bw = bwCode(BW);
  if (bw < 0) { log("[warn] BW must be 125/250/500\n"); return; }
  snprintf(p2pSet, sizeof(p2pSet), "AT+P2P=%lu:%d:%d:%d:%d:%d",
           FREQ_HZ, SF, bw, CR, PREAMBLE, TX_DBM);
  snprintf(p2pWant, sizeof(p2pWant), "%lu:%d:%d:%d:%d:%d",
           FREQ_HZ, SF, bw, CR, PREAMBLE, TX_DBM);

  // Module boots ~1 s after the C3, and on a cold power-up the module can be ready
  // after the C3's first probes. Give it 40 s, not 10 — a probe that gives up
  // prints a false "module not answering" and silently skips P2P setup.
  bool alive = false;
  for (int i = 0; i < 24 && !alive; i++) {
    delay(500);
    alive = atCmd("AT", 1200, 1);
    if (!alive) logf("[probe %d] module not ready\n", i + 1);
  }
  if (!alive) {
    log("\n[warn] module not answering on UART2. Check, in this order:\n");
    log("  1. pad 24 (VDD) voltage = 3.3 V. Continuity from the C3 3V3 pad to pad 24.\n");
    log("     If the C3's 3V3 pad reads 0 V, the VDD wire is on a GND pad (pad 23 sits\n");
    log("     beside pad 24) and the rail is shorted — the C3 browns out and vanishes from USB.\n");
    log("  2. C3 GPIO20 (RX) -> pad 2 (PA2/UART2_TX); C3 GPIO21 (TX) -> pad 1 (PA3/UART2_RX).\n");
    log("     Count pads from the index mark around the perimeter. Pads 1 and 2 swapped is the\n");
    log("     usual cause when 3.3 V is confirmed.\n");
    log("  3. pad 22 (RST) HIGH, pad 21 (BOOT0) LOW. Floating BOOT0 = STM32 UART bootloader.\n");
    log("  4. 100 uF across pad 24-11 (long leg on 24), pad 24 -> pad 23 OPEN.\n");
    log("  Press 'b' for bridge mode and type AT by hand.\n");
    return;
  }

  atCmd("AT+LPM=0");              // stay awake: a sleeping module drops the first byte
  atCmd("AT+VER=?"); logf("[mod] %s\n", resp.c_str());
  atCmd("AT+SN=?");  logf("[mod] sn %s\n", resp.c_str());

  atCmd("AT+NWM=?");
  if (resp.indexOf("0") < 0) {     // not P2P
    logf("[mod] nwm=%s -> setting P2P\n", resp.c_str());
    atCmd("AT+NWM=0");             // module RESTARTS on a mode change
    delay(2000);
    Serial1.end(); Serial1.flush();
    Serial1.begin(115200, SERIAL_8N1, PIN_RAK_RX, PIN_RAK_TX);
    delay(1500);
    while (Serial1.available()) Serial1.read();   // drain restart banner
  }

  // AT+PRECV is persistent: the module boots in continuous RX from the last
  // session, and AT+P2P is rejected with AT_BUSY_ERROR while armed
  // ("P2P_RX_ON already, please disable P2P RX before setting"). Stop RX first.
  atCmd("AT+PRECV=0");

  if (!atCmd(p2pSet)) log("[warn] P2P settings rejected\n");
  atCmd("AT+P2P=?");
  logf("[mod] readback %s\n", resp.c_str());
  if (resp.indexOf(p2pWant) < 0)
    logf("[warn] readback does not contain %s — do not trust what you typed\n", p2pWant);

  if (ROLE == ROLE_LISTEN) {
    if (atCmd("AT+PRECV=65534")) log("[mod] continuous RX armed\n");
    else log("[warn] RX not armed\n");
  } else {
    logf("[mod] talker, node %d, every %d ms\n", NODE_ID, TALK_MS);
  }
  log("\n[h] help [b] bridge [p] readback [d] dump walk log [r] re-arm RX [s] send now [x] stop RX\n");
}

// ---------------------------------------------------------------- actions

void sendOne() {
  char text[40];
#if ROLE == ROLE_NODE
  // The listener parses the CAT<node> <seq> prefix, so seq counting keeps working,
  // and the tail says what this node heard on 433. A collar heard more than
  // COLLAR_FRESH_MS ago is reported as "c-1" rather than repeating a stale RSSI: a
  // relay that keeps claiming a cat it stopped hearing is worse than one that says
  // "nothing", because the stale claim looks exactly like a cat standing still.
  #define COLLAR_FRESH_MS 30000UL
  bool fresh = (heardId >= 0) && (millis() - heardMs < COLLAR_FRESH_MS);
  snprintf(text, sizeof(text), "CAT%d %lu c%d r%d",
           NODE_ID, (unsigned long)seq,
           fresh ? heardId : -1, fresh ? lastRssi433 : 0);
#else
  snprintf(text, sizeof(text), "CAT%d %lu", NODE_ID, (unsigned long)seq);
#endif
  // Echo the exact payload: the only way to line the two consoles up is to see the
  // same string sent here and received there.
  logf("[TX] payload \"%s\"\n", text);
  char hex[64] = "";
  if (hexEncode(text, hex, sizeof(hex)) == 0) { txFail++; return; }
  seq++; txCount++;
  if (!atCmd(String("AT+PSEND=") + hex)) { txFail++; logf("[TX] %lu FAILED\n", txCount); }
}

void reArmRx() {
  atCmd("AT+PRECV=0");            // 65534 cannot be re-set while armed
  atCmd("AT+PRECV=65534");
  log("[mod] RX re-armed\n");
}

void dump() {
  logf("# walk log: %d frames, t_ms, seq, rssi, snr\n", logN);
  for (int i = 0; i < logN; i++)
    logf("%lu,%ld,%d,%d\n", (unsigned long)logMs[i], logSeq[i], logRssi[i], logSnr[i]);
  logf("# rx=%lu miss=%lu\n", rxCount, rxMiss);
}

void readback() {
  atCmd("AT+VER=?");   logf("[mod] ver %s\n", resp.c_str());
  atCmd("AT+NWM=?");   logf("[mod] nwm %s\n", resp.c_str());
  atCmd("AT+P2P=?");   logf("[mod] p2p %s\n", resp.c_str());
  atCmd("AT+PRECV=?"); logf("[mod] precv %s\n", resp.c_str());
  logf("[mod] rssi %d snr %d\n", lastRssi, lastSnr);
}

void bridge() {
  log("\n[bridge] keystrokes go to the module, module output to the console.\n");
  log("[bridge] try: AT+P2P=?  AT+PSEND=434154312031  AT+PRECV=65534.  'q' quits.\n");
  bool quit = false;
  while (!quit) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n') Serial1.print("\r\n");    // AT needs CRLF
      else if (c == 'q') { quit = true; break; }
      else Serial1.write(c);
    }
    while (Serial1.available()) Serial.write((char)Serial1.read());
    delay(1);
  }
  log("[bridge] quit\n");
}

// ---------------------------------------------------------------- setup/loop

// Order matters. The RA-02's 100 uF plus its cold inrush charges through the same
// LDO the RAK3172 runs from. Doing the RA-02 first means that dip lands while the
// RAK is still booting, and a module that browns out during its own power-on can
// come up in the STM32 UART bootloader and never answer AT. So: talk to the RAK
// FIRST, let the rail settle, and only then touch the RA-02.
void setup() {
  Serial.begin(115200);              // USB CDC console
  Serial.setTxTimeoutMs(0);          // a Host that stops reading cannot stall loop()
  Serial1.begin(115200, SERIAL_8N1, PIN_RAK_RX, PIN_RAK_TX);
  delay(1500);
  logf("\nPOC915 ver 2  role %s  node %d\n",
       ROLE == ROLE_LISTEN ? "LISTENER" : (ROLE == ROLE_NODE ? "NODE" : "TALKER"), NODE_ID);
  logf("RAK on GPIO%d(RX)/GPIO%d(TX), 915 MHz SF%d BW%d CR%d preamble %d, %d dBm\n",
       PIN_RAK_RX, PIN_RAK_TX, SF, BW, CR, PREAMBLE, TX_DBM);
  log("Antenna must be on the module before it transmits. 8.2 cm radiator.\n");

  bringUp();                         // RAK first, on an undisturbed rail

#if ROLE == ROLE_NODE
  delay(500);                        // let the rail recover before the RA-02 draws
  logf("RA-02 on SCK=GPIO%d MISO=GPIO%d MOSI=GPIO%d NSS=GPIO%d RST=GPIO%d DIO0=GPIO%d, "
       "433 MHz SF%d BW125 CR4/7 sync %02X preamble %d. Antenna 17.3 cm.\n",
       PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS, PIN_RST, PIN_DIO0, SF433, SYNC433, PREAMBLE433);
  init433();
  check433();
  // If the RA-02 knocked the module out, say so now rather than hours later.
  if (!atCmd("AT", 1200, 2)) log("[warn] RAK stopped answering after the RA-02 was initialised\n");
#endif
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'h') log("\n[h] help [b] bridge [p] readback [r] re-arm RX [s] send now [x] stop RX\n");
    else if (c == 'b') bridge();
    else if (c == 'p') readback();
    else if (c == 'd') dump();
    else if (c == 'r') reArmRx();
    else if (c == 's') sendOne();
#if ROLE == ROLE_NODE
    else if (c == 'k') check433();
    else if (c == 'i') dump433();
    else if (c == 'c') printRoster();
    else if (c == 't') txTestFrame();
#endif
    else if (c == 'x') { atCmd("AT+PRECV=0"); log("[mod] RX stopped\n"); }
  }

  // Drain module events continuously — this is where RX frames land.
  String l = readLine(ROLE == ROLE_LISTEN ? 200 : 20);
  if (l.length()) { if (l.startsWith("+EVT")) handleEvent(l); else logf("[mod] %s\n", l.c_str()); }

#if ROLE == ROLE_NODE
  poll433();
#endif

  if (ROLE == ROLE_TALK || ROLE == ROLE_NODE) {
    static uint32_t lastTalk = 0;
    if (millis() - lastTalk >= TALK_MS) { lastTalk = millis(); sendOne(); }
  }

  static uint32_t lastBeat = 0;
  if (millis() - lastBeat >= HEARTBEAT_MS) {
    lastBeat = millis();
    if (ROLE == ROLE_LISTEN)
      logf("[beat] rx=%lu miss=%lu last rssi %d snr %d\n", rxCount, rxMiss, lastRssi, lastSnr);
    else
      logf("[beat] tx=%lu fail=%lu seq %lu\n", txCount, txFail, (unsigned long)seq);
  }

  if (ROLE == ROLE_LISTEN && lastRxMs && !warnedSilent &&
      millis() - lastRxMs > 30000) {
    warnedSilent = true;
    log("[warn] 30 s of silence. Press r to re-arm RX, p for readback.\n");
  }
}
