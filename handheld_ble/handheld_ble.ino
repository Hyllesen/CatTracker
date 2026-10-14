// Cat Tracker - handheld LoRa receiver with BLE signal broadcasting.
//
// Step 1 of moving the homing UI to the phone. The handheld does what it
// already does (receive LoRa pings from the collar) and additionally runs a
// BLE GATT server that notifies the phone with the signal strength, so the
// phone can render the tone and the signal bar.
//
// Air settings below are the ones proven to interoperate with the collar; see
// handheld/README.md for why modem registers must be written field-by-field.

#include <Arduino.h>
#include <RadioLib.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

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

// ---- Signal quality ----
const float RSSI_CEIL  = -45.0;   // measured: boards ~3 m apart, strong
// Field testing decoded packets down to -140 dBm before the walk was turned
// back, so the old -75 floor mapped the entire far field to 0% and therefore
// to silence - exactly the range where guidance matters most.
//
// This is a conservative stand-in for a floor we have not actually measured.
// The arithmetic limit is knowable though: at <868 MHz RadioLib reports
// RSSI = -164 + PktRssiValue, then adds a negative SNR to correct for LoRa
// decoding below the noise floor. PktRssiValue is 8-bit unsigned and SNR is
// worth about -32 dB at worst, so nothing below roughly -196 dBm can be
// produced. Tighten this once a walk log shows where decoding really stops.
const float RSSI_FLOOR = -180.0;
const uint8_t SMOOTH_N = 12;
// Must comfortably exceed one collar ping interval, or a couple of missed
// packets trip "signal lost" while the collar is perfectly fine.
const uint32_t STALE_MS = 15000;

// The collar's transmit interval (PING_INTERVAL_MS in collar/collar.ino).
// RadioLib's receive() blocks until a preamble arrives or the timeout expires,
// so this window must cover a full ping period or roughly half of every ping is
// dropped. The extra margin stops a ping landing exactly on the window edge
// from being missed due to scheduling jitter.
//
// NOTE: these two boards are coupled - change one interval, change the other.
const uint32_t COLLAR_PING_MS = 5000;
const uint32_t RX_TIMEOUT_MS  = COLLAR_PING_MS + 200;

// ---- BLE ----
#define SERVICE_UUID "8e7a1b20-3f4d-4c8e-9a1f-2b6d5e4c3a21"
#define SIGNAL_UUID  "8e7a1b21-3f4d-4c8e-9a1f-2b6d5e4c3a21"
#define DEVICE_NAME  "CatTracker"

// Signal packet, 8 bytes, little-endian. One notification per update so the
// phone has everything it needs to draw the UI and pick a tone.
//   [0]     flags  bit0 = fresh packet, bit1 = stale (>8s since last),
//                   bit2 = at least one packet has been heard
//   [1..2]  RSSI * 10, int16 dBm - real reading whenever bit2 is set
//   [3..4]  SNR  * 10, int16 dB
//   [5]     signal strength 0..100
//   [6]     seconds since last packet (capped 255)
//   [7]     collar battery, 255 = unknown
#define SIGNAL_PKT_LEN 8
#define FLAG_FRESH 0x01
#define FLAG_STALE 0x02
#define FLAG_HAVE  0x04   // "never heard the collar" is flagged, not a value

BLECharacteristic* signalChar = nullptr;

// Set true with 'v' on the serial port when debugging.
//
// MUST default to false. Serial is native USB CDC, and when the board is
// powered from a power bank or wall charger nothing ever reads that port. The
// CDC buffer fills, Serial.print() blocks, and the whole loop stops - taking
// LoRa receive and BLE notifications down with it. This was observed stalling
// the loop for 52 s while packets kept arriving unheard.
static bool verboseLog = false;

float rssiHistory[SMOOTH_N];
uint8_t rssiIdx = 0, rssiCount = 0;
float rssiSmoothed = 0.0;
uint8_t rxBuf[64];

// Battery percent parsed out of the collar payload, e.g. "CAT-01;bat=87".
// 0xFF means unknown: the collar reports "--" until its ADC pin is wired.
uint8_t batteryPct = 0xFF;

void parseBattery(const char* payload) {
  const char* p = strstr(payload, "bat=");
  if (!p) { batteryPct = 0xFF; return; }
  p += 4;
  if (*p < '0' || *p > '9') { batteryPct = 0xFF; return; }  // "--"
  int v = 0;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    if (v > 100) { v = 100; break; }
    p++;
  }
  batteryPct = (uint8_t)v;
}

uint32_t lastPacketMs = 0;
bool everHeard = false;
uint32_t packetCount = 0;
// SNR of the last received packet, captured alongside the RSSI. getSNR() must
// be read while the packet is still current - it decodes the demodulator's
// last result, which the next receive() overwrites.
float snrLast = 0.0;

uint8_t rssiToPercent(float rssi) {
  if (rssi <= RSSI_FLOOR) return 0;
  if (rssi >= RSSI_CEIL) return 100;
  return (uint8_t)((rssi - RSSI_FLOOR) * 100.0 / (RSSI_CEIL - RSSI_FLOOR));
}

void pushRssi(float rssi) {
  rssiHistory[rssiIdx] = rssi;
  rssiIdx = (rssiIdx + 1) % SMOOTH_N;
  if (rssiCount < SMOOTH_N) rssiCount++;
  float sum = 0.0;
  for (uint8_t i = 0; i < rssiCount; i++) sum += rssiHistory[i];
  rssiSmoothed = sum / rssiCount;
}

// ---- BLE diagnostics state ----
// Declared ahead of publishSignal() so the publish path can report it. NimBLE
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

