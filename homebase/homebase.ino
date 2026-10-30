// Cat Tracker - homebase unit: LoRa receiver, WiFi, MQTT.
//
// A BASE STATION: a fixed, powered LoRa receiver that reports to the house. It is
// not the walk device. It has no BLE, so the phone app does not talk to it - the
// phone talks to the handheld. What a base station buys is placement: it sits high
// and powered near where the cats go, and it never moves, which is what makes
// "Chichi has not been heard in four hours" possible at all.
//
// Why a separate sketch, and not the handheld plus WiFi:
//
//   The handheld carries the BLE stack, and adding WiFi + MQTT to it took the
//   build to 99% of the ESP32-C3 app partition (1,308,309 of 1,310,720 bytes). A
//   base station has no BLE, so it has room.
//
//   The two jobs are not the same hour. The walk device is carried into the
//   jungle to find a cat; the base station is bolted to a shelf. One board cannot
//   be in both places.
//
// ESP8266, not ESP32-C3, on purpose: the ESP8266 has no BLE and no USB CDC, which
// is exactly the set of things a base station does not need. It is cheaper, the
// house already has one, and its WiFi is proven in this house.
//
// The one thing ESP8266 cannot do is the CC1101. The existing ESP8266 in this
// house runs a CC1101 at 433.92 MHz. CC1101 is FSK/GFSK and has no LoRa mode, so
// it cannot receive a collar. This board needs an SX1278 (RA-02) on the same
// 433 MHz band the collars use.

#include <Arduino.h>
#include <RadioLib.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// The base station id and the cat names are baked in at flash time, the same way
// the collar id is, so Home Assistant's discovery creates devices called Chichi,
// Onanda and Chacha rather than Cat 0, Cat 1, Cat 2.
//
//   --build-property "compiler.cpp.extra_flags=-DBASE_ID=base1 -DCAT_NAMES=Chichi|Onanda|Chacha"
#define XSTR(x) #x
#define STR(x) XSTR(x)
#ifndef BASE_ID
#define BASE_ID base1
#endif
#ifndef CAT_NAMES
#define CAT_NAMES Chichi|Onanda|Chacha
#endif

// ---------------------------------------------------------------------------
// Radio hardware: ESP8266 + RA-02 (SX1278)
// ---------------------------------------------------------------------------
// ESP8266 Arduino's SPI is HSPI: SCK = GPIO14 (D5), MISO = GPIO12 (D6),
// MOSI = GPIO13 (D7). NSS is the HSPI chip select, GPIO15 (D8).
//
// D6 is GPIO12 and D7 is GPIO13 - the silkscreen labels are NOT in GPIO order.
// Wiring by the label alone crosses MOSI and MISO, and a crossed pair is silent:
// every SPI read returns 0xFF, which is the same reading as a dead module.
//
// DIO0 must be an interrupt-capable pin. GPIO16 (D0) is not - it is the only pin
// with no interrupt support on the ESP8266, and it is the pin most wiring guides
// suggest for DIO0, which produces a board that receives nothing at all.
const uint8_t PIN_NSS  = 15;   // D8
const uint8_t PIN_DIO0 = 4;    // D2
// RST on D4/GPIO2, which on a NodeMCU V3 also carries the onboard blue LED
// (LED_BUILTIN is 2 in the nodemcuv2 variant). The SX1278 RESET pad is a
// high-impedance input with its own pull-up, so the LED does not hold the module
// in reset - it just lights while reset is pulsed. D1/GPIO5 is the clean
// alternative if that matters: set this to 5 and move the one wire.
const uint8_t PIN_RST  = 2;    // D4

Module* mod = new Module(PIN_NSS, PIN_DIO0, PIN_RST);
SX1278 radio = mod;

// ---- LoRa air settings - MUST match the collar ----
const float   FREQ_MHZ   = 433.0;
const float   BANDWIDTH  = 125.0;
const uint8_t SPREADING  = 9;
const uint8_t CODING_R   = 7;
const uint8_t SYNC_WORD  = 0x12;
const int8_t  TX_POWER   = 17;
const uint16_t PREAMBLE  = 8;

const uint8_t MAX_COLLARS = 3;

