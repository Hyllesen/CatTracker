#pragma once
// Cat Tracker - the collar frame, decoded in exactly one place on this board.
//
// The frame is six bytes, and it is defined in four other places: the collar
// that builds it (collar/collar.ino), the handheld that parses it
// (handheld_ble/), the app's decoder (CatTrackApp/) and the tool that replays
// bytes (tools/). This is the base station's copy. It is deliberately the only
// copy here: the ESPHome YAML wires sensors to Roster and does no byte
// arithmetic of its own.
//
//   byte 0   0xC1        magic / protocol version
//   byte 1   seq         wrapping 0..255, increments every ping
//   byte 2   id          0..MAX_COLLARS-1, baked in at flash time
//   byte 3   battery     0..100, 0xFF = no reading
//   byte 4   flags       bit0 = LOW_BATT (<15%), rest reserved
//   byte 5   reserved
//
// The magic byte is what stops a decoded stray frame from becoming a phantom
// cat, and LoRa's own payload CRC already covers transmission errors, so there
// is no checksum of our own - which is why CRC is switched on in the YAML.
//
// Header-only, and free of Arduino and ESPHome includes, so that
// test_collar_frame.cpp can compile it on the host with a plain C++ compiler.
// A change that breaks the decode is then caught by a test rather than by a cat
// going quietly missing.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ct {

constexpr uint8_t MAX_COLLARS = 3;
constexpr uint8_t FRAME_LEN = 6;
constexpr uint8_t FRAME_MAGIC = 0xC1;
constexpr uint8_t BATT_UNKNOWN = 0xFF;
constexpr uint8_t FLAG_LOW_BATT = 0x01;

// 12 packets is ~96 s of history at the collar's 8 s ping. The measured noise
// floor here is only ~0.7 dB, so this smooths multipath spikes without hiding a
// cat that has walked away.
constexpr uint8_t SMOOTH_N = 12;

// Must comfortably exceed one collar ping interval, or a couple of missed
// packets trip "signal lost" while the collar is fine.
constexpr uint32_t STALE_MS = 15000;

struct Collar {
  bool have = false;
  uint8_t seq = 0;
  bool seq_valid = false;
  uint8_t battery_pct = BATT_UNKNOWN;
  uint8_t flags = 0;
  float rssi_hist[SMOOTH_N] = {};
  uint8_t idx = 0;
  uint8_t count = 0;
  float rssi_smoothed = 0.0f;
  float snr_last = 0.0f;
  uint32_t last_ms = 0;
  uint32_t packets = 0;
  uint32_t missed = 0;

  bool low_battery() const { return have && (flags & FLAG_LOW_BATT) != 0; }
  bool battery_known() const { return have && battery_pct != BATT_UNKNOWN; }
};

class Roster {
 public:
  Collar collars[MAX_COLLARS];
  uint32_t packets = 0;   // every frame that decoded as one of ours
  uint32_t unknown = 0;   // frames dropped: wrong magic, short, or nobody's id

  // Apply one received frame. Returns the collar id, or -1 if the frame is not
  // one of ours. Counters are bumped here rather than by the caller so there is
  // one place that decides what counts as a packet.
  int8_t apply(const uint8_t *buf, size_t len, float rssi, float snr, uint32_t now_ms) {
    if (buf == nullptr || len < FRAME_LEN || buf[0] != FRAME_MAGIC) {
      unknown++;
      return -1;
    }
    const uint8_t id = buf[2];
    if (id >= MAX_COLLARS) {
      unknown++;
      return -1;
    }

    Collar &c = collars[id];
    // Sequence gaps are how a packet that never arrived is counted, so this
    // subtraction has to wrap at 256: 255 -> 0 is 0 missed, 255 -> 2 is 2. The
    // distance is taken as a signed byte rather than computed as
    // `seq - prev - 1` in unsigned, because in unsigned a repeated seq gives
    // 255 - five packets' worth of alarm every time a frame arrives twice.
    // A step of 0 (repeat) or a negative one (out of order) adds nothing.
    // A collar that restarts reports its seq reset as a gap, which is correct:
    // packets really were not heard while it was off.
    if (c.seq_valid) {
      const int8_t step = (int8_t) (uint8_t) (buf[1] - c.seq);
      if (step > 1)
        c.missed += (uint32_t) (step - 1);
    }
    c.seq = buf[1];
    c.seq_valid = true;

    c.battery_pct = buf[3];
    c.flags = buf[4];
    c.snr_last = snr;
    c.last_ms = now_ms;
    c.packets++;
    c.have = true;
    push_rssi(c, rssi);
    packets++;
    return (int8_t) id;
  }

  bool in_range(uint8_t i, uint32_t now_ms) const {
    if (i >= MAX_COLLARS)
      return false;
    const Collar &c = collars[i];
    return c.have && (now_ms - c.last_ms) <= STALE_MS;
  }

  // Seconds since this collar was last heard, unclamped: a cat missing for an
  // hour must read as missing for an hour. The old firmware clamped this at
  // 255 s and every missing cat looked four minutes stale.
  uint32_t age_s(uint8_t i, uint32_t now_ms) const {
    if (i >= MAX_COLLARS || !collars[i].have)
      return 0;
    return (now_ms - collars[i].last_ms) / 1000u;
  }

 private:
  // Running mean over the last SMOOTH_N samples, kept incrementally: adding the
  // new value and subtracting the one it displaces is one divide, not a loop
  // over the history, which matters on an 80 MHz single-core part.
  static void push_rssi(Collar &c, float rssi) {
    if (c.count < SMOOTH_N) {
      c.count++;
      c.rssi_smoothed += (rssi - c.rssi_smoothed) / (float) c.count;
    } else {
      c.rssi_smoothed += (rssi - c.rssi_hist[c.idx]) / (float) SMOOTH_N;
    }
    c.rssi_hist[c.idx] = rssi;
    c.idx = (uint8_t) ((c.idx + 1) % SMOOTH_N);
  }
};

}  // namespace ct

// Unqualified alias, so the lambdas in the YAML read `Collar`, not `ct::Collar`.
using CatRoster = ct::Roster;

// The one roster the board has. Deliberately *not* an ESPHome `globals:` entry:
// that wraps the value in a GlobalsComponent, and this is a plain value, not a
// component. A function-local static, so there is no initialisation-order
// question at boot and the lambdas can just call cat_roster().
inline CatRoster &cat_roster() {
  static CatRoster roster;
  return roster;
}
