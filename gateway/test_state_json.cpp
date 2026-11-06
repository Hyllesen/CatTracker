// Host test for gateway/state_json.h — the MQTT payloads the gateway publishes.
//
//   c++ -std=c++17 -Wall -Wextra -o /tmp/gwtest gateway/test_state_json.cpp && /tmp/gwtest
//
// Every failure mode of assembling JSON with snprintf is silent on the device: a
// truncated record is published, Home Assistant logs "Unable to parse JSON", and no
// entity is created. The base station then looks healthy and the dashboard stays
// empty. So the records are checked here for length, brace balance, template
// completeness, and for the strings Home Assistant actually keys entities on.

#include <cstdio>
#include <cstring>
#include <string>

#include "state_json.h"

static int fails = 0;

static void check(bool ok, const char *what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) fails++;
}

// A JSON string that is not balanced is not parsable, and snprintf truncation is the
// usual cause. Quotes are counted outside string literals, so an escaped quote inside
// a template value cannot make this agree with itself.
static bool jsonBalanced(const char *s) {
  int depth = 0;
  bool inStr = false;
  bool esc = false;
  for (const char *p = s; *p; ++p) {
    if (inStr) {
      if (esc) { esc = false; continue; }
      if (*p == '\\') { esc = true; continue; }
      if (*p == '"') inStr = false;
      continue;
    }
    if (*p == '"') inStr = true;
    else if (*p == '{') depth++;
    else if (*p == '}') depth--;
    if (depth < 0) return false;
  }
  return depth == 0 && !inStr;
}

// Copy one record's unique_id value, exactly. Returns false if there is none.
static bool uidOf(const char *json, char *out, size_t cap) {
  const char *key = "\"unique_id\":\"";
  const char *u = strstr(json, key);
  if (u == nullptr) return false;
  u += strlen(key);
  size_t i = 0;
  while (*u && *u != '"') {
    if (i >= cap - 1) return false;
    out[i++] = *u++;
  }
  out[i] = '\0';
  return *u == '"';
}