// Build and push the current signal state to any subscribed phone.
void publishSignal(bool fresh) {
  uint8_t pkt[SIGNAL_PKT_LEN];
  bool have = (rssiCount > 0);
  bool stale = everHeard && (millis() - lastPacketMs) > STALE_MS;
  lastPublishMs = millis();

  pkt[0] = (fresh ? FLAG_FRESH : 0x00) | (stale ? FLAG_STALE : 0x00)
         | (have ? FLAG_HAVE : 0x00);
  // No sentinel RSSI value. "Never heard the collar" is carried by FLAG_HAVE,
  // so [1..2] is always a real reading. This matters because the old sentinel
  // was -127 dBm: with the floor now at -180, every genuine reading between
  // -127 and -180 would have been decoded by the phone as "no data" and gone
  // silent - the bug this replaces.
  int16_t r10 = (int16_t)((have ? rssiSmoothed : 0.0f) * 10.0f);
  int16_t s10 = (int16_t)(snrLast * 10.0f);
  pkt[1] = (uint8_t)(r10 & 0xFF);
  pkt[2] = (uint8_t)(r10 >> 8);
  pkt[3] = (uint8_t)(s10 & 0xFF);
  pkt[4] = (uint8_t)(s10 >> 8);
  pkt[5] = have ? rssiToPercent(rssiSmoothed) : 0;
  uint32_t ageS = have ? ((millis() - lastPacketMs) / 1000) : 0;
  pkt[6] = (ageS > 255) ? 255 : (uint8_t)ageS;
  pkt[7] = batteryPct;

  if (VERBOSE_PUBLISH) {
    Serial.print(F("  [publish] fresh="));
    Serial.print(fresh);
    Serial.print(F(" rssi*10="));
    Serial.print(pkt[1] | (pkt[2] << 8));
    Serial.print(F(" pct="));
    Serial.print(pkt[5]);
    Serial.print(F(" sub="));
    Serial.println(phoneSubscribed ? 1 : 0);
  }

  if (signalChar) {
    signalChar->setValue(pkt, SIGNAL_PKT_LEN);
    signalChar->notify();
  }
}

const char* statusName(uint32_t s) {
  switch (s) {
    case BLECharacteristicCallbacks::Status::SUCCESS_INDICATE: return "indicate ok";
    case BLECharacteristicCallbacks::Status::SUCCESS_NOTIFY:  return "notify ok";
    case BLECharacteristicCallbacks::Status::ERROR_INDICATE_TIMEOUT: return "indicate TIMEOUT";
    case BLECharacteristicCallbacks::Status::ERROR_INDICATE_FAILURE:  return "indicate FAILED";
    case BLECharacteristicCallbacks::Status::ERROR_INDICATE_DISABLED: return "indications not enabled";
    case BLECharacteristicCallbacks::Status::ERROR_NOTIFY_DISABLED:   return "notifications not enabled";
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
  Serial.println(F("====================================="));
  Serial.print(F("[radio] online  "));
  Serial.print(FREQ_MHZ, 1);
  Serial.print(F(" MHz  SF"));
  Serial.print(SPREADING);
  Serial.println(F("  sync=0x12"));
  Serial.println(F(""));
  Serial.println(F("BLE diagnostics:"));
  Serial.println(F("  [ble] phone CONNECTED   -> phone opened the device"));
  Serial.println(F("  [ble] subscribe event    -> notifications switched on"));
  Serial.println(F("  [ble] NO SUBSCRIBER      -> notifications attempted, phone not subscribed"));
  Serial.println(F(""));
  Serial.println(F("Summary prints every 5s while a phone is attached. 'v' toggles."));
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  pinMode(8, OUTPUT);        // onboard LED, heartbeat
  digitalWrite(8, HIGH);     // active low

  // No banner here on purpose - see verboseLog. Nothing is printed until 'v'
  // is sent, so powering the board from a charger can never wedge the loop.
  if (!initRadio()) while (true) delay(1000);

  setupBLE();
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
      }
    }
  }

  int16_t state = radio.receive(rxBuf, sizeof(rxBuf), RX_TIMEOUT_MS);

  if (state == RADIOLIB_ERR_NONE) {
    float rssi = radio.getRSSI();
    // Read SNR immediately after the RSSI, before anything else touches the
    // demodulator.
    snrLast = radio.getSNR();

    // The payload is ASCII ("CAT-01;bat=87") but RadioLib neither reports its
    // length nor terminates it, so stale FIFO bytes follow it. Scan to the
    // first non-printable byte and terminate there.
    size_t n = 0;
    while (n < sizeof(rxBuf) && rxBuf[n] >= 0x20 && rxBuf[n] < 0x7F) n++;
    rxBuf[n] = '\0';
    parseBattery((const char*)rxBuf);

    pushRssi(rssi);
    lastPacketMs = millis();
    everHeard = true;
    packetCount++;

    // Blink brighter when a packet lands, so lock is visible without serial.
    digitalWrite(8, LOW);
    ledOn = true;
    publishSignal(true);
    if (verboseLog) {
      Serial.print(F("[+] pkt #"));
      Serial.print(packetCount);
      Serial.print(F("  RSSI "));
      Serial.print(rssi, 1);
      Serial.print(F(" dBm  smoothed "));
      Serial.print(rssiSmoothed, 1);
      Serial.print(F(" dBm  snr "));
      Serial.print(snrLast, 1);
      Serial.print(F(" dB  signal "));
      Serial.print(rssiToPercent(rssiSmoothed));
      Serial.println(F("%"));
    }
  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    // Let the phone know we are still alive but the signal has gone stale.
    if (everHeard && (millis() - lastPacketMs) > STALE_MS) {
      everHeard = false;
      if (verboseLog) {
        Serial.println(F("[-] signal stale"));
      }
    }
    publishSignal(false);
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
  }
}