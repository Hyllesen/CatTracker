#pragma once
// Cat Tracker - the gateway's MQTT payloads, built in exactly one place.
//
// The gateway (`gateway/gateway.ino`) publishes Home Assistant discovery records
// and state JSON. Both are strings assembled by `snprintf`, and every failure mode
// of that assembly is silent on the device:
//
//   * a truncated record is published anyway, and Home Assistant logs "Unable to
//     parse JSON" and creates no entity. The base station looks alive and the
//     dashboard stays empty.
//   * `snprintf` returns the length it WOULD have written, so an overflow pushes the
//     running length past the buffer end and `cap - n` goes negative, which as a
//     size_t is a huge length — the next append writes outside the buffer.
//   * a `value_template` cut at 63 characters is valid JSON containing half a Jinja
//     expression, which renders as nothing. An MQTT sensor that renders nothing is
//     "unavailable", which is exactly what a dead cat looks like.
//
// So the assembly is header-only, free of Arduino, and host-tested by
// test_state_json.cpp:
//     c++ -std=c++17 -Wall -Wextra -o /tmp/t gateway/test_state_json.cpp && /tmp/t
//
// Units are base1's, unchanged: RSSI x 10, SNR x 2, integers, because ESP32's
// `printf` is not the constraint on this board but `value_template` is shared
// between base stations. `obs` keeps the frame's own units (dBm, dB) because the
// locator does its own maths and must not inherit a scaling it did not ask for.
//
// Node index 0 is this base's own 433 MHz receiver; nodes 1..MAX_NODES-1 are the
// jungle relays reporting over 915. Publishing the house receiver as node 0 puts it
// in the same table as the relays, which is what the locator needs.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ctgw {

// Overridable at flash time: `-DCTGW_MAX_NODES=6 -DCTGW_MAX_COLLARS=3`. The names
// are prefixed: an unprefixed `MAX_COLLARS` macro collides with collar_frame.h's
// `constexpr uint8_t MAX_COLLARS`, and the gateway includes both headers.
#ifndef CTGW_MAX_COLLARS
#define CTGW_MAX_COLLARS 3
#endif
#ifndef CTGW_MAX_NODES
#define CTGW_MAX_NODES 4      // index 0 = the base's own RA-02, 1..3 = relay nodes
#endif
constexpr int NUM_COLLARS = CTGW_MAX_COLLARS;
constexpr int NUM_NODES = CTGW_MAX_NODES;
constexpr uint8_t BATT_UNKNOWN = 0xFF;

// One collar, as one measurement point heard it. Scaled integers, ready to publish.
struct CatRow {
  bool have = false;
  int16_t rssi10 = 0;   // dBm x 10, smoothed
  int8_t snr2 = 0;      // dB x 2
  bool snrKnown = false;  // false => this point never measured it. A relay's ASCII
                          // frame carries no collar SNR, and 0 dB is a number nobody
                          // measured, so it publishes as `null` and the template
                          // renders unknown. Publishing 0 is a plausible-looking lie.
  uint8_t batt = BATT_UNKNOWN;
  uint32_t ageS = 0;
  uint32_t packets = 0;
  uint32_t missed = 0;
  bool in = false;
  long last = 0;        // epoch; 0 = never heard (Home Assistant renders `none`)
};

// One measurement point: this base's own radio (node 0) or a relay node's report.
struct NodeRow {
  bool have = false;
  bool in = false;      // node itself was heard recently — Blind vs Missing
  uint32_t ageS = 0;
  long last = 0;
  uint32_t packets = 0;  // reports from this node
  uint32_t missed = 0;   // seq gaps inside this node's reports
  CatRow cats[NUM_COLLARS];
};

// One raw observation, in the frame's own units. Feeds the locator, not the dashboard.
struct Obs {
  int node = -1;
  int collar = -1;
  int collarSeq = -1;
  int rssi = 0;         // dBm as the radio measured it, not x10
  int snr = 0;          // dB
  uint8_t batt = BATT_UNKNOWN;
  int hopRssi = 0;      // the 915 hop's own RSSI, dBm
  int hopSnr = 0;
  uint32_t nodeSeq = 0;  // the node's report counter, for gap counting
  long ts = 0;
};

