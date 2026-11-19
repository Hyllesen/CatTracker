// Cat Tracker - handheld LoRa receiver with BLE signal broadcasting.
//
// Step 1 of moving the homing UI to the phone. The handheld does what it
// already does (receive LoRa pings from the collar) and additionally runs a
// BLE GATT server that notifies the phone with the signal strength, so the
// phone can render the tone and the signal bar.
//
// MULTI-COLLAR: up to MAX_COLLARS collars are tracked at once. Each collar
// carries an id in its packet, so the handheld keeps one RSSI history per id
// and publishes the whole roster - every collar heard so far - to the phone
// in a single notification.
//
// Air settings below are the ones proven to interoperate with the collar; see
// handheld/README.md for why modem registers must be written field-by-field.

#include <Arduino.h>
#include <RadioLib.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <math.h>

// ---- Radio hardware config (ESP32-C3 SuperMini + RA-02 / SX1278) ----
const uint8_t PIN_NSS  = 7;
const uint8_t PIN_DIO0 = 3;
const uint8_t PIN_RST  = 10;

Module* mod = new Module(PIN_NSS, PIN_DIO0, PIN_RST);
SX1278 radio = mod;

// ---- LoRa air settings - MUST match the collar ----
const float   FREQ_MHZ   = 433.0;
const float   BANDWIDTH  = 125.0;
const uint8_t SPREADING  = 9;
const uint8_t CODING_R   = 7;
const uint8_t SYNC_WORD  = 0x12;
const int8_t  TX_POWER   = 17;
const uint16_t PREAMBLE   = 8;

// ---- How many collars are tracked at once ----
// Three is the most the roster format can carry: see ROSTER_MAX_LEN.
const uint8_t MAX_COLLARS = 3;

// SLOW_INTERVAL_S in collar/collar.ino. The collar's slow period is its worst
// case (find mode only makes it faster), so both the receive window and the
// staleness window are sized from it. Change one, change the other - these two
// boards are coupled, exactly like the air settings.
const uint32_t COLLAR_SLOW_MS = 60000;

// ---------------------------------------------------------------------------
// Collar packet format - MUST match collar/collar.ino
// ---------------------------------------------------------------------------
// Six bytes:
//
//   byte 0   0xC1        magic / protocol version
//   byte 1   seq         wrapping 0..255, increments every ping
//   byte 2   id          0..MAX_COLLARS-1
//   byte 3   battery     0..100, 0xFF = unknown (ADC pin unwired)
//   byte 4   flags       bit0 = LOW_BATT (<15%), rest reserved
//   byte 5   reserved
//
// The magic byte is what separates a collar packet from anything else on the
// air. LoRa's own payload CRC covers transmission errors, so there is no CRC
// of our own.
const uint8_t FRAME_MAGIC        = 0xC1;
const uint8_t FRAME_LEN          = 6;
const uint8_t FRAME_FLAG_LOW_BATT = 0x01;
const uint8_t FRAME_FLAG_FIND     = 0x02;   // collar is in find mode
const uint8_t BATT_UNKNOWN       = 0xFF;

// ---------------------------------------------------------------------------
// Downlink command frame - MUST match collar/collar.ino (Phase 1)
// ---------------------------------------------------------------------------
// Six bytes, the same size as a ping because at SF9 five and six bytes cost the
// same airtime:
//
//   byte 0   0xC2        magic - a ping is 0xC1, so the two never collide
//   byte 1   target id
//   byte 2   command     0x01 FIND_START, 0x02 FIND_STOP
//   byte 3   parameter   minutes for FIND_START (0 = collar's own default)
//   byte 4   command seq, for logs
//   byte 5   reserved 0
//
// Phase 0a uses FIND_START as its test payload: the collar probe only has to
// prove it decoded a frame, and reusing the real command means the bench test
// exercises the exact bytes Phase 1 will send.
const uint8_t CMD_MAGIC       = 0xC2;
const uint8_t CMD_LEN         = 6;
const uint8_t CMD_FIND_START  = 0x01;
const uint8_t CMD_FIND_STOP   = 0x02;

// 8 symbols - deliberately the same as a ping, and not a knob to raise.
//
// Measured on the bench (docs/plans/find-mode-bench-findings.md §3): a collar
// whose RegPreambleLsb is 8 decodes a command with an 8-symbol preamble (3 of 3
// at -28 dBm) and decodes NOTHING at 16, 24 or 32 (0 of 12). The receiver
// expects the sync word at the symbol its own preamble register names, so a
// longer preamble never locks - and the symptom is indistinguishable from a
// transmitter that is not radiating. A long preamble is therefore not cheap
// insurance, it is a silent failure.
//
// It is also unnecessary: the measured turnaround is ~4 ms (packet complete to
// carrier up), so a 100 ms collar window catches a command whose 8-symbol
// preamble ends ~37 ms after TxDone. `t <id> <min> <preamble>` still exposes the
// knob for re-testing, but do not change this default without re-running that
// test.
const uint16_t CMD_PREAMBLE   = 8;

uint8_t cmdSeq = 0;

// Bench auto-reply: answer a ping from `autoId` with a command, automatically,
// so the turnaround can be measured without a human pressing a key. -1 disarms.
int8_t  autoId      = -1;
uint8_t autoLeft    = 0;
uint8_t autoMinutes = 15;

