// wifi_check — a minimal WiFi probe for one ESP32-C3, nothing else on the board.
//
// Purpose: separate "this board cannot associate" from "this firmware's WiFi path is
// wrong". On the bench the gateway reported status 6 (WL_DISCONNECTED) for every
// network, for 25 s, after a scan that had found all three SSIDs a second earlier.
// That combination is the documented ESP32-C3 clone signature: the radio receives,
// the transmit side never gets a frame out, and the fix is to lower the TX power
// after begin() rather than leave it at the default.
//
//   https://github.com/espressif/arduino-esp32/issues/7265  (scan sees everything,
//   connect fails; reduced TX power is what worked)
//   https://github.com/HomeSpan/HomeSpan/issues/907          (same, C3 board)
//
// This probe prints the Wi-Fi event stream and the disconnect reason, so "no AP
// found", "auth expire" and "never started" are three different answers, not one.
//
// Credentials come from build flags (see gateway/build.sh); prints are gated on
// Serial.isPlugged(), and the port must be opened DTR-low.

#include <Arduino.h>
#include <WiFi.h>

#ifndef WIFI_SSID
#error "WIFI_SSID must be passed as a build flag"
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

struct Net { const char *ssid; const char *pass; };
static const Net NETS[] = { {WIFI_SSID, WIFI_PASS}, {WIFI_SSID2, WIFI_PASS2},
                            {WIFI_SSID3, WIFI_PASS3} };
static const int NET_N = sizeof(NETS) / sizeof(NETS[0]);

bool host() { return Serial.isPlugged(); }
void logf(const char *fmt, ...) {
  if (!host()) return;
  va_list ap; va_start(ap, fmt); Serial.vprintf(fmt, ap); va_end(ap);
}

// The wl_status_t numbers, spelled out, because "status 6" is not a diagnosis.
const char *stName(int s) {
  switch (s) {
    case 0: return "WL_DISCONNECTED";
    case 1: return "WL_NO_SSID_AVAIL";
    case 2: return "WL_SCAN_COMPLETED";
    case 3: return "WL_CONNECTED";
    case 4: return "WL_CONNECT_FAILED";
    case 5: return "WL_CONNECTION_LOST";
    default: return "?";
  }
}

// The disconnect reason is the only thing that distinguishes "the AP is not there",
// "the passphrase is wrong", and "the station never started". WiFi.status() says
// WL_DISCONNECTED for all three.
// The core's callback gives the event id only. The ids are the useful part here:
// WIFI_READY / STA_START / STA_DISCONNECTED in sequence says the station is alive,
// and a station that never prints STA_START never got a config at all.
void onEvent(arduino_event_id_t e) {
  if (!host()) return;
  const char *nm = "?";
  switch (e) {
    case ARDUINO_EVENT_WIFI_READY: nm = "WIFI_READY"; break;
    case ARDUINO_EVENT_WIFI_STA_START: nm = "STA_START"; break;
    case ARDUINO_EVENT_WIFI_STA_STOP: nm = "STA_STOP"; break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: nm = "STA_DISCONNECTED"; break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED: nm = "STA_CONNECTED"; break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: nm = "GOT_IP"; break;
    case ARDUINO_EVENT_SC_SCAN_DONE: nm = "SC_SCAN_DONE"; break;
    default: return;
  }
  logf("   [evt] %s\n", nm);
}

void tryNet(const Net &n, int power) {
  logf("\n[try] %s at txpower %d\n", n.ssid, power);
  // Auto-reconnect off, then begin, then the power. The order matters: begin()
  // issues the connect, and the power change after it applies to the connection in
  // flight — which is exactly the published fix for the clone boards that scan fine
  // and never associate.
  WiFi.setAutoReconnect(false);
  WiFi.begin(n.ssid, n.pass);
  if (power >= 0) WiFi.setTxPower((wifi_power_t)power);
  uint32_t t0 = millis();
  int last = -1;
  while (millis() - t0 < 25000) {
    int s = WiFi.status();
    if (s != last) { logf("   %d %s\n", s, stName(s)); last = s; }
    if (s == WL_CONNECTED) {
      logf("   CONNECTED ip %s rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      WiFiClient c;
      bool ok = c.connect("192.168.100.66", 1883);
      logf("   broker 192.168.100.66:1883 reachable %s\n", ok ? "yes" : "NO");
      c.stop();
      return;
    }
    delay(300);
  }
  logf("   timeout, final %d %s\n", WiFi.status(), stName(WiFi.status()));
  WiFi.disconnect();
  delay(1000);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(1200);
  logf("\nWIFI_CHECK 2\n");

  WiFi.onEvent(onEvent);   // NetworkEventSysCb: the reason lives in the event struct
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  int cnt = WiFi.scanNetworks(false, true, false, 300);
  logf("[scan] %d networks\n", cnt);
  for (int i = 0; i < cnt; i++)
    logf("   %s ch %d rssi %d enc %d\n", WiFi.SSID(i).c_str(), WiFi.channel(i),
         WiFi.RSSI(i), (int)WiFi.encryptionType(i));
  WiFi.scanDelete();

  // Default power first (so the failure is reproduced, not papered over), then the
  // documented fix, then a middle step, on the network that is strongest on the bench.
  for (int n = 0; n < NET_N; n++) {
    if (NETS[n].ssid[0] == '\0') continue;
    tryNet(NETS[n], -1);
    if (WiFi.status() == WL_CONNECTED) break;
    tryNet(NETS[n], WIFI_POWER_8_5dBm);
    if (WiFi.status() == WL_CONNECTED) break;
    tryNet(NETS[n], WIFI_POWER_15dBm);
    if (WiFi.status() == WL_CONNECTED) break;
  }
  logf("\n[done] %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "not connected");
}

void loop() {
  if (host() && WiFi.status() == WL_CONNECTED) {
    // Keep the connection under observation: the point of the base station is a
    // steady link, not a single successful association.
    static uint32_t t = 0;
    if (millis() - t > 10000) {
      t = millis();
      logf("[link] rssi %d txpower %d\n", WiFi.RSSI(), WiFi.getTxPower());
    }
  }
}