// Append with clamping, and drop the record rather than publish a truncated one.
#define CTGW_APPEND(buf, cap, n, ...)                                       \
  do {                                                                     \
    if (n < 0 || n >= (int)(cap)) return 0;                                \
    n += snprintf((buf) + n, (size_t)((cap) - n), __VA_ARGS__);           \
    if (n >= (int)(cap)) return 0;                                         \
  } while (0)

// `cattracker/<base>/n<node>/state` — one topic per measurement point, so each
// record stays small, and each one keeps base1's shape (`c0`..`c2`).
inline int nodeStateTopic(char *topic, size_t cap, const char *base, int node) {
  int n = snprintf(topic, cap, "cattracker/%s/n%d/state", base, node);
  return n >= (int)cap ? 0 : n;
}

inline int baseStateTopic(char *topic, size_t cap, const char *base) {
  int n = snprintf(topic, cap, "cattracker/%s/state", base);
  return n >= (int)cap ? 0 : n;
}

inline int availTopic(char *topic, size_t cap, const char *base) {
  int n = snprintf(topic, cap, "cattracker/%s/availability", base);
  return n >= (int)cap ? 0 : n;
}

inline int obsTopic(char *topic, size_t cap, const char *base) {
  int n = snprintf(topic, cap, "cattracker/%s/obs", base);
  return n >= (int)cap ? 0 : n;
}

// The retained `state` snapshot for one measurement point. Ages are computed by the
// caller and are unclamped: a cat missing for an hour must read as an hour.
//
// `pk` is reports heard from this node, `miss` is seq gaps inside them. The name is
// `miss`, not base1's `unk`, because on a node row there is no unknown-id concept:
// a node's gaps are relay reports that never arrived, which is a different fact and
// must not share a name with the base's dropped frames.
inline int buildNodeState(char *buf, size_t cap, long ts, const NodeRow &r) {
  int n = 0;
  CTGW_APPEND(buf, cap, n,
             "{\"ts\":%ld,\"pk\":%lu,\"miss\":%lu,\"in\":%d,\"age\":%lu,\"last\":%ld",
             ts, (unsigned long)r.packets, (unsigned long)r.missed, r.in ? 1 : 0,
             (unsigned long)r.ageS, r.last);
  for (int i = 0; i < NUM_COLLARS; i++) {
    const CatRow &c = r.cats[i];
    // `snr` is emitted as JSON null when this point never measured it, so Home
    // Assistant renders "unknown" rather than a number that was never taken. The
    // text is built in a local buffer: `std::to_string(...).c_str()` points at a
    // temporary that dies at the end of the expression, which is exactly where the
    // append happens.
    char snrBuf[16];
    const char *snrTxt = "null";
    if (c.snrKnown) { snprintf(snrBuf, sizeof(snrBuf), "%d", (int)c.snr2); snrTxt = snrBuf; }
    CTGW_APPEND(buf, cap, n,
                ",\"c%d\":{\"rssi\":%d,\"snr\":%s,\"batt\":%u,\"age\":%lu,\"pk\":%lu,"
                "\"miss\":%lu,\"in\":%d,\"last\":%ld}",
                i, (int)c.rssi10, snrTxt,
                (unsigned)c.batt, (unsigned long)c.ageS, (unsigned long)c.packets,
                (unsigned long)c.missed, c.in ? 1 : 0, c.last);
  }
  CTGW_APPEND(buf, cap, n, "}");
  return n;
}

// Base-level state: the plumbing, not the cats. `batt` is the base station's own
// cell, `chg` is the TP5000's CHRG line (1 = charging), `w` is the WiFi RSSI.
inline int buildBaseState(char *buf, size_t cap, long ts, uint32_t pk, uint32_t unk,
                          uint8_t batt, int chrg, int wifiRssi, const char *ip,
                          int nodesHeard) {
  int n = 0;
  CTGW_APPEND(buf, cap, n,
              "{\"ts\":%ld,\"pk\":%lu,\"unk\":%lu,\"batt\":%u,\"chg\":%d,\"w\":%d,"
              "\"ip\":\"%s\",\"nodes\":%d}",
              ts, (unsigned long)pk, (unsigned long)unk, (unsigned)batt, chrg,
              wifiRssi, ip, nodesHeard);
  return n;
}