// ---------------------------------------------------------------------------
// Roster published to the phone
// ---------------------------------------------------------------------------
//   byte 0    version = 0x10
//   byte 1    count   (how many collars follow)
//   per collar, 6 bytes, in id order - the position IS the id:
//     +0  flags    bit0 HAVE, bit1 FRESH, bit2 STALE, bit3 LOW_BATT
//     +1  rssi     dBm * 10, int16 little-endian
//     +3  snr      dB * 2, int8 (0.5 dB steps)
//     +4  age      seconds since that collar's last packet, capped 255
//     +5  battery  0..100, 255 = unknown
//
// The version byte is 0x10 rather than the more natural 0x02 because the
// legacy single-collar packet started with a flags byte that only ever used
// bits 0-2. A value with bit 4 set cannot be one, so the phone can tell the
// two formats apart by the first byte alone - which is what lets the app
// talk to a handheld that has not been reflashed yet.
//
// Why the id is the position rather than a byte in the entry: the six bytes
// of an entry are worth more than the id is. Carrying the id explicitly
// would cost a byte per collar and push the roster past 20 bytes, which is
// the limit that matters (see below).
//
// Why 20 bytes is the limit that matters: 23 is the DEFAULT ATT MTU, which
// leaves 20 bytes of payload after the opcode and the handle. NimBLE
// truncates a longer notification to MTU-3 instead of failing, so an
// oversized roster would arrive silently mangled and the phone would see a
// length it cannot make sense of. Fitting the default MTU means the roster
// works whether or not MTU negotiation succeeded on the phone.
//
// That also caps MAX_COLLARS: a fourth collar would need 26 bytes.
const uint8_t ROSTER_VERSION    = 0x10;
const uint8_t ROSTER_ENTRY_LEN  = 6;
const uint8_t ROSTER_MAX_LEN    = 2 + MAX_COLLARS * ROSTER_ENTRY_LEN;
static_assert(ROSTER_MAX_LEN <= 20, "roster must fit the default ATT payload");

const uint8_t ROSTER_FLAG_HAVE     = 0x01;
const uint8_t ROSTER_FLAG_FRESH    = 0x02;
const uint8_t ROSTER_FLAG_STALE    = 0x04;
const uint8_t ROSTER_FLAG_LOW_BATT = 0x08;
// Bits 4-5 are free: the byte only ever used 0-3, in both the firmware and the
// app's decoder. This is how find state reaches the phone without a second
// characteristic and without breaking the 20-byte limit.
const uint8_t ROSTER_FLAG_FIND_REQ    = 0x10;
const uint8_t ROSTER_FLAG_FIND_ACTIVE = 0x20;

// ---- Signal quality ----
// The RSSI published to the phone is the reading for the last packet heard,
// not a moving average.
//
// It used to be a 12-sample mean (~96 s of history at an 8 s ping) to smooth
// multipath spikes. That made the number lag the cat: walking past a collar
// changed nothing on screen for a minute and a half, and a lagging reading is
// worse than a noisy one when the number is being used to find something.
// Measured noise is only ~0.7 dB, and the tone moves in 13.5 dB steps (the
// 135 dB display range over 10 steps), so one packet's worth of jitter never
// reaches the audio.
//
// SNR has always been the last packet's, and now both numbers describe the
// same packet.
//
// Must comfortably exceed one collar ping interval, or a couple of missed
// packets trip "signal lost" while the collar is perfectly fine.
// 2.5x the collar's slow interval, so one missed ping does not mark a healthy
// cat lost. (Was 15 s when the collar pinged every 8 s; at a 60 s slow
// interval it has to move with it, exactly like COLLAR_PING_MS.)
const uint32_t STALE_MS = (COLLAR_SLOW_MS * 5) / 2;

// The collar's transmit interval (PING_INTERVAL_MS in collar/collar.ino).
// RadioLib's receive() blocks until a preamble arrives or the timeout
// expires, so this window must cover a full ping period of the SLOWEST
// collar or roughly half of every ping is dropped. The extra margin stops a
// ping landing exactly on the window edge from being missed due to
// scheduling jitter.
//
// NOTE: these two boards are coupled - change one interval, change the other.
//
// Each collar pings on its own period, offset by its id, so the slowest one is
// id MAX_COLLARS-1 and the window has to cover that, not the nominal interval.
// The step size differs per collar build, and the window has to cover the
// largest one in the fleet:
//   collar/collar.ino        PING_PERIOD_STEP_MS = 137 ms per id
//   at-tiny85-collar/        sleepFor(WDTO_1S) per id = 1000 ms per id, and its
//                            watchdog oscillator runs 7-10 s for a nominal 8 s
// So an ATtiny at PING_TICKS=1 with id 2 pings every ~10 s, ~13 s at the fast
// end of the oscillator. A window under that drops a ping every time the beacon
// lands outside it, which shows up as `missed` in tools/monitor.py - not as an
// error, and not as a dead collar.
//
// 11000 covers 10 s nominal + 1 s of oscillator drift. receive() returns as soon
// as a preamble arrives, so a longer window costs nothing in latency. STALE_MS
// below stays above the worst case (13 s), which is why it does not need to move.
// SLOW_INTERVAL_S in collar/collar.ino. This is the collar's SLOW period, i.e.
// its worst case: find mode only makes it ping faster, and faster is always
// inside this window. It must be kept in step with the collar build or the
// handheld spends most of every slow interval deaf.
const uint32_t COLLAR_PING_MS = COLLAR_SLOW_MS + (MAX_COLLARS - 1) * 1000 + 1000;
const uint32_t RX_TIMEOUT_MS  = COLLAR_PING_MS + 200;   // 63200 ms

// ---- BLE ----
#define SERVICE_UUID "8e7a1b20-3f4d-4c8e-9a1f-2b6d5e4c3a21"
#define SIGNAL_UUID  "8e7a1b21-3f4d-4c8e-9a1f-2b6d5e4c3a21"
#define FIND_UUID    "8e7a1b22-3f4d-4c8e-9a1f-2b6d5e4c3a21"
#define DEVICE_NAME  "CatTracker"

BLECharacteristic* signalChar = nullptr;
BLECharacteristic* findChar   = nullptr;

// Set true with 'v' on the serial port when debugging.
//
// MUST default to false. Serial is native USB CDC, and when the board is
// powered from a power bank or wall charger nothing ever reads that port. The
// CDC buffer fills, Serial.print() blocks, and the whole loop stops - taking
// LoRa receive and BLE notifications down with it. This was observed stalling
// the loop for 52 s while packets kept arriving unheard.
static bool verboseLog = false;