// 8 s collar pings, so 12 packets is ~96 s of history. Measured noise floor is
// only ~0.7 dB, so this smooths multipath spikes.
const uint8_t SMOOTH_N = 12;

// ---- Collar packet format - MUST match collar/collar.ino ----
const uint8_t FRAME_MAGIC = 0xC1;
const uint8_t FRAME_LEN   = 6;
const uint8_t FRAME_FLAG_LOW_BATT = 0x01;
const uint8_t BATT_UNKNOWN = 0xFF;

const uint8_t ROSTER_FLAG_HAVE     = 0x01;
const uint8_t ROSTER_FLAG_FRESH    = 0x02;
const uint8_t ROSTER_FLAG_STALE    = 0x04;
const uint8_t ROSTER_FLAG_LOW_BATT = 0x08;

// Must comfortably exceed one collar ping interval, or a couple of missed packets
// trip "signal lost" while the collar is fine.
const uint32_t STALE_MS = 15000;
const uint32_t COLLAR_PING_MS = 8000 + (MAX_COLLARS - 1) * 137;
const uint32_t RX_TIMEOUT_MS  = COLLAR_PING_MS + 200;

// ---- House link ----
const char* WIFI_SSID = "HUAWEI-2.4G-7Pdq";
const char* WIFI_PASS = "KqWGaq8v";
const char* MQTT_BROKER = "192.168.100.66";   // m710q, wired
const uint16_t MQTT_PORT = 1883;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

char topicAvail[48];
char topicState[48];

bool mqttUp = false;
bool verboseLog = false;   // 'v' on the serial port. Off by default: a base
                           // station on a USB brick has nothing reading UART0.
uint32_t lastWifiTry = 0, lastMqttTry = 0, lastPublish = 0;

// The publish cadence. radio.receive() blocks for RX_TIMEOUT_MS, so the timer
// fires once per receive window: a beacon publishes immediately, and a quiet
// window publishes once so the ages keep counting up on Home Assistant. A
// replayed state would freeze a cat's age at whatever it was when the last
// packet arrived, and the house would show a cat as "3 s ago" for as long as it
// was actually missing.
const uint32_t PUBLISH_MS = 2000;

// ---------------------------------------------------------------------------
// Per-collar state
// ---------------------------------------------------------------------------
// The array index IS the collar id, so there is no map and no lookup.
struct CollarState {
  bool     have;
  uint8_t  id;
  uint8_t  seq;
  bool     seqValid;
  uint8_t  batteryPct;
  uint8_t  flags;
  float    rssiHistory[SMOOTH_N];
  uint8_t  idx, count;
  float    rssiSmoothed;
  float    snrLast;
  uint32_t lastMs;
  uint32_t packets;
  uint32_t missed;
};
CollarState collars[MAX_COLLARS];

uint8_t rxBuf[64];
uint32_t packetCount = 0;
uint32_t unknownIdCount = 0;

void pushRssi(CollarState& c, float rssi) {
  // Running mean over the last SMOOTH_N samples, kept incrementally: adding the
  // new value and subtracting the old one is one divide, not a loop over the
  // history, which matters on an 80 MHz single-core part.
  if (c.count < SMOOTH_N) {
    c.count++;
    c.rssiSmoothed += (rssi - c.rssiSmoothed) / c.count;
  } else {
    c.rssiSmoothed += (rssi - c.rssiHistory[c.idx]) / SMOOTH_N;
  }
  c.rssiHistory[c.idx] = rssi;
  c.idx = (c.idx + 1) % SMOOTH_N;
}

// Apply one received packet. Returns the collar id, or -1 if unusable.
int8_t applyPacket(const uint8_t* buf, size_t len, float rssi, float snr) {
  if (len < FRAME_LEN || buf[0] != FRAME_MAGIC) {
    unknownIdCount++;   // counted, not silently folded into a collar's row
    return -1;
  }
  uint8_t id = buf[2];
  if (id >= MAX_COLLARS) {
    unknownIdCount++;
    return -1;
  }

  CollarState& c = collars[id];
  if (!c.have) {
    c.have = true;
    c.id = id;
    c.seqValid = false;
  }
  uint8_t seq = buf[1];
  if (c.seqValid) {
    c.missed += (uint8_t)(seq - c.seq - 1);
  }
  c.seq = seq;
  c.seqValid = true;

  c.batteryPct = buf[3];
  c.flags = buf[4];
  c.snrLast = snr;
  pushRssi(c, rssi);
  c.lastMs = millis();
  c.packets++;
  return (int8_t)id;
}