// One observation, non-retained, raw units.
inline int buildObs(char *buf, size_t cap, const Obs &o) {
  int n = 0;
  CTGW_APPEND(buf, cap, n,
              "{\"n\":%d,\"c\":%d,\"seq\":%d,\"cseq\":%d,\"r\":%d,\"s\":%d,\"b\":%u,"
              "\"hop\":%d,\"hs\":%d,\"ts\":%ld}",
              o.node, o.collar, (int)o.nodeSeq, o.collarSeq, o.rssi, o.snr,
              (unsigned)o.batt, o.hopRssi, o.hopSnr, o.ts);
  return n;
}

// ---------------------------------------------------------------------------
// value_template strings. Each is the exact string Home Assistant gets, so a
// template that does not fit the buffer is a build-time failure here, not a
// silently unavailable entity in the house.
//
// Returns 0 if the template does not fit `cap` — the caller must skip the record,
// not publish it.
// ---------------------------------------------------------------------------
inline int catTemplate(char *out, size_t cap, int cat, const char *field) {
  int n;
  if (strcmp(field, "signal") == 0) {
    n = snprintf(out, cap, "{{ value_json.c%d.rssi / 10 }}", cat);
  } else if (strcmp(field, "snr") == 0) {
    // The relay rows carry `snr` as JSON null; dividing null is a template error,
    // so the guard has to be in the template, not on the device.
    n = snprintf(out, cap,
                 "{{ (value_json.c%d.snr / 2) if value_json.c%d.snr is not none else none }}",
                 cat, cat);
  } else if (strcmp(field, "battery") == 0) {
    // 0xFF means "no reading", not "empty cell": render it as `none` so the gauge
    // reads unknown rather than 255%.
    n = snprintf(out, cap, "{{ value_json.c%d.batt if value_json.c%d.batt < 255 else none }}",
                 cat, cat);
  } else if (strcmp(field, "last seen") == 0) {
    // last == 0 is "never heard", not 1970: a cat heard 56 years ago reads as a
    // data fault, and it is not one.
    n = snprintf(out, cap,
                 "{{ (value_json.c%d.last | timestamp_local) if value_json.c%d.last > 0 else none }}",
                 cat, cat);
  } else if (strcmp(field, "in range") == 0) {
    n = snprintf(out, cap, "{{ 'ON' if value_json.c%d.in == 1 else 'OFF' }}", cat);
  } else {
    return 0;
  }
  return n >= (int)cap ? 0 : n;
}

inline int nodeTemplate(char *out, size_t cap, const char *field) {
  int n;
  if (strcmp(field, "in range") == 0) {
    n = snprintf(out, cap, "{{ 'ON' if value_json.in == 1 else 'OFF' }}");
  } else if (strcmp(field, "last seen") == 0) {
    n = snprintf(out, cap,
                 "{{ (value_json.last | timestamp_local) if value_json.last > 0 else none }}");
  } else if (strcmp(field, "packets") == 0) {
    n = snprintf(out, cap, "{{ value_json.pk }}");
  } else if (strcmp(field, "missed") == 0) {
    n = snprintf(out, cap, "{{ value_json.miss }}");
  } else {
    return 0;
  }
  return n >= (int)cap ? 0 : n;
}

inline int baseTemplate(char *out, size_t cap, const char *field) {
  int n;
  if (strcmp(field, "packets") == 0) {
    n = snprintf(out, cap, "{{ value_json.pk }}");
  } else if (strcmp(field, "unknown ids") == 0) {
    n = snprintf(out, cap, "{{ value_json.unk }}");
  } else if (strcmp(field, "battery") == 0) {
    n = snprintf(out, cap, "{{ value_json.batt if value_json.batt < 255 else none }}");
  } else if (strcmp(field, "charging") == 0) {
    n = snprintf(out, cap, "{{ 'ON' if value_json.chg == 1 else 'OFF' }}");
  } else if (strcmp(field, "WiFi signal") == 0) {
    n = snprintf(out, cap, "{{ value_json.w }}");
  } else if (strcmp(field, "IP address") == 0) {
    n = snprintf(out, cap, "{{ value_json.ip }}");
  } else {
    return 0;
  }
  return n >= (int)cap ? 0 : n;
}