// ---------------------------------------------------------------------------
// Per-collar state
// ---------------------------------------------------------------------------
// The array index IS the collar id, so there is no map and no lookup: a
// packet naming id 2 lands in collars[2] or is dropped.
//
// ~700 bytes total for three collars. Negligible on a C3 with 400 KB SRAM.
struct CollarState {
  bool     have;            // at least one packet heard from this collar
  uint8_t  id;
  uint8_t  seq;             // last seq seen, for the missed-packet counter
  bool     seqValid;
  uint8_t  batteryPct;      // 255 = unknown
  uint8_t  flags;           // bit0 LOW_BATT, bit1 FIND, carried from the collar
  bool     findActive;      // collar says it is in find mode (byte 4 bit 1)
  uint8_t  findMinutes;     // minutes left, from byte 5 (0 when not in find)
  float    rssiLast;        // RSSI of this collar's last packet, dBm
  float    snrLast;         // SNR of this collar's last packet, dB
  uint32_t lastMs;
  uint32_t packets;
  uint32_t missed;          // seq gaps: packets we never heard
};
CollarState collars[MAX_COLLARS];

// ---------------------------------------------------------------------------
// Find requests, per collar (Phase 1)
// ---------------------------------------------------------------------------
// The command cannot be transmitted whenever we feel like it: it only has a
// chance of being heard in the short window that opens when the collar finishes
// its ping, and the collar only listens then. So a request is a flag that the
// packet handler turns into a transmission on the next ping from that collar.
//
// The acknowledgement is the next ping: it carries FIND_ACTIVE for a start, or
// stops carrying it for a stop. No separate ack frame.
const uint8_t FIND_REQ_NONE  = 0;
const uint8_t FIND_REQ_START = 1;
const uint8_t FIND_REQ_STOP  = 2;

uint8_t  findRequest[MAX_COLLARS];   // FIND_REQ_*
uint8_t  findMinutes[MAX_COLLARS];   // duration asked for
uint8_t  findTries[MAX_COLLARS];     // attempts used on the current request
uint32_t findSent = 0, findAcked = 0, findFailed = 0;

// Guards the collar state and the roster build. The keep-alive task builds a
// roster too, so without this it could read a half-updated entry.
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t rxBuf[64];
uint32_t packetCount = 0;
uint32_t unknownIdCount = 0;

// Legacy collar payload, "CAT-01;bat=87". Accepted for one release and
// mapped onto collar 0, so cats can be swapped one at a time instead of
// opening all three enclosures at once. An old collar sends 'C' = 0x43, which
// is not the binary frame's magic, so the two never collide.
bool parseLegacy(const uint8_t* buf, size_t len, uint8_t* batt) {
  // The payload is ASCII but RadioLib neither reports its length nor
  // terminates it, so scan to the first non-printable byte.
  size_t n = 0;
  while (n < len && buf[n] >= 0x20 && buf[n] < 0x7F) n++;
  if (n < 4 || memcmp(buf, "CAT-", 4) != 0) return false;

  // The buffer is scratch space, so terminating it here is safe.
  char tmp[32];
  size_t copy = n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1;
  memcpy(tmp, buf, copy);
  tmp[copy] = '\0';

  const char* p = strstr(tmp, "bat=");
  if (!p) { *batt = BATT_UNKNOWN; return true; }
  p += 4;
  if (*p < '0' || *p > '9') { *batt = BATT_UNKNOWN; return true; }  // "--"
  int v = 0;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    if (v > 100) { v = 100; break; }
    p++;
  }
  *batt = (uint8_t)v;
  return true;
}

// Apply one received packet to the collar state.
// Returns the collar id, or -1 if the packet was not usable.
int8_t applyPacket(const uint8_t* buf, size_t len, float rssi, float snr) {
  uint8_t id, batt, flags;
  bool hasSeq = false;
  uint8_t seq = 0;

  if (len >= FRAME_LEN && buf[0] == FRAME_MAGIC) {
    id    = buf[2];
    seq   = buf[1];
    batt  = buf[3];
    flags = buf[4];
    hasSeq = true;
    if (id >= MAX_COLLARS) {
      // A collar flashed with an id this build does not know. Counted and
      // dropped rather than silently folded into another collar's row.
      unknownIdCount++;
      return -1;
    }
  } else {
    if (!parseLegacy(buf, len, &batt)) return -1;
    id = 0;
    flags = 0;
    /* no find information in a legacy ASCII frame */
  }

  CollarState& c = collars[id];
  if (!c.have) {
    c.have = true;
    c.id = id;
    c.seqValid = false;
  }

  // The seq counter turns a silent loss into a number. A gap means packets
  // were sent that we never heard - usually two collars transmitting on top
  // of each other, but also a packet lost for any other reason, so this is
  // an upper bound on collisions rather than a count of them.
  //
  // A legacy ASCII collar carries no seq, so it is left out of the count
  // entirely rather than being treated as a gap of 255 packets.
  if (hasSeq) {
    if (c.seqValid) {
      uint8_t gap = (uint8_t)(seq - c.seq - 1);
      c.missed += gap;
    }
    c.seq = seq;
    c.seqValid = true;
  }

  c.batteryPct = batt;
  c.flags = flags;
  // Byte 5 was reserved and is now "minutes of find mode left"; a pre-find
  // collar writes 0 there, which reads as "not in find mode" - the same thing
  // bit 0 of FLAG_FIND says, so the two never disagree in practice.
  c.findActive = (flags & FRAME_FLAG_FIND) != 0;
  c.findMinutes = c.findActive ? (uint8_t)((len >= FRAME_LEN) ? buf[5] : 0) : 0;
  c.snrLast = snr;
  c.rssiLast = rssi;
  c.lastMs = millis();
  c.packets++;
  return (int8_t)id;
}

// ---------------------------------------------------------------------------
// BLE diagnostics state
// ---------------------------------------------------------------------------
// Declared ahead of publishRoster() so the publish path can report it. NimBLE
// silently drops notify() calls when nobody has subscribed, which looks
// identical to "the phone isn't updating"; these counters make every step of
// the subscription handshake visible on the serial monitor.
bool phoneConnected = false;
bool phoneSubscribed = false;
uint32_t notifyOk = 0, notifyFail = 0;
uint32_t lastPublishMs = 0;

