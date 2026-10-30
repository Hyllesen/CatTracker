// Host-side test for collar_frame.h - the base station's copy of the collar
// frame decode.
//
//     c++ -std=c++17 -Wall -Wextra -o /tmp/test_collar_frame \
//         homebase/esphome/test_collar_frame.cpp && /tmp/test_collar_frame
//
// The failure this exists to catch is silent. A frame whose bytes are decoded
// one field off does not crash and does not log: the cat keeps reporting, with
// somebody else's battery or a sequence gap that is not there, and the only
// symptom is an alert that fires or does not fire weeks later. So every field of
// the frame is exercised here, including the 255 -> 0 sequence wrap.

#include <cstdio>
#include <cstdlib>

#include "collar_frame.h"

static int failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
      failures++;                                                                \
    }                                                                            \
  } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

// A frame as the collar builds it.
static void frame(uint8_t *out, uint8_t seq, uint8_t id, uint8_t batt = 82, uint8_t flags = 0) {
  out[0] = 0xC1;
  out[1] = seq;
  out[2] = id;
  out[3] = batt;
  out[4] = flags;
  out[5] = 0x00;
}

static void test_rejects_foreign_frames() {
  ct::Roster r;
  uint8_t f[6];

  frame(f, 0, 0);
  f[0] = 0xC2;  // wrong magic
  CHECK(r.apply(f, sizeof(f), -60.0f, 8.0f, 1000) == -1);
  CHECK(r.unknown == 1);
  CHECK(r.packets == 0);

  frame(f, 0, 0);
  CHECK(r.apply(f, 4, -60.0f, 8.0f, 1000) == -1);  // short
  CHECK(r.unknown == 2);

  frame(f, 0, 3);  // nobody is flashed as collar 3
  CHECK(r.apply(f, sizeof(f), -60.0f, 8.0f, 1000) == -1);
  CHECK(r.unknown == 3);

  // A rejected frame must not have touched any collar's state.
  for (uint8_t i = 0; i < ct::MAX_COLLARS; i++) {
    CHECK(!r.collars[i].have);
    CHECK(r.collars[i].packets == 0);
  }
  CHECK(r.packets == 0);
}

static void test_fields_land_where_they_belong() {
  ct::Roster r;
  uint8_t f[6];

  frame(f, 7, 1, 43, ct::FLAG_LOW_BATT);
  CHECK(r.apply(f, sizeof(f), -71.5f, 9.25f, 5000) == 1);
  CHECK(r.packets == 1);
  CHECK(r.unknown == 0);

  const ct::Collar &c = r.collars[1];
  CHECK(c.have);
  CHECK(c.seq == 7);
  CHECK(c.battery_pct == 43);
  CHECK(c.seq_valid);
  CHECK(c.packets == 1);
  CHECK(c.missed == 0);
  CHECK(c.last_ms == 5000);
  CHECK_NEAR(c.rssi_smoothed, -71.5f, 0.001f);
  CHECK_NEAR(c.snr_last, 9.25f, 0.001f);

  // The flag travelled to the right collar, and only to it.
  CHECK(c.low_battery());
  CHECK(!r.collars[0].have);
  CHECK(!r.collars[2].have);

  // A collar with no battery divider wired sends 0xFF, which must stay "no
  // reading" rather than becoming a 255% battery.
  frame(f, 8, 0, ct::BATT_UNKNOWN);
  CHECK(r.apply(f, sizeof(f), -50.0f, 5.0f, 6000) == 0);
  CHECK(r.collars[0].have);
  CHECK(!r.collars[0].battery_known());
  CHECK(!r.collars[0].low_battery());  // 0xFF with no flag bits is not a warning
}