// One discovery record. `uid` is the discovery unique id AND the topic fragment,
// `entName` is the entity name, `devName` the device it hangs on.
inline int discoveryTopic(char *topic, size_t cap, const char *kind, const char *uid) {
  int n = snprintf(topic, cap, "homeassistant/%s/%s/config", kind, uid);
  return n >= (int)cap ? 0 : n;
}

inline int buildSensorConfig(char *json, size_t cap, const char *stateTopic,
                             const char *availTopic, const char *uid,
                             const char *entName, const char *devIdent,
                             const char *devName, const char *unit,
                             const char *dclass, const char *tmpl, bool binary) {
  int n = 0;
  CTGW_APPEND(json, cap, n,
             "{\"name\":\"%s\",\"unique_id\":\"%s\",\"state_topic\":\"%s\","
             "\"availability_topic\":\"%s\",", entName, uid, stateTopic, availTopic);
  if (unit && !binary) CTGW_APPEND(json, cap, n, "\"unit_of_measurement\":\"%s\",", unit);
  if (dclass) CTGW_APPEND(json, cap, n, "\"device_class\":\"%s\",", dclass);
  CTGW_APPEND(json, cap, n,
              "\"value_template\":\"%s\",\"device\":{\"identifiers\":[\"%s\"],"
              "\"name\":\"%s\",\"manufacturer\":\"Cat Tracker\",\"model\":\"Gateway\"}}",
              tmpl, devIdent, devName);
  return n;
}

// The full set of discovery records for one gateway, written as one JSON object:
// {"topic":"...", "json":"..."} lines, so the host test can check every record's
// length, balance, and template completeness before any of them reach the broker.
// `catName[i]` is the collar name for slot i, `nodeName[0]` names the base's own
// radio. Records for a cat slot with no name are skipped, exactly as the firmware
// skips them.
//
// `hasBase` is false when the gateway has no RA-02: a base station that cannot hear
// 433 has no node 0, and publishing a node-0 row that is permanently `in range: off`
// would read as a relay that is blind, not as a receiver that does not exist.
struct Discovery {
  char topic[96];
  char json[420];
};