// Set true to print a line for every publish. Left off so the walk data stays
// readable; the subscribe events print unconditionally.
const bool VERBOSE_PUBLISH = false;

// Build the roster into `out`. Returns its length.
//
// Callers must hold stateMux: the collar state is read here and the
// keep-alive task can be running at the same time as loop().
uint8_t buildRoster(uint8_t* out, int8_t freshId) {
  out[0] = ROSTER_VERSION;
  out[1] = MAX_COLLARS;
  uint8_t* p = out + 2;
  uint32_t now = millis();

  for (uint8_t i = 0; i < MAX_COLLARS; i++) {
    CollarState& c = collars[i];

    uint8_t flags = 0;
    if (c.have) {
      flags |= ROSTER_FLAG_HAVE;
      if ((int8_t)i == freshId) flags |= ROSTER_FLAG_FRESH;
      if (now - c.lastMs > STALE_MS) flags |= ROSTER_FLAG_STALE;
      if (c.flags & FRAME_FLAG_LOW_BATT) flags |= ROSTER_FLAG_LOW_BATT;
      if (c.flags & FRAME_FLAG_FIND)     flags |= ROSTER_FLAG_FIND_ACTIVE;
      if (findRequest[i])               flags |= ROSTER_FLAG_FIND_REQ;
    }

    *p++ = flags;

    int16_t r10 = (int16_t)(c.have ? c.rssiLast * 10.0f : 0.0f);
    *p++ = (uint8_t)(r10 & 0xFF);
    *p++ = (uint8_t)(r10 >> 8);

    int8_t s2 = (int8_t)lroundf(c.have ? c.snrLast * 2.0f : 0.0f);
    *p++ = (uint8_t)s2;

    uint32_t ageS = c.have ? ((now - c.lastMs) / 1000) : 0;
    *p++ = ageS > 255 ? 255 : (uint8_t)ageS;

    *p++ = c.have ? c.batteryPct : BATT_UNKNOWN;
  }
  return (uint8_t)(2 + MAX_COLLARS * ROSTER_ENTRY_LEN);
}

// Build and push the current roster to any subscribed phone.
//
// freshId is the collar that just sent a packet, or -1 for "nothing new" -
// a keep-alive or a timeout. The phone uses FRESH to tell a real beacon from
// a keep-alive, so it does not count the same packet twice.
void publishRoster(int8_t freshId) {
  uint8_t pkt[ROSTER_MAX_LEN];
  uint8_t len;

  portENTER_CRITICAL(&stateMux);
  len = buildRoster(pkt, freshId);
  portEXIT_CRITICAL(&stateMux);

  lastPublishMs = millis();

  if (VERBOSE_PUBLISH) {
    Serial.print(F("  [publish] fresh="));
    Serial.print((int)freshId);
    Serial.print(F(" bytes="));
    Serial.println(len);
  }

  if (signalChar) {
    signalChar->setValue(pkt, len);
    signalChar->notify();
  }
}

// ---------------------------------------------------------------------------
// Keep-alive
// ---------------------------------------------------------------------------
// Comfortably under Android's ~7.2 s supervision timeout, and frequent enough
// that the phone always has a fresh reading without flooding the radio.
const uint32_t KEEPALIVE_MS = 2000;

// The radio loop publishes once per received packet, and radio.receive()
// blocks for RX_TIMEOUT_MS, so without help the phone would hear nothing for
// ~11 s at a time. Android's BLE supervision timeout is ~7.2 s, so that gap
// can drop the link - and the app has no way to notice, because a dead
// connection looks exactly like a quiet one.
//
// This task re-publishes the roster on its own timer so the link stays warm
// between beacons. The radio is untouched: loop() still owns receive(),
// still uses the full window, and still misses nothing.
//
// The roster is rebuilt rather than replayed from a cache, so the ages in it
// keep counting up. A replayed copy would freeze the age at whatever it was
// when the last packet arrived, and the phone would show a cat as "3 s old"
// for as long as the cat was actually missing.
void publishKeepaliveTask(void* arg) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(KEEPALIVE_MS));

    // Only while a phone is actually listening - notifying with no client
    // connected is what fills notify_fail in the diagnostics.
    if (!signalChar || !phoneConnected || !phoneSubscribed) {
      continue;
    }

    // No fresh collar: this is a keep-alive, not a beacon. The phone must
    // not count it as a packet or it would beep on a 2 s cadence.
    publishRoster(-1);
  }
}

const char* statusName(uint32_t s) {
  switch (s) {
    case BLECharacteristicCallbacks::Status::SUCCESS_INDICATE: return "indicate ok";
    case BLECharacteristicCallbacks::Status::SUCCESS_NOTIFY:  return "notify ok";
    case BLECharacteristicCallbacks::Status::ERROR_INDICATE_TIMEOUT: return "indicate TIMEOUT";
    case BLECharacteristicCallbacks::Status::ERROR_INDICATE_FAILURE: return "indicate FAILED";
    case BLECharacteristicCallbacks::Status::ERROR_INDICATE_DISABLED: return "indications not enabled";
    case BLECharacteristicCallbacks::Status::ERROR_NOTIFY_DISABLED: return "notifications not enabled";
    case BLECharacteristicCallbacks::Status::ERROR_GATT:              return "GATT ERROR";
    case BLECharacteristicCallbacks::Status::ERROR_NO_CLIENT:         return "no client connected";
    case BLECharacteristicCallbacks::Status::ERROR_NO_SUBSCRIBER:     return "NO SUBSCRIBER";
    default: return "other";
  }
}

