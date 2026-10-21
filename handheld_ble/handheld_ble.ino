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
const uint8_t BATT_UNKNOWN       = 0xFF;

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

// ---- Signal quality ----
// 8 s collar pings, so 12 packets is ~96 s of history. Measured noise floor is
// only ~0.7 dB, so this smooths multipath spikes; at this interval the bar
// reacts a little more slowly than it did at 5 s, which is the cost of the
// longer beacon period.
const uint8_t SMOOTH_N = 12;
// Must comfortably exceed one collar ping interval, or a couple of missed
// packets trip "signal lost" while the collar is perfectly fine.
const uint32_t STALE_MS = 15000;

// The collar's transmit interval (PING_INTERVAL_MS in collar/collar.ino).
// RadioLib's receive() blocks until a preamble arrives or the timeout
// expires, so this window must cover a full ping period of the SLOWEST
// collar or roughly half of every ping is dropped. The extra margin stops a
// ping landing exactly on the window edge from being missed due to
// scheduling jitter.
//
// NOTE: these two boards are coupled - change one interval, change the other.
//
// Each collar pings on its own period, offset by its id (PING_PERIOD_STEP_MS
// in collar/collar.ino), so the slowest one is id MAX_COLLARS-1 and the
// window has to cover that, not the nominal interval.
const uint32_t COLLAR_PING_MS = 8000 + (MAX_COLLARS - 1) * 137;
const uint32_t RX_TIMEOUT_MS  = COLLAR_PING_MS + 200;

// ---- BLE ----
#define SERVICE_UUID "8e7a1b20-3f4d-4c8e-9a1f-2b6d5e4c3a21"
#define SIGNAL_UUID  "8e7a1b21-3f4d-4c8e-9a1f-2b6d5e4c3a21"
#define DEVICE_NAME  "CatTracker"

BLECharacteristic* signalChar = nullptr;

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
  uint8_t  flags;           // bit0 LOW_BATT, carried from the collar
  float    rssiHistory[SMOOTH_N];
  uint8_t  idx, count;
  float    rssiSmoothed;
  float    snrLast;         // SNR of this collar's last packet
  uint32_t lastMs;
  uint32_t packets;
  uint32_t missed;          // seq gaps: packets we never heard
};
CollarState collars[MAX_COLLARS];

// Guards the collar state and the roster build. The keep-alive task builds a
// roster too, so without this it could read a half-updated entry.
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t rxBuf[64];
uint32_t packetCount = 0;
uint32_t unknownIdCount = 0;

void pushRssi(CollarState& c, float rssi) {
  c.rssiHistory[c.idx] = rssi;
  c.idx = (c.idx + 1) % SMOOTH_N;
  if (c.count < SMOOTH_N) c.count++;
  float sum = 0.0;
  for (uint8_t i = 0; i < c.count; i++) sum += c.rssiHistory[i];
  c.rssiSmoothed = sum / c.count;
}

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
  c.snrLast = snr;
  pushRssi(c, rssi);
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
    }

    *p++ = flags;

    int16_t r10 = (int16_t)(c.have ? c.rssiSmoothed * 10.0f : 0.0f);
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
// ~8 s at a time. Android's BLE supervision timeout is ~7.2 s, so that gap
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
    Serial.print(c.rssiSmoothed, 1);
    Serial.print(F(" dBm  packets "));
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
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'v' || c == 'V') {
      verboseLog = !verboseLog;
      if (verboseLog) {
        printBanner();
        printCollars();
      }
    }
  }

  int16_t state = radio.receive(rxBuf, sizeof(rxBuf), RX_TIMEOUT_MS);

  if (state == RADIOLIB_ERR_NONE) {
    float rssi = radio.getRSSI();
    // Read SNR immediately after the RSSI, before anything else touches the
    // demodulator.
    float snr = radio.getSNR();

    // RadioLib does not report the payload length through receive(), but the
    // modem does know it - and without the length the tail of the buffer
    // still holds bytes from an earlier, longer packet.
    size_t len = radio.getPacketLength();
    if (len > sizeof(rxBuf)) len = sizeof(rxBuf);

    int8_t id;
    portENTER_CRITICAL(&stateMux);
    id = applyPacket(rxBuf, len, rssi, snr);
    portEXIT_CRITICAL(&stateMux);

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
        Serial.print(F(" dBm  smoothed "));
        Serial.print(c.rssiSmoothed, 1);
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
  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    // Let the phone know we are still alive but the signal has gone stale.
    publishRoster(-1);
  } else {
    if (verboseLog) {
      Serial.print(F("[!] receive error "));
      Serial.println(state);
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