static void test_sequence_gaps() {
  ct::Roster r;
  uint8_t f[6];

  frame(f, 10, 0);
  CHECK(r.apply(f, sizeof(f), -60.0f, 8.0f, 1000) == 0);
  CHECK(r.collars[0].missed == 0);  // the first packet cannot imply a gap

  frame(f, 10, 0);  // a duplicate is not a gap either
  CHECK(r.apply(f, sizeof(f), -60.0f, 8.0f, 2000) == 0);
  CHECK(r.collars[0].missed == 0);

  frame(f, 13, 0);  // 11 and 12 never arrived
  CHECK(r.apply(f, sizeof(f), -60.0f, 8.0f, 3000) == 0);
  CHECK(r.collars[0].missed == 2);

  // The 255 -> 0 wrap is 0 missing, not 255. This is the arithmetic that a
  // naive unsigned subtraction gets wrong, and it would report a healthy collar
  // as missing five packets every 34 minutes.
  frame(f, 255, 1);
  CHECK(r.apply(f, sizeof(f), -60.0f, 8.0f, 4000) == 1);
  frame(f, 0, 1);
  CHECK(r.apply(f, sizeof(f), -60.0f, 8.0f, 5000) == 1);
  CHECK(r.collars[1].missed == 0);

  frame(f, 2, 1);  // after the wrap: 1 arrived, so only 1 is missing
  CHECK(r.apply(f, sizeof(f), -60.0f, 8.0f, 6000) == 1);
  CHECK(r.collars[1].missed == 1);

  // Gaps are per collar. Collar 0's two missing packets are not collar 1's.
  CHECK(r.collars[0].missed == 2);
}

static void test_rssi_smoothing() {
  ct::Roster r;
  uint8_t f[6];

  // Twelve packets at one level: the mean must settle on that level exactly.
  for (uint8_t i = 0; i < ct::SMOOTH_N; i++) {
    frame(f, i, 0);
    r.apply(f, sizeof(f), -60.0f, 8.0f, (uint32_t) i * 1000);
  }
  CHECK_NEAR(r.collars[0].rssi_smoothed, -60.0f, 0.001f);

  // A full window is in the history now, so one packet from a new level moves
  // the mean by exactly 1/N of the step - not by the whole step.
  frame(f, ct::SMOOTH_N, 0);
  r.apply(f, sizeof(f), -72.0f, 8.0f, 20000);
  CHECK_NEAR(r.collars[0].rssi_smoothed, -61.0f, 0.001f);
  CHECK(r.collars[0].packets == ct::SMOOTH_N + 1);
}

static void test_age_and_range() {
  ct::Roster r;
  uint8_t f[6];

  // Never heard: not in range, and no age to report (0 means "nothing known",
  // which is why the YAML maps it to unknown rather than to 1970).
  CHECK(!r.in_range(0, 1000));
  CHECK(r.age_s(0, 1000) == 0);

  frame(f, 0, 0);
  CHECK(r.apply(f, sizeof(f), -60.0f, 8.0f, 10000) == 0);

  CHECK(r.in_range(0, 10000));
  CHECK(r.age_s(0, 10000) == 0);
  CHECK(r.age_s(0, 14000) == 4);
  CHECK(r.in_range(0, 25000));   // 15 s: still inside the stale window
  CHECK(!r.in_range(0, 25001));  // one millisecond past it
  CHECK(r.age_s(0, 25001) == 15);

  // Unclamped. A cat missing for 74 minutes reads as 74 minutes.
  CHECK(r.age_s(0, 10000 + 4440u * 1000u) == 4440);
  CHECK(!r.in_range(0, 10000 + 4440u * 1000u));

  // Only the collar that was heard is in range.
  CHECK(!r.in_range(1, 10000));
}

static void test_three_cats_do_not_merge() {
  ct::Roster r;
  uint8_t f[6];

  frame(f, 0, 0, 90);
  r.apply(f, sizeof(f), -55.0f, 9.0f, 1000);
  frame(f, 0, 1, 80);
  r.apply(f, sizeof(f), -75.0f, 6.0f, 1500);
  frame(f, 0, 2, 70);
  r.apply(f, sizeof(f), -95.0f, 3.0f, 2000);

  CHECK(r.packets == 3);
  CHECK_NEAR(r.collars[0].rssi_smoothed, -55.0f, 0.001f);
  CHECK_NEAR(r.collars[1].rssi_smoothed, -75.0f, 0.001f);
  CHECK_NEAR(r.collars[2].rssi_smoothed, -95.0f, 0.001f);
  CHECK(r.collars[0].battery_pct == 90);
  CHECK(r.collars[1].battery_pct == 80);
  CHECK(r.collars[2].battery_pct == 70);
  CHECK(r.collars[0].last_ms == 1000);
  CHECK(r.collars[2].last_ms == 2000);
}

int main() {
  test_rejects_foreign_frames();
  test_fields_land_where_they_belong();
  test_sequence_gaps();
  test_rssi_smoothing();
  test_age_and_range();
  test_three_cats_do_not_merge();

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all collar_frame checks passed\n");
  return EXIT_SUCCESS;
}