class SignalCharCb : public BLECharacteristicCallbacks {
#if defined(CONFIG_NIMBLE_ENABLED)
  void onSubscribe(BLECharacteristic*, ble_gap_conn_desc*, uint16_t subValue) override {
    // subValue bit0 = notify, bit1 = indicate
    phoneSubscribed = (subValue & 0x0001) != 0 || (subValue & 0x0002) != 0;
    Serial.print(F("[ble] subscribe event, value=0x"));
    Serial.print(subValue, HEX);
    Serial.print(F(" -> "));
    Serial.println(phoneSubscribed ? F("NOTIFY ON") : F("notify off"));
  }
#endif
  void onStatus(BLECharacteristic*, BLECharacteristicCallbacks::Status s, uint32_t code) override {
    if (s == BLECharacteristicCallbacks::Status::SUCCESS_NOTIFY) {
      notifyOk++;
      return;   // too chatty to print every one
    }
    if (s == BLECharacteristicCallbacks::Status::ERROR_NO_SUBSCRIBER ||
        s == BLECharacteristicCallbacks::Status::ERROR_NO_CLIENT) {
      notifyFail++;
    }
    if (verboseLog) {
      Serial.print(F("[ble] notify status: "));
      Serial.print(statusName(s));
      if (code) { Serial.print(F(" code=")); Serial.print(code); }
      Serial.println();
    }
  }
};

// Restart advertising when the phone disconnects so it can reconnect.
class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    phoneConnected = true;
    if (verboseLog) Serial.println(F("[ble] phone CONNECTED"));
  }
  void onDisconnect(BLEServer* s) override {
    phoneConnected = false;
    phoneSubscribed = false;
    if (verboseLog) Serial.println(F("[ble] phone disconnected - advertising again"));
    s->getAdvertising()->start();
  }
};

// ---------------------------------------------------------------------------
// Find command characteristic (phone -> handheld)
// ---------------------------------------------------------------------------
// Three bytes, little thought required:
//
//   [0] collar id
//   [1] 0x01 FIND_START, 0x02 FIND_STOP
//   [2] minutes for FIND_START (0 = the collar's default)
//
// A longer write is accepted and the tail ignored, so the format can grow
// without invalidating an older app.
//
// This callback runs on the NimBLE task. It must NOT touch the radio: the
// command can only be transmitted inside the window after that collar's next
// ping, which is loop()'s job. So it sets the request and returns.
class FindCharCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    if (!ch) return;
    uint8_t* d = ch->getData();
    size_t n = ch->getLength();
    if (!d || n < 2) return;

    const uint8_t id = d[0];
    const uint8_t cmd = d[1];
    const uint8_t minutes = (n > 2) ? d[2] : 0;
    if (id >= MAX_COLLARS) return;
    if (cmd != CMD_FIND_START && cmd != CMD_FIND_STOP) return;

    portENTER_CRITICAL(&stateMux);
    findRequest[id] = (cmd == CMD_FIND_START) ? FIND_REQ_START : FIND_REQ_STOP;
    findMinutes[id] = minutes ? minutes : 15;
    findTries[id] = 0;
    portEXIT_CRITICAL(&stateMux);

    Serial.print(F("[ble] find write id="));
    Serial.print((int)id);
    Serial.print(cmd == CMD_FIND_START ? F(" START ") : F(" STOP "));
    Serial.print((int)minutes);
    Serial.println(F(" min - queued for the next ping"));
  }
};