inline int buildAllDiscovery(Discovery *out, size_t cap, const char *base,
                             const char *const catName[NUM_COLLARS],
                             const char *const nodeName[NUM_NODES],
                             const char *availTopic, bool hasBase = true) {
  int nrec = 0;
  char stateTopic[64], tmpl[128];
  char baseIdent[48];
  snprintf(baseIdent, sizeof(baseIdent), "cattracker_%s", base);

  for (int node = hasBase ? 0 : 1; node < NUM_NODES; node++) {
    if (nodeStateTopic(stateTopic, sizeof(stateTopic), base, node) == 0) return nrec;
    // Node 0 is this base's own radio, so its rows hang on the base device.
    char devIdent[64], devName[48];
    if (node == 0) {
      snprintf(devIdent, sizeof(devIdent), "%s", baseIdent);
      snprintf(devName, sizeof(devName), "Cat Base 3");
    } else {
      snprintf(devIdent, sizeof(devIdent), "%s_node%d", baseIdent, node);
      snprintf(devName, sizeof(devName), "Cat Base 3 Node %d", node);
    }

    for (int i = 0; i < NUM_COLLARS; i++) {
      if (catName[i] == nullptr || *catName[i] == '\0') continue;
      // Parallel arrays: the entity name shown in Home Assistant, the template field
      // name, and the discovery uid. Comparing string literals with `==` is an
      // unspecified pointer comparison in C, so the mapping is by index, not by `==`.
      const char *fields[5] = {"signal", "SNR", "battery", "last seen", "in range"};
      const char *tmplField[5] = {"signal", "snr", "battery", "last seen", "in range"};
      const char *uidPart[5] = {"signal", "snr", "battery", "last_seen", "in_range"};
      const char *units[5] = {"dBm", "dB", "%", nullptr, nullptr};
      const char *classes[5] = {"signal_strength", nullptr, "battery", "timestamp",
                                "connectivity"};
      for (int f = 0; f < 5; f++) {
        if (catTemplate(tmpl, sizeof(tmpl), i, tmplField[f]) == 0) return nrec;
        char uid[64];
        snprintf(uid, sizeof(uid), "%s_n%d_cat%d_%s", base, node, i, uidPart[f]);
        char entName[48];
        snprintf(entName, sizeof(entName), "%s %s", catName[i], fields[f]);
        if (out == nullptr || nrec >= (int)cap) return nrec;
        if (discoveryTopic(out[nrec].topic, sizeof(out[nrec].topic),
                           f == 4 ? "binary_sensor" : "sensor", uid) == 0) return nrec;
        if (buildSensorConfig(out[nrec].json, sizeof(out[nrec].json), stateTopic,
                              availTopic, uid, entName, devIdent, devName, units[f],
                              classes[f], tmpl, f == 4) == 0) return nrec;
        nrec++;
      }
    }

    // The node's own row: Blind vs Missing. A node that stops reporting is the
    // plumbing failing, not the cat walking away, and the alert has to say which.
    const char *nodeFields[4] = {"in range", "last seen", "packets", "missed"};
    const char *nodeUid[4] = {"in_range", "last_seen", "packets", "missed"};
    for (int f = 0; f < 4; f++) {
      if (nodeTemplate(tmpl, sizeof(tmpl), nodeFields[f]) == 0) return nrec;
      char uid[64], entName[48];
      snprintf(uid, sizeof(uid), "%s_n%d_%s", base, node, nodeUid[f]);
      snprintf(entName, sizeof(entName), "%s %s", nodeName[node], nodeFields[f]);
      if (out == nullptr || nrec >= (int)cap) return nrec;
      if (discoveryTopic(out[nrec].topic, sizeof(out[nrec].topic),
                         f == 0 ? "binary_sensor" : "sensor", uid) == 0) return nrec;
      const char *cls = f == 0 ? "connectivity" : (f == 1 ? "timestamp" : nullptr);
      if (buildSensorConfig(out[nrec].json, sizeof(out[nrec].json), stateTopic,
                            availTopic, uid, entName, devIdent, devName,
                            nullptr, cls, tmpl, f == 0) == 0) return nrec;
      nrec++;
    }
  }

  // Base-level rows live on the base topic.
  const char *baseFields[6] = {"packets", "unknown ids", "battery", "charging",
                               "WiFi signal", "IP address"};
  const char *baseUid[6] = {"packets", "unknown_id_drops", "battery", "charging",
                            "wifi_signal", "ip_address"};
  const char *baseUnits[6] = {nullptr, nullptr, "%", nullptr, "dB", nullptr};
  const char *baseClasses[6] = {nullptr, nullptr, "battery", nullptr, nullptr, nullptr};
  for (int f = 0; f < 6; f++) {
    if (baseTemplate(tmpl, sizeof(tmpl), baseFields[f]) == 0) return nrec;
    char uid[64], entName[48], baseState[64];
    snprintf(uid, sizeof(uid), "%s_%s", base, baseUid[f]);
    snprintf(entName, sizeof(entName), "%s", baseFields[f]);
    if (out == nullptr || nrec >= (int)cap) return nrec;
    baseStateTopic(baseState, sizeof(baseState), base);
    if (discoveryTopic(out[nrec].topic, sizeof(out[nrec].topic),
                       f == 3 ? "binary_sensor" : "sensor", uid) == 0) return nrec;
    if (buildSensorConfig(out[nrec].json, sizeof(out[nrec].json), baseState,
                          availTopic, uid, entName, baseIdent, "Cat Base 3",
                          baseUnits[f], baseClasses[f], tmpl, f == 3) == 0) return nrec;
    nrec++;
  }
  return nrec;
}

}  // namespace ctgw