// The name for a collar slot, from the flash-time list.
char* catName(uint8_t i) {
  static char buf[24];
  const char* p = STR(CAT_NAMES);
  for (uint8_t k = 0; k < i; k++) {
    p = strchr(p, '|');
    if (!p) return nullptr;
    p++;
  }
  size_t n = 0;
  while (p[n] && p[n] != '|' && n < sizeof(buf) - 1) { buf[n] = p[n]; n++; }
  buf[n] = '\0';
  return buf;
}

// ---------------------------------------------------------------------------
// Home Assistant discovery
// ---------------------------------------------------------------------------
// One device per cat, one for the base station. Published retained, once per MQTT
// connect, so a broker restart re-reads it.
//
// Every optional field is written only when it is set: a null passed to %s prints
// "(null)", which is valid C and invalid JSON, and a discovery payload Home
// Assistant cannot parse means the entity never appears - silently, with no error
// on the device.
void publishSensorConfig(const char* uid, const char* nm, const char* unit,
                         const char* dclass, const char* tmpl,
                         const char* devIdent, const char* devName, bool binary) {
  // The longest record measured is ~342 bytes, so 340 truncated it and the
  // truncated JSON was published anyway. Home Assistant logged "Unable to parse
  // JSON" for every cat and created no entities - silently, on the device.
  char topic[96], json[420];
  snprintf(topic, sizeof(topic), "homeassistant/%s/%s_%s/config",
           binary ? "binary_sensor" : "sensor", STR(BASE_ID), uid);
  int n = snprintf(json, sizeof(json),
      "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",", nm, STR(BASE_ID), uid);

  // snprintf returns the length it WOULD have written. If a segment overflows, n
  // runs past the buffer and sizeof(json) - n goes negative, which as a size_t is
  // a huge length. Clamp after every append, and drop the record rather than
  // publish broken JSON.
  #define APPEND(...) do {                                     \
      if (n < 0 || n >= (int)sizeof(json)) return;             \
      n += snprintf(json + n, sizeof(json) - n, __VA_ARGS__);  \
      if (n >= (int)sizeof(json)) return;                      \
    } while (0)

  if (unit && !binary) APPEND("\"unit_of_measurement\":\"%s\",", unit);
  if (dclass) APPEND("\"device_class\":\"%s\",", dclass);
  APPEND("\"state_topic\":\"%s\",\"value_template\":\"%s\","
         "\"availability_topic\":\"%s\","
         "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
         "\"manufacturer\":\"Cat Tracker\",\"model\":\"Homebase\"}}",
         topicState, tmpl, topicAvail, devIdent, devName);
  #undef APPEND

  mqtt.publish(topic, json, true);
}

