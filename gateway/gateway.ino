/*
 * gateway.ino — base3: the 915 MHz gateway that reports the cats to the house.
 *
 * A base station: fixed, powered, and never carried. It is the receive end of the
 * jungle hop. Two measurement surfaces, and the board has one, the other, or both:
 *
 *   -DROLE / HAS_433=1  an RA-02 (SX1278) on 433 MHz hears the collar pings
 *                       directly. This is published as **node 0**, the house's own
 *                       measurement point, in the same table as the relays.
 *   915 MHz always      the RAK3172 on UART2 hears the relay nodes' reports
 *                       (`CAT<n> <seq> c<id> s<collarseq> r<rssi>`), nodes 1..N.
 *
 * WiFi + MQTT + Home Assistant discovery, base id `base3`. `base1` is the ESP8266
 * inside the house and `cat-base2` is the ESPHome rooftop unit, and a second board
 * publishing as either of those ids overwrites the first on the broker, so `base3`
 * is not a preference, it is the only free name.
 *
 * Console is USB CDC. Every print is gated on `Serial.isPlugged()` — a board fed
 * from a solar charger has nothing reading the cable, an ungated write blocks, and
 * the board stops. See AGENTS.md.
 *
 * Keys: h help, v report, w network/MQTT, p module readback, r re-arm RX, x stop RX,
 * d dump the observation log, b bridge (type AT straight to the module).
 *
 * Credentials come from build flags, never from this file — see gateway/build.sh,
 * which reads them from the ESPHome config that already holds them.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>

#include "state_json.h"
#include "rak_event.h"

// Credentials come from build flags, never from this file. Three networks, in the
// order the house should be tried: the rooftop TP-Link first, HUAWEI (the route
// base1 uses to reach the broker) second, Stefan_Office third. An empty string is
// skipped, so a build with two networks works.
#ifndef WIFI_SSID
#error "WIFI_SSID must be passed as a build flag: -DWIFI_SSID=\"...\" (see gateway/build.sh)"
#endif
#ifndef WIFI_PASS
#error "WIFI_PASS must be passed as a build flag"
#endif
#ifndef WIFI_SSID2
#define WIFI_SSID2 ""
#endif
#ifndef WIFI_PASS2
#define WIFI_PASS2 ""
#endif
#ifndef WIFI_SSID3
#define WIFI_SSID3 ""
#endif
#ifndef WIFI_PASS3
#define WIFI_PASS3 ""
#endif

#define XSTR(x) #x
#define STR(x) XSTR(x)

#ifndef BASE_ID
#define BASE_ID base3
#endif
#ifndef CAT_NAMES
#define CAT_NAMES Chichi|Onanda|Chacha
#endif
// Node row labels. Index 0 is this base's own 433 receiver; the rest are the relay
// ids as they appear in `NODE_ID` on the node boards.
#ifndef NODE_NAMES
#define NODE_NAMES 433 radio|Node 1|Node 2|Node 3
#endif
#ifndef MQTT_BROKER
#define MQTT_BROKER "192.168.100.66"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif

// HAS_433 is 1 on a board with an RA-02 fitted. Without it there is no node 0, and
// a permanently-blank node-0 row would read as a relay that has gone blind.
#ifndef HAS_433
#define HAS_433 0
#endif

// Static address. .231 is cat-base2; do not reuse an address a base station already
// has, because Home Assistant's config entry stores it.
#ifndef STATIC_IP_LAST
#define STATIC_IP_LAST 232
#endif

// Base station's own cell. -1 = not wired. TP5000 CHRG/STDBY are open-drain and
// active low, so they need a pull-up: INPUT_PULLUP, and "charging" is a low read.
#ifndef BAT_ADC
#define BAT_ADC -1
#endif
#ifndef BAT_DIV
#define BAT_DIV 2
#endif
#ifndef CHRG_PIN
#define CHRG_PIN -1
#endif
#ifndef STDBY_PIN
#define STDBY_PIN -1
#endif

// ---------------------------------------------------------------------------
// 915 air settings — MUST match every node. One field off is silence with no
// error, indistinguishable from a dead module.
// ---------------------------------------------------------------------------
#ifndef FREQ_HZ
#define FREQ_HZ 915000000UL
#endif
#ifndef SF
#define SF 7
#endif
#ifndef BW
#define BW 125              // 125 kHz, and AT+P2P wants it as the CODE 0
#endif
#ifndef CR
#define CR 0               // 0 = 4/5
#endif
#ifndef PREAMBLE
#define PREAMBLE 8
#endif
#ifndef TX_DBM
#define TX_DBM 14
#endif

#define PIN_RAK_RX 20
#define PIN_RAK_TX 21

// RSSI smoothing: first sample lands directly, each later one moves the running
// value by 1/8 of the difference. base1's running mean over 12 packets has the same
// time constant, so the two base stations' "signal" rows move at the same rate.
#define SMOOTH_K 8

// Freshness. These are deliberately different, and the difference is the reason.
//
// Node 0 hears every collar ping, so a cat there is stale when one ping is missing.
// A relay reports ONE collar per report — whichever it heard most recently — so with
// three collars a given cat appears in roughly every third report, and a fresh
// 15 s window at a relay would flap with the report rotation.
#define CAT_STALE_OWN 15000UL
#define CAT_STALE_NODE 90000UL
// The node row is the plumbing. A relay heartbeats every ~64 s when it has heard
// nothing, so a node is Blind only when it has been silent past a heartbeat plus
// margin. This is what separates "the cat walked away" from "the node's battery
// died", and the alert has to be able to tell them apart.
#define NODE_STALE_MS 150000UL

// Publish cadence: 5 s, so ages keep counting while nothing arrives. A replayed
// snapshot would freeze a cat's age at whatever it was when the last packet landed.
#define PUBLISH_MS 5000UL
// How long one network gets before the next is tried. Association is a few seconds
// when the router is reachable, so a board in range of two networks spends at most
// one rotation on a network that is off.
#define ROTATE_MS 20000UL

// 34 is the core's code for 8.5 dBm (wifi_power_t). Overridable: -DTX_POWER=60 is
// 15 dBm. A board that associates at the default keeps the default; a board on a
// long cable in the sun may need more.
#ifndef TX_POWER
#define TX_POWER 34
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Point {
  ctgw::NodeRow row;
  uint32_t lastMs = 0;                         // last report from this point
  uint32_t catMs[ctgw::NUM_COLLARS] = {0};
  uint8_t catSeq[ctgw::NUM_COLLARS] = {0};
  bool catSeqValid[ctgw::NUM_COLLARS] = {false};
  long nodeSeq = -1;                           // the relay's own report counter
};

static Point pt[ctgw::NUM_NODES];

// The house has three 2.4 GHz networks, and a board only learns which one it is on
// by trying: "not associated" is the same output for a wrong passphrase, a router
// that is off, and a router that is out of range. The order is TP-Link (where the
// rooftop base lives), HUAWEI (the route base1 uses to reach the broker),
// Stefan_Office. Each is given ROTATE_MS before the next is tried.
struct Net { const char *ssid; const char *pass; };
static const Net NETS[] = { {WIFI_SSID, WIFI_PASS}, {WIFI_SSID2, WIFI_PASS2},
                            {WIFI_SSID3, WIFI_PASS3} };
static const int NET_N = sizeof(NETS) / sizeof(NETS[0]);
static int netIdx = 0;

static char availTopic[64], baseTopic[64];
static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static bool mqttUp = false, verbose = false, wifiUp = false;
static uint32_t lastWifiTry = 0, lastMqttTry = 0, lastPublish = 0, lastRefresh = 0;
static uint32_t basePackets = 0, baseUnknown = 0;
static uint32_t lastRxMs = 0, lastReArm = 0, lastRadioCheck = 0;
static int lastHopRssi = 0, lastHopSnr = 0;
static char moduleVer[64] = "", p2pReadback[64] = "";

// RAM log of observations. A board on solar has no console, so the data is kept and
// dumped with `d` when the cable is back on. RAM lives as long as the cell does.
#define OBS_MAX 40
static ctgw::Obs obsLog[OBS_MAX];
static int obsN = 0, obsPtr = 0;

static String resp;

// ---------------------------------------------------------------------------
// Console — every print gated on the cable
// ---------------------------------------------------------------------------
static bool host() { return Serial.isPlugged(); }

void logf(const char *fmt, ...) {
  if (!host()) return;
  va_list ap;
  va_start(ap, fmt);
  Serial.vprintf(fmt, ap);
  va_end(ap);
}

void log(const char *s) { if (host()) Serial.print(s); }

// ---------------------------------------------------------------------------
// Names from the flash-time lists
// ---------------------------------------------------------------------------
// `STR(CAT_NAMES)` and `STR(NODE_NAMES)` are pipe-separated, in index order, so
// Home Assistant's device names come out as Chichi / Onanda / Chacha and Node 1 /
// Node 2, baked in the same way the collar id is.
// Console use only: one name printed at a time. The buffer is static, so every
// caller shares it, and the last name parsed is what every earlier caller sees.
const char *nameAt(const char *list, int i) {
  static char buf[24];
  const char *p = list;
  for (int k = 0; k < i; k++) {
    p = strchr(p, '|');
    if (!p) return "";
    p++;
  }
  size_t n = 0;
  while (p[n] && p[n] != '|' && n < sizeof(buf) - 1) { buf[n] = p[n]; n++; }
  buf[n] = '\0';
  return buf;
}

// Discovery use: names have to be kept at the same time, because the set of
// discovery records is built in one pass. A static buffer makes every name the same
// string, and Home Assistant created entities called "Cat Base 3 Node 1 Node 3
// signal" for every cat - the entity_id for Chacha was built from the node label.
void nameList(const char *list, char (*out)[24], int n) {
  const char *p = list;
  for (int i = 0; i < n; i++) {
    int k = 0;
    if (p) {
      while (p[k] && p[k] != '|' && k < 23) { out[i][k] = p[k]; k++; }
      p = strchr(p, '|');
      if (p) p++;
    }
    out[i][k] = '\0';
  }
}

// ---------------------------------------------------------------------------
// Applying a measurement
// ---------------------------------------------------------------------------
// The RSSI moves the smoothed value by 1/SMOOTH_K of the difference, in integers, so
// the publish path never formats a float.
static void smooth(int16_t &rssi10, int rawDbm, bool first) {
  int16_t raw10 = (int16_t)(rawDbm * 10);
  if (first) { rssi10 = raw10; return; }
  rssi10 = (int16_t)(rssi10 + (raw10 - rssi10) / SMOOTH_K);
}

// One collar frame, from the 433 radio (node 0) or, in the relay's case, from the
// numbers the relay measured. Byte layout is the collar's, in collar_frame.h — the
// decode lives in one place for the whole project, and its host test is
// homebase/esphome/test_collar_frame.cpp.
//
// `own` is true only for the board's own radio: the relay's ASCII frame carries no
// collar battery, so a relay row must say "unknown" (255) rather than inherit a
// number measured by somebody else.
void applyFrame(int node, uint8_t id, int seq, int rssi, int snr, uint8_t batt,
               bool own, long ts) {
  if (node < 0 || node >= ctgw::NUM_NODES || id >= ctgw::NUM_COLLARS) {
    baseUnknown++;
    return;
  }
  Point &p = pt[node];
  ctgw::CatRow &c = p.row.cats[id];
  bool first = !c.have;
  c.have = true;

  // Sequence gaps are the packets that never arrived. Taken as a signed byte so a
  // repeat (step 0) and an out-of-order frame (negative) add nothing, and 255 -> 0
  // is zero missed, not 255.
  if (p.catSeqValid[id]) {
    const int8_t step = (int8_t)(uint8_t)(seq - p.catSeq[id]);
    if (step > 1) c.missed += (uint32_t)(step - 1);
  }
  p.catSeq[id] = (uint8_t)seq;
  p.catSeqValid[id] = true;

  smooth(c.rssi10, rssi, first);
  // Only the board that measured it fills in SNR. A relay's frame has no collar SNR,
  // so the row keeps snrKnown false and publishes JSON null, which Home Assistant
  // renders as unknown — not the 0 dB that an unmeasured number would look like.
  if (own) { c.snr2 = (int8_t)(snr * 2); c.snrKnown = true; }
  c.batt = own ? batt : ctgw::BATT_UNKNOWN;
  c.packets++;
  p.catMs[id] = millis();
  basePackets++;

  ctgw::Obs o;
  o.node = node; o.collar = id; o.collarSeq = seq; o.rssi = rssi; o.snr = snr;
  o.batt = c.batt; o.hopRssi = lastHopRssi; o.hopSnr = lastHopSnr;
  o.nodeSeq = (uint32_t)(p.nodeSeq >= 0 ? p.nodeSeq : 0);
  o.ts = ts;
  obsLog[obsPtr] = o;
  obsPtr = (obsPtr + 1) % OBS_MAX;
  if (obsN < OBS_MAX) obsN++;
}

// A relay report: `CAT<n> <seq> c<id> s<collarseq> r<rssi>`.
void applyReport(const RakFrame &f, long ts) {
  int node = f.node;
  if (node < 1 || node >= ctgw::NUM_NODES) { baseUnknown++; return; }
  Point &p = pt[node];
  p.row.packets++;
  if (p.nodeSeq >= 0 && f.seq > p.nodeSeq + 1) p.row.missed += (uint32_t)(f.seq - p.nodeSeq - 1);
  p.nodeSeq = f.seq;
  p.lastMs = millis();
  lastHopRssi = f.rssi; lastHopSnr = f.snr; lastRxMs = millis();
  // The node's cell and charge line ride in the report tail. A report that omits them
  // leaves the last known value: a node that drops the battery field for one cycle
  // must not make the dashboard read "unknown" for a cell it measured a minute ago.
  if (f.nodeBatt >= 0) p.row.nodeBatt = f.nodeBatt;
  if (f.nodeChg >= 0) p.row.nodeChg = f.nodeChg;

  // `c-1` is the relay saying "nothing heard in the last 30 s". It is not a cat, and
  // a relay that kept repeating a stale RSSI would look exactly like a cat standing
  // still, so the absence is published as absence.
  if (f.collar >= 0) applyFrame(node, (uint8_t)f.collar, f.collarSeq, f.collarRssi, 0,
                               ctgw::BATT_UNKNOWN, false, ts);

  ctgw::Obs o;
  o.node = node; o.collar = f.collar; o.collarSeq = f.collarSeq;
  o.rssi = f.collarRssi; o.snr = f.snr; o.batt = ctgw::BATT_UNKNOWN;
  o.hopRssi = f.rssi; o.hopSnr = f.snr; o.nodeSeq = (uint32_t)f.seq; o.ts = ts;
  if (f.collar >= 0) { obsLog[obsPtr] = o; obsPtr = (obsPtr + 1) % OBS_MAX;
                       if (obsN < OBS_MAX) obsN++; }
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------
static bool timeSynced() { return time(nullptr) > 1700000000; }

// Ages are computed here, at publish time, from the arrival stamps. A published age
// frozen at the last packet would show a cat missing for an hour as heard four
// minutes ago, which is the exact thing the base station exists to detect.
void refreshRows() {
  long now = time(nullptr);
  bool synced = timeSynced();
  for (int n = 0; n < ctgw::NUM_NODES; n++) {
    Point &p = pt[n];
    ctgw::NodeRow &r = p.row;
    r.in = p.lastMs && (millis() - p.lastMs <= NODE_STALE_MS);
    r.ageS = p.lastMs ? (millis() - p.lastMs) / 1000u : 0;
    r.last = (synced && p.lastMs) ? (long)(now - r.ageS) : 0;
    for (int c = 0; c < ctgw::NUM_COLLARS; c++) {
      ctgw::CatRow &cr = r.cats[c];
      uint32_t cm = p.catMs[c];
      cr.in = cr.have && cm && (millis() - cm <= (n == 0 ? CAT_STALE_OWN : CAT_STALE_NODE));
      cr.ageS = cm ? (millis() - cm) / 1000u : 0;
      cr.last = (synced && cm) ? (long)(now - cr.ageS) : 0;
    }
  }
}

void publishAll() {
  char topic[64], buf[512];
  long ts = timeSynced() ? time(nullptr) : 0;
  int firstNode = HAS_433 ? 0 : 1;
  int heard = 0;
  for (int n = firstNode; n < ctgw::NUM_NODES; n++) {
    if (ctgw::nodeStateTopic(topic, sizeof(topic), STR(BASE_ID), n) == 0) continue;
    int len = ctgw::buildNodeState(buf, sizeof(buf), ts, pt[n].row);
    // Overflow is a dropped publish, not a partial one. A truncated record on a
    // retained topic is a dashboard that renders nothing and a broker that keeps the
    // wreckage until the next snapshot.
    if (len == 0) { logf("[warn] node %d state overflow, publish dropped\n", n); continue; }
    if (!mqtt.publish(topic, buf, true)) logf("[warn] publish node %d failed\n", n);
    if (pt[n].row.in) heard++;
  }
  int batt = ctgw::BATT_UNKNOWN;
#if BAT_ADC >= 0
  uint32_t raw = 0;
  for (int i = 0; i < 8; i++) raw += analogRead(BAT_ADC);
  float v = (raw / 8.0f) * 3.3f / 4095.0f * BAT_DIV;
  int pct = (int)((v - 3.0f) * 100.0f / 1.2f);      // 3.0 V = 0 %, 4.2 V = 100 %
  batt = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
#endif
  int chg = 0;
#if CHRG_PIN >= 0
  chg = digitalRead(CHRG_PIN) == LOW ? 1 : 0;
#endif
  int w = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  char ip[16];
  snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
  // `baseTopic` is built once in setup(); reusing it keeps this path one snprintf.
  int len = ctgw::buildBaseState(buf, sizeof(buf), ts, basePackets, baseUnknown,
                                 (uint8_t)batt, chg, w, ip, heard);
  if (len == 0) { logf("[warn] base state overflow, publish dropped\n"); return; }
  mqtt.publish(baseTopic, buf, true);
  lastPublish = millis();
}

// The discovery set. Records are built in RAM (ctgw::Discovery is ~42 KB for 4
// nodes, which is ESP32 heap, not ESP8266), published retained, and rebuilt on every
// MQTT connect so a broker restart re-reads them.
void publishDiscovery() {
  // 6 node-level rows per node (in range, last seen, packets, missed, battery,
  // charging), not 4. A cap that is short by two silently drops the last records of
  // the last node, and the symptom is a node row that never appears in Home
  // Assistant — the discovery pass prints "82 of 82" and looks complete.
  const int MAXREC = ctgw::NUM_NODES * (ctgw::NUM_COLLARS * 5 + 6) + 6;
  static ctgw::Discovery *rec = nullptr;
  if (rec == nullptr) rec = (ctgw::Discovery *)malloc(MAXREC * sizeof(ctgw::Discovery));
  if (rec == nullptr) { logf("[warn] discovery heap failed, %d records not published\n", MAXREC); return; }

  // Storage, not aliases. Every record built in this pass needs its own copy of the
  // name it puts in `name:`.
  static char catStore[ctgw::NUM_COLLARS][24], nodeStore[ctgw::NUM_NODES][24];
  nameList(STR(CAT_NAMES), catStore, ctgw::NUM_COLLARS);
  nameList(STR(NODE_NAMES), nodeStore, ctgw::NUM_NODES);
  const char *cats[ctgw::NUM_COLLARS], *nodes[ctgw::NUM_NODES];
  for (int i = 0; i < ctgw::NUM_COLLARS; i++) cats[i] = catStore[i];
  for (int n = 0; n < ctgw::NUM_NODES; n++) nodes[n] = nodeStore[n];

  int nrec = ctgw::buildAllDiscovery(rec, (size_t)MAXREC, STR(BASE_ID), cats, nodes,
                                     availTopic, HAS_433);
  logf("[discovery] %d of %d records built\n", nrec, MAXREC);
  for (int i = 0; i < nrec; i++) {
    if (rec[i].topic[0] == '\0' || rec[i].json[0] == '\0') continue;
    if (!mqtt.publish(rec[i].topic, rec[i].json, true))
      logf("[warn] discovery %s failed\n", rec[i].topic);
  }
}

// WiFi and MQTT are driven without blocking the radio. The last-will is the point of
// the availability topic: if the base loses power or WiFi, the broker fires it and
// the house knows it has gone blind, so a Missing alert can only ever mean the cat.
void wifiPump() {
  if (WiFi.status() == WL_CONNECTED) { wifiUp = true; return; }
  if (millis() - lastWifiTry < ROTATE_MS) return;
  lastWifiTry = millis();
  // Rotate, not retry: the core's own auto-reconnect keeps re-issuing the last
  // config, so a board stuck on a network it cannot reach never gets to the next
  // one. Auto-reconnect is switched off in setup() and this is the only connect path.
  const Net &n = NETS[netIdx];
  netIdx = (netIdx + 1) % NET_N;
  if (n.ssid == nullptr || n.ssid[0] == '\0') return;
  logf("[net] trying %s at txpower %d\n", n.ssid, TX_POWER);
  WiFi.begin(n.ssid, n.pass);
  // TX power has to be lowered on these boards. At the default the station starts,
  // prints STA_DISCONNECTED twice, and sits at WL_DISCONNECTED for 25 s with a scan
  // that found the AP a second earlier. At 8.5 dBm the same board associates in one
  // second and reaches the broker. This is the documented ESP32-C3 clone failure:
  // the receive side works, the transmit side never gets a frame out.
  //   https://github.com/espressif/arduino-esp32/issues/7265
  //   https://github.com/HomeSpan/HomeSpan/issues/907
  WiFi.setTxPower((wifi_power_t)TX_POWER);
}

void mqttPump() {
  wifiPump();
  if (WiFi.status() != WL_CONNECTED) { mqttUp = false; wifiUp = false; return; }
  wifiUp = true;

  if (!mqtt.connected()) {
    if (millis() - lastMqttTry < 5000) return;
    lastMqttTry = millis();
    if (mqtt.connect("cattracker-base3", availTopic, 1, true, "offline")) {
      mqttUp = true;
      mqtt.publish(availTopic, "online", true);
      publishDiscovery();
      refreshRows();
      publishAll();
      logf("[mqtt] connected to %s:%d as base3\n", MQTT_BROKER, MQTT_PORT);
    } else {
      logf("[mqtt] connect failed, rc=%d\n", mqtt.state());
    }
    return;
  }
  mqtt.loop();
}

// ---------------------------------------------------------------------------
// RAK3172 on UART2
// ---------------------------------------------------------------------------
void poke() { Serial1.print("\r\n"); }

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

bool isStatus(const String &l) { return l == "OK" || l.startsWith("AT_"); }

void handleEvent(String e) {
  long ts = timeSynced() ? time(nullptr) : 0;
  RakFrame f = parseRxp2p(e.c_str());
  if (!f.ok) return;
  lastHopRssi = f.rssi; lastHopSnr = f.snr; lastRxMs = millis();
  applyReport(f, ts);
  if (verbose) {
    if (f.collar >= 0)
    logf("[RX] node %d seq %ld  cat %d seq %d r%d  | 915 rssi %d snr %d  cell %d chg %d\n",
           f.node, f.seq, f.collar, f.collarSeq, f.collarRssi, f.rssi, f.snr,
           f.nodeBatt, f.nodeChg);
    else
      logf("[RX] node %d seq %ld  no collar  | 915 rssi %d snr %d\n", f.node, f.seq, f.rssi, f.snr);
  }
}

bool atCmd(String cmd, uint32_t ms = 4000, int tries = 3) {
  resp = "";
  for (int attempt = 0; attempt < tries; attempt++) {
    poke();
    Serial1.print(cmd); Serial1.print("\r\n");
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
      uint32_t left = ms - (millis() - t0);
      String l = readLine(left > 0 ? left : 1);
      if (!l.length()) continue;
      if (isStatus(l)) return l == "OK";
      if (l.startsWith("+EVT")) { handleEvent(l); continue; }
      resp = l;
    }
    if (attempt == tries - 1) logf("[at] %s — no reply\n", cmd.c_str());
  }
  return false;
}

// Bandwidth is a CODE in AT+P2P (0 = 125 kHz), not kHz. Typing 125 gets
// AT_PARAM_ERROR and the module keeps its own defaults, which on this part are
// already the target settings — so a rejected set reads as a success. Compare the
// readback string, never the string you typed.
static int bwCode(int bw) {
  if (bw == 125) return 0;
  if (bw == 250) return 1;
  if (bw == 500) return 2;
  return -1;
}

void bringUp() {
  int bw = bwCode(BW);
  if (bw < 0) { log("[warn] BW must be 125/250/500\n"); return; }
  char p2pSet[64], p2pWant[40];
  snprintf(p2pSet, sizeof(p2pSet), "AT+P2P=%lu:%d:%d:%d:%d:%d",
           FREQ_HZ, SF, bw, CR, PREAMBLE, TX_DBM);
  snprintf(p2pWant, sizeof(p2pWant), "%lu:%d:%d:%d:%d:%d",
           FREQ_HZ, SF, bw, CR, PREAMBLE, TX_DBM);

  // The module boots ~1 s after the C3, and on a cold solar power-up it can be ready
  // well after. A short probe prints a false "module not answering" and silently
  // skips P2P setup while the module is fine, so probe for 40 s.
  bool alive = false;
  for (int i = 0; i < 24 && !alive; i++) {
    delay(500);
    alive = atCmd("AT", 1200, 1);
    if (!alive && verbose) logf("[probe %d] module not ready\n", i + 1);
  }
  if (!alive) {
    log("\n[warn] module not answering on UART2. Check, in this order:\n");
    log("  1. pad 24 (VDD) = 3.3 V, and pad 24 -> pad 23 is OPEN (23 is GND and sits\n");
    log("     beside it — a slid VDD joint shorts the rail and the C3 vanishes from USB).\n");
    log("  2. C3 GPIO20 (RX) -> pad 2 (PA2/UART2_TX), GPIO21 (TX) -> pad 1 (PA3/UART2_RX).\n");
    log("  3. pad 22 (RST) HIGH, pad 21 (BOOT0) LOW. Floating BOOT0 = STM32 UART bootloader.\n");
    log("  4. 100 uF across VDD-GND at the module, antenna fitted before any transmit.\n");
    return;
  }

  atCmd("AT+LPM=0");              // a sleeping module drops the first byte
  atCmd("AT+VER=?");
  snprintf(moduleVer, sizeof(moduleVer), "%s", resp.c_str());
  logf("[mod] %s\n", moduleVer);

  atCmd("AT+NWM=?");
  if (resp.indexOf("0") < 0) {
    logf("[mod] nwm=%s -> setting P2P\n", resp.c_str());
    atCmd("AT+NWM=0");            // the module RESTARTS on a mode change
    delay(2000);
    Serial1.end(); Serial1.flush();
    Serial1.begin(115200, SERIAL_8N1, PIN_RAK_RX, PIN_RAK_TX);
    delay(1500);
    while (Serial1.available()) Serial1.read();
  }

  // AT+PRECV is persistent across reboot: the module boots in continuous RX from the
  // last session, and AT+P2P is rejected with AT_BUSY_ERROR while it is armed. Stop
  // RX first, set, read back, then arm.
  atCmd("AT+PRECV=0");
  if (!atCmd(p2pSet)) log("[warn] P2P settings rejected\n");
  atCmd("AT+P2P=?");
  snprintf(p2pReadback, sizeof(p2pReadback), "%s", resp.c_str());
  logf("[mod] readback %s\n", p2pReadback);
  if (resp.indexOf(p2pWant) < 0)
    logf("[warn] readback does not contain %s — do not trust what you typed\n", p2pWant);

  if (atCmd("AT+PRECV=65534")) log("[mod] continuous RX armed\n");
  else log("[warn] RX not armed\n");
}

void reArmRx() {
  atCmd("AT+PRECV=0");
  atCmd("AT+PRECV=65534");
  lastReArm = millis();
  log("[mod] RX re-armed\n");
}

// ---------------------------------------------------------------------------
// RA-02 / SX1278 on 433 MHz (node 0), compiled in only when fitted
// ---------------------------------------------------------------------------
#if HAS_433
#include "collar_frame.h"

// Bit-banged SPI. The C3's hardware SPI read 0x00 from every register on these
// same pads while bit-bang read the correct values, so the SPI1 path is not usable
// here.
#define PIN_SCK 4
#define PIN_MISO 5
#define PIN_MOSI 6
#define PIN_NSS 7
#define PIN_RST 1
#define PIN_DIO0 0

#define SF433 9
#define PREAMBLE433 8
#define BW433_CODE 0x70
#define CR47_CODE 0x06
#define SYNC433 0x12

const uint8_t R_FIFO = 0x00, R_OP_MODE = 0x01, R_FRF_MSB = 0x06, R_FRF_MID = 0x07,
            R_FRF_LSB = 0x08, R_PA_CONFIG = 0x09, R_OCP = 0x0B, R_LNA = 0x0C,
            R_FIFO_ADDR_PTR = 0x0D, R_FIFO_TX_BASE = 0x0E, R_FIFO_RX_BASE = 0x0F,
            R_FIFO_RX_CURRENT = 0x10, R_IRQ_FLAGS = 0x12, R_RX_NB_BYTES = 0x13,
            R_PKT_RSSI = 0x1A, R_PKT_SNR = 0x19, R_MODEM1 = 0x1D, R_MODEM2 = 0x1E,
            R_PREAMBLE_MSB = 0x20, R_PREAMBLE_LSB = 0x21, R_PAYLOAD_LEN = 0x22,
            R_MODEM3 = 0x26, R_DETECT_OPT = 0x31, R_THRESH = 0x37, R_INVERT_IQ = 0x33,
            R_INVERT_IQ2 = 0x3B, R_SYNC = 0x39, R_VERSION = 0x42, R_PA_DAC = 0x4D;

// 0x83 is TRANSMIT and 0x85 is continuous RX. They were swapped in the first draft,
// and the failure is silent: the radio sat in TX and every "packet" reported was its
// own TxDone.
const uint8_t M_SLEEP = 0x80, M_STDBY = 0x81, M_TX = 0x83, M_RXCONT = 0x85;
const uint8_t F_RX_DONE = 0x40, F_CRC_ERROR = 0x20, F_TX_DONE = 0x08, F_CLEAR_ALL = 0xFF;

int rssiFrom(uint8_t raw, int snrQuarter) {
  int rssi = -164 + (int8_t)raw;         // -164 below 868 MHz; -156 reads ~8 dB wrong
  int snr = snrQuarter / 4;
  if (snr < 0) rssi += snr;
  return rssi;
}

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

// NRESET is released to the internal pull-up, never tied to 3V3: the datasheet wants
// NRESET floating during the power-on reset sequence.
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

  writeReg(R_OP_MODE, M_SLEEP); delay(1);
  writeReg(R_OP_MODE, M_STDBY); delay(1);

  uint32_t frf = (uint32_t)((433.0f * 1000000.0f * 524288.0f) / 32000000.0f);
  writeReg(R_FRF_MSB, (frf >> 16) & 0xFF);
  writeReg(R_FRF_MID, (frf >> 8) & 0xFF);
  writeReg(R_FRF_LSB, frf & 0xFF);
  writeReg(0x24, 0x00);

  writeReg(R_PA_CONFIG, 0xFF);
  writeReg(R_PA_DAC, 0x04);
  writeReg(R_OCP, 0x20 | 3);

  setRegBits(R_MODEM1, BW433_CODE, 7, 4);
  setRegBits(R_MODEM1, CR47_CODE, 3, 1);
  setRegBits(R_MODEM1, 0x04, 2, 2);        // payload CRC ON
  setRegBits(R_MODEM1, 0x00, 0, 0);        // explicit header
  setRegBits(R_MODEM2, (uint8_t)((SF433 << 4) | 0x04), 7, 2);

  setRegBits(R_DETECT_OPT, 0x03, 2, 0);
  writeReg(R_THRESH, 0x0A);
  setRegBits(R_INVERT_IQ, 0x00, 6, 6);
  setRegBits(R_INVERT_IQ, 0x01, 0, 0);
  writeReg(R_INVERT_IQ2, 0x1D);
  setRegBits(R_MODEM3, 0x04, 2, 2);

  writeReg(R_PREAMBLE_MSB, (PREAMBLE433 >> 8) & 0xFF);
  writeReg(R_PREAMBLE_LSB, PREAMBLE433 & 0xFF);
  writeReg(R_SYNC, SYNC433);
  writeReg(R_FIFO_TX_BASE, 0x00);
  setRegBits(R_LNA, 0x03, 1, 0);

  writeReg(R_PAYLOAD_LEN, ct::FRAME_LEN);
  writeReg(R_IRQ_FLAGS, F_CLEAR_ALL);
  writeReg(R_OP_MODE, M_RXCONT);
}

void check433() {
  uint32_t frf = ((uint32_t)readReg(R_FRF_MSB) << 16) |
                 ((uint32_t)readReg(R_FRF_MID) << 8) | readReg(R_FRF_LSB);
  logf("[433] ver %02X want 12  frf %06X want 6C4000  mc1 %02X want 76  mc2 %02X want 94\n",
       readReg(R_VERSION), frf, readReg(R_MODEM1), readReg(R_MODEM2));
  logf("[433] sync %02X want 12  inviq %02X want 27  pa %02X want FF  opmode %02X want 85\n",
       readReg(R_SYNC), readReg(R_INVERT_IQ), readReg(R_PA_CONFIG), readReg(R_OP_MODE));
}

void poll433() {
  uint8_t ir = readReg(R_IRQ_FLAGS);
  if (!(ir & F_RX_DONE)) return;
  if (ir & F_CRC_ERROR) { writeReg(R_IRQ_FLAGS, F_CLEAR_ALL); baseUnknown++; return; }

  uint8_t len = readReg(R_RX_NB_BYTES);
  if (len > ct::FRAME_LEN) len = ct::FRAME_LEN;
  uint8_t addr = readReg(R_FIFO_RX_CURRENT);
  writeReg(R_FIFO_ADDR_PTR, addr);            // seed the pointer, or the payload lands shifted
  uint8_t buf[ct::FRAME_LEN] = {0};
  for (int i = 0; i < len; i++) buf[i] = readReg(R_FIFO);
  int snrQ = (int8_t)readReg(R_PKT_SNR);
  int rssi = rssiFrom(readReg(R_PKT_RSSI), snrQ);
  int snr = snrQ / 4;
  writeReg(R_IRQ_FLAGS, F_CLEAR_ALL);

  // Two guards that the log taught us. A packet read while the packet-status
  // registers are still settling gives an impossible RSSI (-289 dBm seen on the bench)
  // even though the payload decoded, and every real collar packet was reported twice.
  if (rssi < -140 || rssi > -20) { baseUnknown++; return; }
  if (buf[0] != ct::FRAME_MAGIC) { baseUnknown++; return; }

  long ts = timeSynced() ? time(nullptr) : 0;
  applyFrame(0, buf[2], buf[1], rssi, snr, buf[3], true, ts);
  pt[0].lastMs = millis();
  lastRxMs = millis();
  if (verbose)
    logf("[433] cat %d seq %d  rssi %d snr %d batt %d\n", buf[2], buf[1], rssi, snr, buf[3]);
}

// The RegVersion watchdog. A module that stops answering SPI is not recoverable
// without a reset line, and RST is on GPIO1 for exactly this.
void radioWatchdog() {
  if (millis() - lastRadioCheck < 60000) return;
  lastRadioCheck = millis();
  if (readReg(R_VERSION) != 0x12) {
    log("[warn] 433 RegVersion lost, resetting the module\n");
    reset433();
    init433();
  }
}
#else
void radioWatchdog() {}
#endif

// ---------------------------------------------------------------------------
// Console reports
// ---------------------------------------------------------------------------
void printReport() {
  refreshRows();   // the report is read with the board off the network
  logf("\nGATEWAY ver 1  base %s  node 0 %s\n", STR(BASE_ID),
       HAS_433 ? "own RA-02 on 433" : "absent (915 relay only)");
  logf("module %s\nreadback %s\n", moduleVer, p2pReadback);
  logf("wifi %s rssi %d  ip %s  mqtt %s\n",
       WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "not associated",
       WiFi.RSSI(), WiFi.localIP().toString().c_str(), mqttUp ? "connected" : "down");
  int batt = ctgw::BATT_UNKNOWN;
#if BAT_ADC >= 0
  uint32_t raw = 0;
  for (int i = 0; i < 8; i++) raw += analogRead(BAT_ADC);
  float v = (raw / 8.0f) * 3.3f / 4095.0f * BAT_DIV;
  logf("cell %.2f V at ADC pin %.2f V (factor needs a measured calibration)\n",
       v, (raw / 8.0f) * 3.3f / 4095.0f);
  int pct = (int)((v - 3.0f) * 100.0f / 1.2f);
  batt = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
#endif
  logf("base pk=%lu unk=%lu batt=%d\n", (unsigned long)basePackets,
       (unsigned long)baseUnknown, batt);
#if HAS_433
  check433();
#endif
  // Same integer scaling as the JSON, so the console and the dashboard can be read
  // against each other.
  for (int n = (HAS_433 ? 0 : 1); n < ctgw::NUM_NODES; n++) {
    ctgw::NodeRow &r = pt[n].row;
    logf("[n%d %s] in=%d age=%lu pk=%lu miss=%lu cell=%d chg=%d\n", n, nameAt(STR(NODE_NAMES), n),
         r.in ? 1 : 0, (unsigned long)r.ageS, (unsigned long)r.packets,
         (unsigned long)r.missed, r.nodeBatt, r.nodeChg);
    for (int c = 0; c < ctgw::NUM_COLLARS; c++) {
      ctgw::CatRow &cr = r.cats[c];
      if (!cr.have) continue;
      logf("   [%s] r10=%d s2=%d batt=%d pk=%lu miss=%lu age=%lu in=%d\n",
           nameAt(STR(CAT_NAMES), c), (int)cr.rssi10, (int)cr.snr2, (int)cr.batt,
           (unsigned long)cr.packets, (unsigned long)cr.missed,
           (unsigned long)cr.ageS, cr.in ? 1 : 0);
    }
  }
}

void dumpObs() {
  logf("# %d observations, newest last\n", obsN);
  for (int i = 0; i < obsN; i++) {
    int idx = (obsPtr - obsN + i + OBS_MAX * 2) % OBS_MAX;
    const ctgw::Obs &o = obsLog[idx];
    logf("n%d c%d cseq%d r%d s%d b%d hop%d hs%d ts%ld\n", o.node, o.collar,
         o.collarSeq, o.rssi, o.snr, (int)o.batt, o.hopRssi, o.hopSnr, o.ts);
  }
}

void bridge() {
  log("\n[bridge] keystrokes go to the module, module output to the console.\n");
  log("[bridge] try: AT+P2P=?  AT+PRECV=65534.  'q' quits.\n");
  bool quit = false;
  while (!quit) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n') Serial1.print("\r\n");
      else if (c == 'q') { quit = true; break; }
      else Serial1.write(c);
    }
    while (Serial1.available()) Serial.write((char)Serial1.read());
    delay(1);
  }
  log("[bridge] quit\n");
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // A Host that opens the port and stops reading must not be able to stall loop():
  // the write is dropped instead of blocking.
  Serial.setTxTimeoutMs(0);
  Serial1.begin(115200, SERIAL_8N1, PIN_RAK_RX, PIN_RAK_TX);
  delay(1500);

  snprintf(availTopic, sizeof(availTopic), "cattracker/%s/availability", STR(BASE_ID));
  snprintf(baseTopic, sizeof(baseTopic), "cattracker/%s/state", STR(BASE_ID));
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(1024);       // a 4-node snapshot is ~300 bytes; the default is 128

  logf("\nGATEWAY ver 1  base %s  node 0 %s\n", STR(BASE_ID),
       HAS_433 ? "own RA-02 on 433" : "absent");
  logf("RAK on GPIO%d(RX)/GPIO%d(TX), 915 MHz SF%d BW%d CR%d preamble %d, %d dBm\n",
       PIN_RAK_RX, PIN_RAK_TX, SF, BW, CR, PREAMBLE, TX_DBM);
  logf("availability %s\n", availTopic);
  log("Antenna must be on the module before it transmits. 8.2 cm radiator at 915 MHz.\n");

  bringUp();                    // the module first, on an undisturbed rail

#if HAS_433
  delay(500);                   // the RA-02's cold inrush must not land on the module's boot
  logf("RA-02 on SCK=%d MISO=%d MOSI=%d NSS=%d RST=%d DIO0=%d, 433 MHz SF9 BW125 CR4/7 "
       "sync %02X preamble %d. Antenna 17.3 cm.\n",
       PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS, PIN_RST, PIN_DIO0, SYNC433, PREAMBLE433);
  init433();
  check433();
  if (!atCmd("AT", 1200, 2))
    log("[warn] RAK stopped answering after the RA-02 was initialised\n");
#endif

#if BAT_ADC >= 0
  pinMode(BAT_ADC, INPUT);
#endif
#if CHRG_PIN >= 0
  pinMode(CHRG_PIN, INPUT_PULLUP);
#endif
#if STDBY_PIN >= 0
  pinMode(STDBY_PIN, INPUT_PULLUP);
#endif

  WiFi.mode(WIFI_STA);
  // Modem sleep off: boards in this house do not associate reliably with it on, and
  // the failure looks like "the base is on the network but mute".
  WiFi.setSleep(false);
  IPAddress ip(192, 168, 100, STATIC_IP_LAST), gw(192, 168, 100, 1),
            mask(255, 255, 255, 0), dns(192, 168, 100, 1);
  WiFi.config(ip, gw, mask, dns);
  // Auto-reconnect is off: it re-issues the last config forever, so a board that
  // cannot reach its first network never gets to the fallback. wifiPump() drives the
  // rotation.
  WiFi.setAutoReconnect(false);
  WiFi.begin(NETS[0].ssid, NETS[0].pass);
  netIdx = 1 % NET_N;
  lastWifiTry = millis();
  // last-seen is a timestamp, so the board needs wall-clock time. Until SNTP lands,
  // `last` publishes as 0 and Home Assistant renders unknown, not 1970.
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  logf("[net] static 192.168.100.%d, broker %s:%d\n", STATIC_IP_LAST,
       MQTT_BROKER, MQTT_PORT);
  log("\n[h] help [v] report [w] network [p] module [r] re-arm RX [d] dump obs [n] scan wifi [b] bridge [x] stop RX\n");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'h') {
      log("\n[h] help [v] report [w] network [p] module [r] re-arm RX [d] dump obs [n] scan wifi [b] bridge [x] stop RX\n");
      log("v prints every node row in the same integer scaling as the MQTT state.\n");
    }
    else if (c == 'v') { verbose = !verbose; printReport(); }
    else if (c == 'w') {
      logf("[net] status %d  ssid %s  rssi %d  ip %s  txpower %d  next %s\n",
           WiFi.status(), WiFi.SSID().c_str(), WiFi.RSSI(),
           WiFi.localIP().toString().c_str(), WiFi.getTxPower(), NETS[netIdx].ssid);
      logf("[mqtt] %s  state %d\n", mqtt.connected() ? "connected" : "down", mqtt.state());
    }
    else if (c == 'p') {
      atCmd("AT+VER=?");   logf("[mod] ver %s\n", resp.c_str());
      atCmd("AT+NWM=?");   logf("[mod] nwm %s\n", resp.c_str());
      atCmd("AT+P2P=?");   logf("[mod] p2p %s\n", resp.c_str());
      atCmd("AT+PRECV=?"); logf("[mod] precv %s\n", resp.c_str());
      logf("[mod] last hop rssi %d snr %d\n", lastHopRssi, lastHopSnr);
    }
    else if (c == 'r') reArmRx();
    else if (c == 'd') dumpObs();
    else if (c == 'n') {
      // "It is on the network" and "it is on the network I assumed" are otherwise
      // indistinguishable: a board sitting on a passphrase that does not match its
      // router, or on a router that is off, prints nothing but "not associated".
      WiFi.disconnect();          // the connecting station owns the radio: a scan
      delay(400);                // while it is mid-association returns -2, not a list
      int cnt = WiFi.scanNetworks();
      logf("[scan] %d networks\n", cnt);
      for (int i = 0; i < cnt; i++)
        logf("   %s rssi %d ch %d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
      WiFi.scanDelete();
      WiFi.reconnect();
    }
    else if (c == 'b') bridge();
    else if (c == 'x') { atCmd("AT+PRECV=0"); log("[mod] RX stopped\n"); }
  }

  // RX events land here. 20 ms keeps loop() responsive; continuous RX needs no
  // re-arm after a receive.
  String l = readLine(20);
  if (l.length() && l.startsWith("+EVT")) handleEvent(l);

#if HAS_433
  poll433();
#endif

  mqttPump();

  // Rows are refreshed on the timer, not on the publish. The console report is read
  // on the bench with the board off the network, and rows that only refresh inside
  // the publish path show in=0 age=0 for a receiver that is answering every 8 s.
  if (millis() - lastRefresh >= PUBLISH_MS) {
    lastRefresh = millis();
    refreshRows();
  }
  if (mqttUp && millis() - lastPublish >= PUBLISH_MS) {
    lastPublish = millis();
    publishAll();
  }

  // A gateway that stops hearing has to say so, and the module can stop delivering
  // without any error visible on the console. Re-arm, and re-arm only occasionally:
  // AT+PRECV=0 mid-reception throws away the packet in flight.
  if (lastRxMs && millis() - lastRxMs > 180000 && millis() - lastReArm > 120000) {
    log("[warn] 3 minutes of silence, re-arming RX\n");
    reArmRx();
  }

  radioWatchdog();
}