int main() {
  using namespace ctgw;

  const char *catName[NUM_COLLARS] = {"Chichi", "Onanda", "Chacha"};
  const char *nodeName[NUM_NODES] = {"Base radio", "Node 1", "Node 2", "Node 3"};
  const char *avail = "cattracker/base3/availability";

  // ---------------------------------------------------------------- templates
  // The templates are the longest part of a discovery record, and homebase's first
  // build truncated them at 64 characters: valid JSON, half a Jinja expression, an
  // entity that renders nothing, and a cat that reads as "unavailable" forever.
  char t[128];
  check(catTemplate(t, sizeof(t), 2, "signal") > 0 &&
            strcmp(t, "{{ value_json.c2.rssi / 10 }}") == 0,
        "cat 2 signal template is exact");
  check(catTemplate(t, sizeof(t), 2, "snr") > 0 &&
            strcmp(t, "{{ (value_json.c2.snr / 2) if value_json.c2.snr is not none else none }}") == 0,
        "cat 2 SNR template guards a null SNR");
  check(catTemplate(t, 64, 2, "snr") == 0,
        "SNR template is refused at 64 chars, not truncated");
  check(catTemplate(t, sizeof(t), 2, "battery") > 0 &&
            strcmp(t, "{{ value_json.c2.batt if value_json.c2.batt < 255 else none }}") == 0,
        "cat 2 battery template maps 0xFF to none");
  check(catTemplate(t, sizeof(t), 2, "last seen") > 0 &&
            strcmp(t, "{{ (value_json.c2.last | timestamp_local) if value_json.c2.last > 0 else none }}") == 0,
        "cat 2 last-seen template maps 0 to none");
  check(catTemplate(t, sizeof(t), 2, "in range") > 0 &&
            strcmp(t, "{{ 'ON' if value_json.c2.in == 1 else 'OFF' }}") == 0,
        "cat 2 in-range template is exact");
  // The trap, stated as a test: at the length the first homebase build used, the
  // builder must refuse the record rather than publish half of it.
  check(catTemplate(t, 64, 2, "last seen") == 0,
        "last-seen template is refused at 64 chars, not truncated");

  check(nodeTemplate(t, sizeof(t), "in range") > 0 &&
            strcmp(t, "{{ 'ON' if value_json.in == 1 else 'OFF' }}") == 0,
        "node in-range template is exact");
  check(nodeTemplate(t, sizeof(t), "missed") > 0 &&
            strcmp(t, "{{ value_json.miss }}") == 0,
        "node missed reads `miss`, not base1's `unk`");
  check(baseTemplate(t, sizeof(t), "charging") > 0 &&
            strcmp(t, "{{ 'ON' if value_json.chg == 1 else 'OFF' }}") == 0,
        "base charging template reads the TP5000 CHRG flag");

  // ---------------------------------------------------------------- topics
  char topic[96];
  check(nodeStateTopic(topic, sizeof(topic), "base3", 0) > 0 &&
            strcmp(topic, "cattracker/base3/n0/state") == 0,
        "node 0 state topic");
  check(nodeStateTopic(topic, sizeof(topic), "base3", 3) > 0 &&
            strcmp(topic, "cattracker/base3/n3/state") == 0,
        "node 3 state topic");
  check(baseStateTopic(topic, sizeof(topic), "base3") > 0 &&
            strcmp(topic, "cattracker/base3/state") == 0,
        "base state topic");
  check(availTopic(topic, sizeof(topic), "base3") > 0 &&
            strcmp(topic, "cattracker/base3/availability") == 0,
        "availability topic");
  check(obsTopic(topic, sizeof(topic), "base3") > 0 &&
            strcmp(topic, "cattracker/base3/obs") == 0,
        "obs topic");

  // ---------------------------------------------------------------- state JSON
  NodeRow r;
  r.have = true;
  r.in = true;
  r.ageS = 12;
  r.last = 1762434489;
  r.packets = 123;
  r.missed = 4;
  r.cats[0] = CatRow{true, -901, 20, true, 88, 11, 40, 1, true, 1762434478};
  // cat 1 is a relay row: it has RSSI but no collar SNR, so snr publishes as null.
  r.cats[1] = CatRow{true, -1023, 0, false, 93, 33, 17, 0, true, 1762434456};
  // A cat never heard: last = 0, battery unknown. It must publish, not vanish.
  r.cats[2] = CatRow{false, 0, 0, false, BATT_UNKNOWN, 0, 0, 0, false, 0};

  char buf[512];
  int n = buildNodeState(buf, sizeof(buf), 1762434501, r);
  check(n > 0 && n < (int)sizeof(buf), "node state fits 512 bytes");
  check(jsonBalanced(buf), "node state is balanced JSON");
  check(strstr(buf, "\"c2\"") != nullptr, "a never-heard cat still publishes its row");
  check(strstr(buf, "(null)") == nullptr, "node state has no (null) text");
  check(strstr(buf, "\"miss\":4") != nullptr, "node report gaps publish as miss");
  check(strstr(buf, "\"snr\":20") != nullptr, "a measured SNR publishes as x2");
  check(strstr(buf, "\"snr\":null") != nullptr, "an unmeasured SNR publishes as JSON null, not 0");
  check(strstr(buf, "\"last\":1762434478") != nullptr, "per-cat last publishes as epoch");

  // Overflow must drop the record. A truncated snapshot published to a retained
  // topic is a dashboard that renders nothing and a broker that keeps the wreckage.
  // Probe at exactly the boundary: one byte short of the real length must be refused,
  // and the real length must be published.
  int real = n;
  char small[512];
  memset(small, 'x', sizeof(small));
  small[real - 1] = '\0';
  check(buildNodeState(small, (size_t)real - 1, 1762434501, r) == 0,
        "node state is refused one byte short, not truncated");
  memset(small, 0, sizeof(small));
  // One byte above the string length: snprintf needs the terminator, so cap == length
  // is one byte short and the builder must refuse it.
  check(buildNodeState(small, (size_t)real + 1, 1762434501, r) == real && jsonBalanced(small),
        "node state publishes at its exact length");
  printf("      node state is %d characters\n", real);
  check(real > 240, "the 4-node snapshot needs > 240 bytes, so the publish buffer must be sized for it");

  char base[256];
  n = buildBaseState(base, sizeof(base), 1762434501, 500, 7, 88, 1, -47, "192.168.100.232", 2);
  check(n > 0 && jsonBalanced(base), "base state is balanced JSON");
  check(strstr(base, "\"ip\":\"192.168.100.232\"") != nullptr, "base state carries its IP");
  check(strstr(base, "\"batt\":88") != nullptr, "base cell percentage publishes");

  Obs o{1, 2, 44, -90, 10, 88, -25, 12, 1762434501};
  char obs[128];
  n = buildObs(obs, sizeof(obs), o);
  check(n > 0 && jsonBalanced(obs), "obs is balanced JSON");
  // obs is in the frame's own units: -90 dBm, not -900. The locator must not inherit
  // a scaling it did not ask for.
  check(strstr(obs, "\"r\":-90") != nullptr, "obs keeps raw dBm, not x10");
  check(strstr(obs, "\"s\":10") != nullptr, "obs keeps raw dB, not x2");

  // ---------------------------------------------------------------- discovery
  Discovery rec[NUM_NODES * (NUM_COLLARS * 5 + 4) + 6];
  int nrec = buildAllDiscovery(rec, sizeof(rec) / sizeof(rec[0]), "base3", catName,
                               nodeName, avail, true);
  check(nrec == NUM_NODES * (NUM_COLLARS * 5 + 4) + 6,
        "every discovery record is built (nodes x cats x 5 fields + node rows + base rows)");

  bool allOk = true, uniqueOk = true, uidOk = true;
  std::string seen;
  for (int i = 0; i < nrec; i++) {
    const char *j = rec[i].json;
    if (strlen(j) == 0) allOk = false;
    if (!jsonBalanced(j)) allOk = false;
    if (strstr(j, "(null)") != nullptr) allOk = false;
    if (strstr(j, "%s") != nullptr) allOk = false;   // a format string left unexpanded
    if (strstr(j, "%u") != nullptr) allOk = false;   // homebase's first build left these
    // The uid is also the discovery topic fragment, so a space in it (from an entity
    // name used as a uid) makes a topic Home Assistant cannot match, and a literal
    // %u means every cat publishes to one topic — retained discovery, last cat wins.
    // Extract the value, not the tail of the record: the templates after it are full
    // of legitimate spaces.
    char uid[64];
    if (uidOf(j, uid, sizeof(uid))) {
      if (strchr(uid, ' ') || strstr(uid, "%u") || strstr(uid, "%s")) uidOk = false;
    } else {
      uidOk = false;
    }
    if (seen.find(std::string(rec[i].topic)) != std::string::npos) uniqueOk = false;
    seen += rec[i].topic + std::string("\n");
  }
  check(allOk, "every discovery record is complete, balanced, unformatted JSON");
  check(uidOk, "every unique_id is a clean slug: no spaces, no leftover format specifiers");
  check(uniqueOk, "every discovery record has its own topic");

  // The entity Home Assistant creates is device name + entity name, so the device
  // names are the entity ids. node 0 must land on the base device, not on a
  // "Node 0" device, or the dashboard has a phantom station.
  bool node0Base = false, node1Base = false, baseRows = false;
  for (int i = 0; i < nrec; i++) {
    if (strstr(rec[i].json, "\"unique_id\":\"base3_n0_cat0_signal\"") &&
        strstr(rec[i].json, "\"name\":\"Cat Base 3\"")) node0Base = true;
    if (strstr(rec[i].json, "\"unique_id\":\"base3_n1_cat0_signal\"") &&
        strstr(rec[i].json, "\"name\":\"Cat Base 3 Node 1\"")) node1Base = true;
    if (strstr(rec[i].topic, "homeassistant/sensor/base3_packets/config")) baseRows = true;
  }
  check(node0Base, "the house's own 433 receiver is node 0 on the base device");
  check(node1Base, "relay node 1 is its own device");
  check(baseRows, "base-level packets record exists");

  // A cat slot with no name publishes nothing, and the rest of the set must still be
  // complete: a renamed cat must not delete the other cats' rows.
  const char *named[NUM_COLLARS] = {"Chichi", "", "Chacha"};
  int nrec2 = buildAllDiscovery(rec, sizeof(rec) / sizeof(rec[0]), "base3", named,
                               nodeName, avail, true);
  check(nrec2 == (NUM_NODES * (2 * 5 + 4) + 6),
        "a cat with no name drops only its own rows");

  // A gateway with no RA-02 has no node 0. Publishing a node-0 row that is
  // permanently `in range: off` reads as a relay that has gone blind, which is the
  // exact thing the node rows exist to distinguish.
  int nrec3 = buildAllDiscovery(rec, sizeof(rec) / sizeof(rec[0]), "base3", catName,
                               nodeName, avail, false);
  check(nrec3 == (NUM_NODES - 1) * (NUM_COLLARS * 5 + 4) + 6,
        "without the 433 radio, node 0 rows are not published");
  bool noNode0 = true;
  for (int i = 0; i < nrec3; i++)
    if (strstr(rec[i].topic, "base3_n0_") != nullptr) noNode0 = false;
  check(noNode0, "no discovery record mentions node 0 when the base has no radio");

  printf("\n%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
  return fails == 0 ? 0 : 1;
}