void publishDiscovery() {
  // t holds the value_template. The longest one is the last-seen guard, which is
  // 78 characters, so 64 truncated it to 63 and Home Assistant got a record that
  // is valid JSON containing a half-written Jinja expression - which renders as
  // nothing, and an MQTT sensor with a template that renders nothing is
  // "unavailable". Silent on the device, and the entity looks like a dead cat.
  char ident[48], t[128], uid[24];
  for (uint8_t i = 0; i < MAX_COLLARS; i++) {
    char* nm = catName(i);
    if (!nm || !*nm) continue;
    snprintf(ident, sizeof(ident), "cattracker_%s_cat%u", STR(BASE_ID), (unsigned)i);

    // The second argument is the ENTITY name, the last is the DEVICE name. Passing
    // the cat name as both makes Home Assistant concatenate them: every sensor for
    // Chichi reads "Chichi Chichi". The entity name is what the sensor measures, so
    // Home Assistant renders "Chichi signal", "Chichi battery", and the entity ids
    // automations use come out as sensor.chichi_signal, sensor.chichi_battery.
    //
    // uid is passed to publishSensorConfig as a plain string, so it has to be
    // formatted here. Passing "cat%u_signal" leaves the literal "%u" in the topic
    // and the unique_id: all three cats publish to one topic, and because
    // discovery is retained the last cat wins and the other two never appear.
    snprintf(t, sizeof(t), "{{ value_json.c%u.rssi / 10 }}", (unsigned)i);
    snprintf(uid, sizeof(uid), "cat%u_signal", (unsigned)i);
    publishSensorConfig(uid, "signal", "dBm", "signal_strength", t, ident, nm, false);

    snprintf(t, sizeof(t), "{{ value_json.c%u.snr / 2 }}", (unsigned)i);
    snprintf(uid, sizeof(uid), "cat%u_snr", (unsigned)i);
    publishSensorConfig(uid, "SNR", "dB", nullptr, t, ident, nm, false);

    snprintf(t, sizeof(t),
             "{{ value_json.c%u.batt if value_json.c%u.batt < 255 else none }}",
             (unsigned)i, (unsigned)i);
    snprintf(uid, sizeof(uid), "cat%u_battery", (unsigned)i);
    publishSensorConfig(uid, "battery", "%", "battery", t, ident, nm, false);

    // A collar never heard has last = 0, and "0 | timestamp_local" renders as
    // 1970-01-01, which reads as "this cat was heard 56 years ago" rather than
    // "never". Map 0 to none so Home Assistant shows unknown.
    snprintf(t, sizeof(t),
             "{{ (value_json.c%u.last | timestamp_local) if value_json.c%u.last > 0 else none }}",
             (unsigned)i, (unsigned)i);
    snprintf(uid, sizeof(uid), "cat%u_last_seen", (unsigned)i);
    publishSensorConfig(uid, "last seen", nullptr, "timestamp", t, ident, nm, false);

    snprintf(t, sizeof(t), "{{ 'ON' if value_json.c%u.in == 1 else 'OFF' }}", (unsigned)i);
    snprintf(uid, sizeof(uid), "cat%u_in_range", (unsigned)i);
    publishSensorConfig(uid, "in range", nullptr, "connectivity", t, ident, nm, true);
  }

  snprintf(ident, sizeof(ident), "cattracker_%s", STR(BASE_ID));
  publishSensorConfig("packets", "Packets", nullptr, nullptr,
                      "{{ value_json.pk }}", ident, STR(BASE_ID), false);
  publishSensorConfig("unknown_id", "Unknown id drops", nullptr, nullptr,
                      "{{ value_json.unk }}", ident, STR(BASE_ID), false);
}

// The state as JSON. Integers, in the roster's scaling (dBm * 10, dB * 2), so the
// firmware does no float formatting - ESP8266's printf has no %f at all - and Home
// Assistant's value_template divides by 10 and 2.
//
// Retained, so Home Assistant sees the last known state on restart rather than
// showing every cat as unknown until the next beacon.
void publishState(uint8_t freshId) {
  char buf[420];
  time_t now = time(nullptr);
  bool synced = now > 1700000000;   // NTP has landed. Before it, last_seen is 1970.

  int n = snprintf(buf, sizeof(buf), "{\"t\":%lu,\"ts\":%ld,\"pk\":%lu,\"unk\":%lu",
      (unsigned long)millis(), (long)now,
      (unsigned long)packetCount, (unsigned long)unknownIdCount);

  for (uint8_t i = 0; i < MAX_COLLARS; i++) {
    CollarState& c = collars[i];
    uint32_t ageS = c.have ? ((millis() - c.lastMs) / 1000) : 0;
    bool in = c.have && (millis() - c.lastMs <= STALE_MS);
    long last = synced && c.have ? (long)(now - ageS) : 0;
    int16_t r10 = (int16_t)(c.have ? c.rssiSmoothed * 10.0f : 0.0f);
    int8_t s2 = (int8_t)lroundf(c.have ? c.snrLast * 2.0f : 0.0f);

    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"c%u\":{\"rssi\":%d,\"snr\":%d,\"batt\":%u,\"age\":%u,\"pk\":%lu,"
        "\"miss\":%lu,\"in\":%d,\"last\":%ld}",
        (unsigned)i, (int)r10, (int)s2, (unsigned)(c.have ? c.batteryPct : BATT_UNKNOWN),
        (unsigned)ageS, (unsigned long)c.packets, (unsigned long)c.missed,
        in ? 1 : 0, last);
    if (n >= (int)sizeof(buf) - 1) return;   // truncated: drop the publish
  }
  strncat(buf, "}", sizeof(buf) - strlen(buf) - 1);
  mqtt.publish(topicState, buf, true);
  lastPublish = millis();
}