void setupBLE() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(64);

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCb());

  BLEService* svc = server->createService(SERVICE_UUID);
  signalChar = svc->createCharacteristic(
      SIGNAL_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  // No BLE2902 here: this core uses NimBLE, which adds the CCCD descriptor
  // automatically for notifying characteristics.
  //
  // PROPERTY_READ matters now that there is more than one collar: a phone
  // that connects late does a read() for a full roster snapshot and *then*
  // subscribes. Without it, the app only ever learns about the cats that
  // happen to ping after it connected - which is how you end up convinced
  // that one cat is missing.
  signalChar->setCallbacks(new SignalCharCb());

  // The downlink. WRITE (with response) so the phone learns the write was
  // accepted; WRITE_NR as well so an app that does not wait for the response
  // still works.
  findChar = svc->createCharacteristic(
      FIND_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  findChar->setCallbacks(new FindCharCb());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();

  if (verboseLog) Serial.println(F("[ble] advertising as CatTracker"));
}

bool initRadio() {
  int16_t state = radio.begin(FREQ_MHZ, BANDWIDTH, SPREADING, CODING_R,
                              SYNC_WORD, TX_POWER, PREAMBLE);
  if (state != RADIOLIB_ERR_NONE) {
    if (verboseLog) {
      Serial.print(F("[radio] init FAILED code "));
      Serial.println(state);
    }
    return false;
  }
  return true;
}

// Transmit one command frame. Returns true when TxDone was observed.
//
// Callers may use this from the packet path, so it must stay cheap until the
// moment the carrier starts: RadioLib's transmit() does standby, FIFO write and
// key-up itself, and nothing here should sit in front of it.
bool sendCommand(uint8_t id, uint8_t cmd, uint8_t param, uint16_t preamble = CMD_PREAMBLE) {
  uint8_t frame[CMD_LEN] = { CMD_MAGIC, id, cmd, param, ++cmdSeq, 0x00 };
  radio.setPreambleLength(preamble);
  int16_t state = radio.transmit(frame, CMD_LEN);
  radio.setPreambleLength(PREAMBLE);
  return state == RADIOLIB_ERR_NONE;
}

// ---------------------------------------------------------------------------
// Bench serial commands (Phase 0a)
// ---------------------------------------------------------------------------
//   t <id> [min]   send one command frame now, without waiting for a ping
//   a <id> [n]     answer the next n pings from <id> with a command (n=5)
//   a              disarm auto-reply
//   v              toggle verbose logging (single key, no newline needed)
//
// Both a newline and a 150 ms gap end a line, because tools/serial_cmd.py sends
// its argument with no terminator at all - it was written for single-keypress
// commands. Without the gap rule `t 2` would sit in the buffer forever.
//
// The replies here are printed unconditionally, unlike everything else in this
// sketch: they only exist because a host typed something, so there is by
// definition a host to read them.
const uint8_t CMD_LINE_MAX = 24;
char     cmdLine[CMD_LINE_MAX];
uint8_t  cmdLineLen = 0;
uint32_t lastSerialByteMs = 0;

void handleSerialLine(char* line) {
  while (*line == ' ') line++;
  if (*line == '\0') return;

  // The verb is a whole word, not a single character: "find 2 15" used to be
  // parsed as verb 'f' with the argument taken from "ind 2 15", which strtol
  // reads as 0 - i.e. every find request silently went to collar 0.
  char* p = line;
  while (*p && *p != ' ') p++;
  if (*p) { *p = '\0'; p++; }
  const char* verb = line;
  char* rest = p;

  while (*rest == ' ') rest++;
  int32_t a = 0;
  bool haveA = false;
  if (*rest) { haveA = true; a = strtol(rest, &rest, 10); }
  while (*rest == ' ') rest++;
  int32_t b = 0;
  if (*rest) b = strtol(rest, &rest, 10);
  while (*rest == ' ') rest++;
  int32_t c = 0;
  if (*rest) c = strtol(rest, &rest, 10);

  if (strcmp(verb, "v") == 0) {
    verboseLog = !verboseLog;
    if (verboseLog) { printBanner(); printCollars(); }
    return;
  }

  if (strcmp(verb, "find") == 0) {
    if (!haveA || a < 0 || a >= MAX_COLLARS) {
      Serial.println(F("[cmd] find <id> [min] - id must be 0..2"));
      return;
    }
    findRequest[a] = FIND_REQ_START;
    findMinutes[a] = (b > 0 && b < 256) ? (uint8_t)b : 15;
    findTries[a] = 0;
    Serial.print(F("[cmd] find id="));
    Serial.print(a);
    Serial.print(F(" for "));
    Serial.print((int)findMinutes[a]);
    Serial.println(F(" min - sent in the window after the next ping"));
    return;
  }

  if (strcmp(verb, "stop") == 0) {
    if (!haveA || a < 0 || a >= MAX_COLLARS) {
      Serial.println(F("[cmd] stop <id> - id must be 0..2"));
      return;
    }
    findRequest[a] = FIND_REQ_STOP;
    findTries[a] = 0;
    Serial.print(F("[cmd] stop id="));
    Serial.print(a);
    Serial.println(F(" - sent in the window after the next ping"));
    return;
  }

  if (strcmp(verb, "t") == 0) {
    if (!haveA || a < 0 || a >= MAX_COLLARS) {
      Serial.println(F("[cmd] t <id> [min] [preamble] - id must be 0..2"));
      return;
    }
    uint8_t minutes = (b > 0 && b < 256) ? (uint8_t)b : 15;
    // Optional third argument: preamble symbols. Being able to A/B this without
    // a reflash is what tells "the collar cannot decode a 32-symbol preamble"
    // apart from "the collar cannot hear this transmitter at all".
    uint16_t preamble = (c > 0 && c <= 255) ? (uint16_t)c : CMD_PREAMBLE;
    uint32_t t0 = micros();
    bool ok = sendCommand((uint8_t)a, CMD_FIND_START, minutes, preamble);
    uint32_t dt = micros() - t0;
    Serial.print(F("[cmd] t id="));
    Serial.print(a);
    Serial.print(F(" min="));
    Serial.print(minutes);
    Serial.print(F(" seq="));
    Serial.print(cmdSeq);
    Serial.print(F(" pre="));
    Serial.print(preamble);
    Serial.print(F(" air="));
    Serial.print((float)dt / 1000.0f, 1);
    Serial.println(ok ? F("ms ok") : F("ms TIMEOUT"));
    return;
  }

  if (strcmp(verb, "a") == 0) {
    // 'a' with no number disarms, which is why the presence of the argument is
    // tracked separately: 0 is a perfectly good collar id.
    if (!haveA || a < 0 || a >= MAX_COLLARS) {
      autoId = -1;
      autoLeft = 0;
      Serial.println(F("[cmd] auto-reply OFF"));
      return;
    }
    autoId = (int8_t)a;
    autoLeft = (b > 0 && b < 256) ? (uint8_t)b : 5;
    autoMinutes = 15;
    Serial.print(F("[cmd] auto-reply ON for id "));
    Serial.print((int)autoId);
    Serial.print(F(", "));
    Serial.print((int)autoLeft);
    Serial.println(F(" pings. 'a' alone disarms."));
    return;
  }

  Serial.println(F("[cmd] unknown - 'find <id> [min]', 'stop <id>', 't <id> [min] [pre]', 'a <id> [n]', 'v'"));
}

void printBanner() {
  Serial.println();
  Serial.println(F("====================================="));
  Serial.println(F(" Cat Tracker - Handheld + BLE"));
  Serial.println(F(" ESP32-C3 + RA-02 (SX1278 433MHz)"));
  Serial.println(F("====================================="));
  Serial.print(F("[radio] online  "));
  Serial.print(FREQ_MHZ, 1);
  Serial.print(F(" MHz  SF"));
  Serial.print(SPREADING);
  Serial.println(F("  sync=0x12"));
  Serial.print(F("Tracking up to "));
  Serial.print(MAX_COLLARS);
  Serial.println(F(" collars"));
  Serial.println(F(""));
  Serial.println(F("BLE diagnostics:"));
  Serial.println(F("  [ble] phone CONNECTED   -> phone opened the device"));
  Serial.println(F("  [ble] subscribe event    -> notifications switched on"));
  Serial.println(F("  [ble] NO SUBSCRIBER      -> notifications attempted, phone not subscribed"));
  Serial.println(F(""));
  Serial.println(F("Summary prints every 5s while a phone is attached. 'v' toggles."));
  Serial.println();
}

// One line per collar: where it is, how often it is heard, and how many of
// its packets never arrived.
void printCollars() {
  Serial.println(F("Collars:"));
  for (uint8_t i = 0; i < MAX_COLLARS; i++) {
    CollarState& c = collars[i];
    Serial.print(F("  id="));
    Serial.print(i);
    if (!c.have) {
      Serial.println(F("  (never heard)"));
      continue;
    }
    Serial.print(F("  rssi "));
    Serial.print(c.rssiLast, 1);
    Serial.print(F(" dBm  snr "));
    Serial.print(c.snrLast, 1);
    Serial.print(F(" dB  packets "));
    Serial.print(c.packets);
    Serial.print(F("  missed "));
    Serial.print(c.missed);
    Serial.print(F("  age "));
    Serial.print((millis() - c.lastMs) / 1000);
    Serial.print(F("s  batt "));
    if (c.batteryPct == BATT_UNKNOWN) {
      Serial.print(F("--"));
    } else {
      Serial.print(c.batteryPct);
      Serial.print(F("%"));
    }
    if (findRequest[i] == FIND_REQ_START) {
      Serial.print(F("  find: requested"));
    } else if (findRequest[i] == FIND_REQ_STOP) {
      Serial.print(F("  find: stop requested"));
    } else if (c.findActive) {
      Serial.print(F("  find: active "));
      Serial.print(c.findMinutes);
      Serial.print(F("min"));
    }
    Serial.println();
  }
  if (unknownIdCount) {
    Serial.print(F("  unknown id packets dropped: "));
    Serial.println(unknownIdCount);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(8, OUTPUT);        // onboard LED, heartbeat
  digitalWrite(8, HIGH);     // active low

  // No banner here on purpose - see verboseLog. Nothing is printed until 'v'
  // is sent, so powering the board from a charger can never wedge the loop.
  if (!initRadio()) while (true) delay(1000);

  setupBLE();

  // Priority 1, same as loopTask. The stack only has to hold a 20-byte buffer
  // plus the NimBLE call, so 3 KB is ample.
  xTaskCreate(publishKeepaliveTask, "keepalive", 3072, NULL, 1, NULL);
}

// Print a one-line BLE summary so the state is visible without verbose spam.
void printBleSummary() {
  if (!phoneConnected) return;
  Serial.print(F("[ble] connected=1 subscribed="));
  Serial.print(phoneSubscribed ? 1 : 0);
  Serial.print(F(" notify_ok="));
  Serial.print(notifyOk);
  Serial.print(F(" notify_fail="));
  Serial.print(notifyFail);
  Serial.print(F(" last_pub_age="));
  Serial.print(millis() - lastPublishMs);
  Serial.println(F("ms"));
}

void loop() {
  // Heartbeat blink. Tracked as state rather than digitalRead() on an output
  // pin, which returns a constant and made the buzzer test look steady.
  static bool ledOn = false;
  ledOn = !ledOn;
  digitalWrite(8, ledOn ? LOW : HIGH);

  // Serial commands are read even with logging off, so a host can still turn
  // diagnostics on over a USB cable that is otherwise only supplying power.
  //
  // 'v' is a single keypress (unchanged), and lines are ended either by a
  // newline or by a 150 ms gap - see the note above handleSerialLine().
  while (Serial.available()) {
    char c = (char)Serial.read();
    lastSerialByteMs = millis();
    if (c == '\r' || c == '\n') {
      if (cmdLineLen > 0) {
        cmdLine[cmdLineLen] = '\0';
        cmdLineLen = 0;
        handleSerialLine(cmdLine);
      }
    } else if (c == 'v' || c == 'V') {
      verboseLog = !verboseLog;
      if (verboseLog) {
        printBanner();
        printCollars();
      }
    } else if (cmdLineLen < CMD_LINE_MAX - 1) {
      cmdLine[cmdLineLen++] = c;
    }
  }
  if (cmdLineLen > 0 && millis() - lastSerialByteMs > 150) {
    cmdLine[cmdLineLen] = '\0';
    cmdLineLen = 0;
    handleSerialLine(cmdLine);
  }

  // -------------------------------------------------------------------------
  // Armed receive, polled - not radio.receive(data, len, timeout)
  // -------------------------------------------------------------------------
  // Two reasons this is not the old blocking call:
  //
  // 1. That three-argument receive() does not exist in any released RadioLib
  //    (checked 3.5 through 8.0: upstream offers receive(data, len) only), so
  //    this sketch could not be built from this tree at all.
  // 2. A blocking window is exactly what has to go once the collar slows down:
  //    at a 60 s ping the loop would sit inside receive() for a minute, unable
  //    to service a serial command or the LED.
  //
  // startReceive() arms RX continuous and returns. DIO0 is wired to this board
  // (PIN_DIO0) and RadioLib maps it to RxDone, so polling it costs one GPIO
  // read and adds about 1 ms of latency - well inside the collar's 100 ms
  // listen window. The deadline keeps the old RX_TIMEOUT_MS meaning: one slow
  // interval plus margin, so a ping landing on the edge is not missed.
  static bool     rxArmed    = false;
  static uint32_t rxDeadline = 0;

  if (!rxArmed) {
    int16_t armed = radio.startReceive();
    if (armed == RADIOLIB_ERR_NONE) {
      rxArmed = true;
      rxDeadline = millis() + RX_TIMEOUT_MS;
    } else if (verboseLog) {
      Serial.print(F("[!] startReceive failed "));
      Serial.println(armed);
    }
  }

  bool gotPacket = false;
  if (rxArmed) {
    if (digitalRead(PIN_DIO0) == HIGH) {
      gotPacket = true;
      rxArmed = false;
    } else if ((int32_t)(millis() - rxDeadline) >= 0) {
      rxArmed = false;
      // Let the phone know we are still alive but the signal has gone stale.
      publishRoster(-1);
    } else {
      // One millisecond is the whole cost of this loop: the CPU stays in idle
      // between polls instead of spinning, and the collar still has 99 ms of
      // its window left when the packet is in.
      delay(1);
    }
  }

  if (gotPacket) {
    // The instant the packet is complete. The collar's listen window opens at
    // the same moment (its TxDone), so everything between here and the command
    // going on air is spent out of that window.
    uint32_t rxReturnUs = micros();

    float rssi = radio.getRSSI();
    // Read SNR immediately after the RSSI, before anything else touches the
    // demodulator.
    float snr = radio.getSNR();

    // RadioLib does not report the payload length through receive(), but the
    // modem does know it - and without the length the tail of the buffer
    // still holds bytes from an earlier, longer packet. Against the armed
    // receive the order matters: getPacketLength() BEFORE readData(), because
    // readData() clears the register once it has consumed the packet.
    size_t len = radio.getPacketLength();
    if (len > sizeof(rxBuf)) len = sizeof(rxBuf);

    int16_t readState = radio.readData(rxBuf, len);
    if (readState != RADIOLIB_ERR_NONE) {
      if (verboseLog) {
        Serial.print(F("[!] readData error "));
        Serial.println(readState);
      }
      len = 0;   // a CRC failure must not be parsed as a cat
    }

    int8_t id;
    portENTER_CRITICAL(&stateMux);
    id = applyPacket(rxBuf, len, rssi, snr);
    portEXIT_CRITICAL(&stateMux);

    // ---- Phase 0a / Phase 1: answer the ping as fast as possible ----
    //
    // This deliberately sits BEFORE publishRoster(), the LED and the packet
    // log. A BLE notify is the most expensive thing in this loop and the
    // collar is listening with a 100 ms window; the ordering is the whole
    // point of the measurement, so do not "tidy" it downwards.
    if (id >= 0 && autoId == id && autoLeft > 0) {
      uint32_t setupUs = micros() - rxReturnUs;
      autoLeft--;
      bool ok = sendCommand((uint8_t)id, CMD_FIND_START, autoMinutes);
      uint32_t totalUs = micros() - rxReturnUs;

      // Printed unconditionally: a reply to a typed command cannot wedge a
      // board nobody is listening to, because nobody typed it.
      Serial.print(F("[auto] id="));
      Serial.print((int)id);
      Serial.print(F(" setup="));
      Serial.print((float)setupUs / 1000.0f, 2);
      // to_txdone, not to_key: RadioLib's transmit() returns at TxDone, so this
      // is packet-complete to *end* of the command. Subtract the command's own
      // airtime (238.6 ms with a 32-symbol preamble at SF9) to get the key-up
      // delay, which is the number the collar's listen window has to cover.
      Serial.print(F("ms to_txdone="));
      Serial.print((float)totalUs / 1000.0f, 2);
      Serial.print(F("ms seq="));
      Serial.print(cmdSeq);
      Serial.print(F(" left="));
      Serial.print((int)autoLeft);
      Serial.println(ok ? F(" ok") : F(" TIMEOUT"));
    }

    // ---- Find mode: deliver the request, and take the ack from the ping ----
    // A ping that agrees with the request is the acknowledgement: FIND_ACTIVE
    // set for a start, clear for a stop. Retry on up to 3 pings, then give up
    // and say so, because "silently never worked" is the worst failure mode
    // here - the owner is out looking for a cat.
    // The request state is written by the NimBLE task (a phone write) and read
    // here, so take a copy under the mutex and do the transmit outside it - a
    // transmit takes ~144 ms and holding the lock that long would block the
    // BLE callback.
    uint8_t req = FIND_REQ_NONE;
    uint8_t reqMinutes = 15;
    uint8_t reqTries = 0;
    if (id >= 0) {
      portENTER_CRITICAL(&stateMux);
      req = findRequest[id];
      reqMinutes = findMinutes[id];
      reqTries = findTries[id];
      portEXIT_CRITICAL(&stateMux);
    }

    if (id >= 0 && req != FIND_REQ_NONE) {
      const bool wantActive = (req == FIND_REQ_START);
      if (collars[id].findActive == wantActive) {
        portENTER_CRITICAL(&stateMux);
        findRequest[id] = FIND_REQ_NONE;
        findTries[id] = 0;
        portEXIT_CRITICAL(&stateMux);
        findAcked++;
        Serial.print(F("[find] acked id="));
        Serial.print((int)id);
        Serial.print(wantActive ? F(" active, ") : F(" stopped, "));
        Serial.print((int)collars[id].findMinutes);
        Serial.println(F(" min left"));
      } else if (reqTries >= 3) {
        portENTER_CRITICAL(&stateMux);
        findRequest[id] = FIND_REQ_NONE;
        findTries[id] = 0;
        portEXIT_CRITICAL(&stateMux);
        findFailed++;
        Serial.print(F("[find] FAILED id="));
        Serial.print((int)id);
        Serial.println(F(" after 3 attempts"));
      } else {
        bool ok = sendCommand((uint8_t)id,
                              wantActive ? CMD_FIND_START : CMD_FIND_STOP,
                              wantActive ? reqMinutes : 0);
        portENTER_CRITICAL(&stateMux);
        findTries[id] = (uint8_t)(reqTries + 1);
        portEXIT_CRITICAL(&stateMux);
        findSent++;
        Serial.print(F("[find] sent id="));
        Serial.print((int)id);
        Serial.print(wantActive ? F(" START ") : F(" STOP "));
        Serial.print((int)(reqTries + 1));
        Serial.print(F("/3 "));
        Serial.println(ok ? F("ok") : F("TIMEOUT"));
      }
    }

    if (id < 0) {
      // Not a packet we understand. Not an error worth shouting about: the
      // sync word already keeps other traffic off this frequency.
    } else {
      packetCount++;

      // Blink brighter when a packet lands, so lock is visible without serial.
      digitalWrite(8, LOW);
      ledOn = true;
      publishRoster(id);

      if (verboseLog) {
        CollarState& c = collars[id];
        Serial.print(F("[+] pkt #"));
        Serial.print(packetCount);
        Serial.print(F(" id="));
        Serial.print(id);
        Serial.print(F("  RSSI "));
        Serial.print(rssi, 1);
        Serial.print(F(" dBm  snr "));
        Serial.print(snr, 1);
        Serial.print(F(" dB"));
        if (c.batteryPct == BATT_UNKNOWN) {
          Serial.print(F("  batt --"));
        } else {
          Serial.print(F("  batt "));
          Serial.print(c.batteryPct);
          Serial.print(F("%"));
        }
        Serial.println();
      }
    }
  }

  // Periodic BLE state so we can see subscription status at a glance.
  static uint32_t lastSummary = 0;
  if (verboseLog && phoneConnected && millis() - lastSummary > 5000) {
    lastSummary = millis();
    printBleSummary();
    printCollars();
  }
}