// WiFi and MQTT are driven without blocking the radio loop.
void mqttPump() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 10000) {
      lastWifiTry = millis();
      WiFi.reconnect();
    }
    mqttUp = false;
    return;
  }

  if (!mqtt.connected()) {
    if (millis() - lastMqttTry < 5000) return;
    lastMqttTry = millis();
    // The last will is what makes Blind visible: if the base station loses power
    // or WiFi, the broker fires it and Home Assistant knows the house went blind.
    // A Missing alert then only ever means the cat, never the plumbing - which is
    // what stops the first notification being a false alarm about a power cut.
    if (mqtt.connect("cattracker-homebase", topicAvail, 1, true, "offline")) {
      mqttUp = true;
      mqtt.publish(topicAvail, "online", true);
      publishDiscovery();
    }
    return;
  }
  mqtt.loop();
}

bool initRadio() {
  int16_t state = radio.begin(FREQ_MHZ, BANDWIDTH, SPREADING, CODING_R,
                              SYNC_WORD, TX_POWER, PREAMBLE);
  return state == RADIOLIB_ERR_NONE;
}

void setup() {
  Serial.begin(115200);
  snprintf(topicAvail, sizeof(topicAvail), "cattracker/%s/availability", STR(BASE_ID));
  snprintf(topicState, sizeof(topicState), "cattracker/%s/state", STR(BASE_ID));
  // setServer is what makes MQTT_BROKER mean anything. PubSubClient's default
  // server is 0.0.0.0:1883, so without this line connect() fails forever and the
  // base station publishes nothing at all - silently, with no error on the device.
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(512);   // the state JSON is ~300 bytes; the default is 128

  if (!initRadio()) {
    Serial.print(F("[radio] init FAILED. Wiring: NSS=D8/GPIO15, DIO0=D2/GPIO4, "
                   "RST=D4/GPIO2, SPI=D5/D6/D7."));
    while (true) delay(1000);
  }

  WiFi.mode(WIFI_STA);
  // ESP8266 core: the call is WiFi.setSleep(false), not setPowerSave(). Modem
  // sleep must be off - the ESPHome configs in this house found generic ESP32-C3
  // boards do not associate reliably with it on, and the same is true here.
  WiFi.setSleep(false);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.println(F("Cat Tracker - homebase (ESP8266 + RA-02)"));
  Serial.print(F("base id ")); Serial.println(STR(BASE_ID));
  Serial.print(F("state topic ")); Serial.println(topicState);
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'v' || c == 'V') verboseLog = !verboseLog;
  }

  mqttPump();

  int16_t state = radio.receive(rxBuf, sizeof(rxBuf), RX_TIMEOUT_MS);

  if (state == RADIOLIB_ERR_NONE) {
    float rssi = radio.getRSSI();
    float snr = radio.getSNR();
    size_t len = radio.getPacketLength();
    if (len > sizeof(rxBuf)) len = sizeof(rxBuf);

    int8_t id = applyPacket(rxBuf, len, rssi, snr);
    if (id >= 0) {
      packetCount++;
      if (mqttUp) publishState((uint8_t)id);

      if (verboseLog) {
        Serial.print(F("[+] pkt #")); Serial.print(packetCount);
        Serial.print(F(" id=")); Serial.print(id);
        Serial.print(F(" RSSI ")); Serial.print(rssi, 1);
        Serial.print(F(" dBm snr ")); Serial.print(snr, 1);
        Serial.println(F(" dB"));
      }
    }
  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    // Quiet window: publish so the ages keep counting up.
    if (mqttUp) publishState(255);
  }

  // The publish timer also covers the case where receive() returns quickly.
  if (mqttUp && millis() - lastPublish > PUBLISH_MS) {
    publishState(255);
  }
}
