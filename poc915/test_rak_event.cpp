// Host test for poc915/rak_event.h — the POC's RX parse.
// Run: c++ -std=c++17 -o /tmp/poctest poc915/test_rak_event.cpp && /tmp/poctest
#include "rak_event.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
  // A normal frame: "CAT1 1" is 43 41 54 31 20 31
  RakFrame f = parseRxp2p("+EVT:RXP2P:-60:11:434154312031");
  assert(f.ok);
  assert(f.rssi == -60 && f.snr == 11);
  assert(f.node == 1 && f.seq == 1);
  assert(strcmp(f.text, "CAT1 1") == 0);

  // Node 3, seq 12, weak signal
  f = parseRxp2p("+EVT:RXP2P:-129:-10:43415433203132");
  assert(f.ok && f.rssi == -129 && f.snr == -10);
  assert(f.node == 3 && f.seq == 12);

  // seq 255, the last value before wrap
  f = parseRxp2p("+EVT:RXP2P:-90:5:4341543120323535");
  assert(f.node == 1 && f.seq == 255);

  // Lowercase hex must work as well
  f = parseRxp2p("+EVT:RXP2P:-40:12:434154312031");
  assert(f.node == 1);

  // Non-printable bytes become dots, not garbage
  f = parseRxp2p("+EVT:RXP2P:-40:0:00FF0A");
  assert(f.ok && strcmp(f.text, "...") == 0 && f.node == -1);

  // Odd hex length: trailing nibble is dropped, not shifted
  f = parseRxp2p("+EVT:RXP2P:-40:0:43415");
  assert(f.ok && strcmp(f.text, "CA") == 0);

  // Non-hex payload: ok, no CAT parse
  f = parseRxp2p("+EVT:RXP2P:-40:0:ZZ");
  assert(f.ok && f.text[0] == '\0' && f.node == -1);

  // Other events and short lines are not frames
  assert(!parseRxp2p("+EVT:TXP2P DONE").ok);
  assert(!parseRxp2p("+EVT:RXP2P RECEIVE TIMEOUT").ok);
  assert(!parseRxp2p("+EVT:RXP2P:-70").ok);
  assert(!parseRxp2p("AT+P2P=915000000:7:125:0:8:14").ok);

  // Long payload is truncated, not overflowed
  char big[200];
  for (int i = 0; i < 140; i++) big[i] = 'A';   // 140 hex chars = 70 bytes
  big[140] = '\0';
  char line[300];
  snprintf(line, sizeof(line), "+EVT:RXP2P:-50:9:%s", big);
  f = parseRxp2p(line);
  assert(f.ok && strlen(f.text) == 60);

  // Encode/decode round trip, the TX side's half
  char hex[128];
  assert(hexEncode("CAT1 12", hex, sizeof(hex)) == 14);
  assert(strcmp(hex, "43415431203132") == 0);
  snprintf(line, sizeof(line), "+EVT:RXP2P:-70:8:%s", hex);
  f = parseRxp2p(line);
  assert(f.node == 1 && f.seq == 12 && strcmp(f.text, "CAT1 12") == 0);

  // ---- relay-node tail with the collar's own seq: "CAT1 137 c2 s229 r-88" ----
  char hex2[128], line2[300];
  hexEncode("CAT1 137 c2 s229 r-88", hex2, sizeof(hex2));
  snprintf(line2, sizeof(line2), "+EVT:RXP2P:-71:9:%s", hex2);
  f = parseRxp2p(line2);
  assert(f.ok && f.node == 1 && f.seq == 137);
  assert(f.collar == 2 && f.collarSeq == 229 && f.collarRssi == -88);

  // Collar seq wraps: 0 and 255 must survive
  hexEncode("CAT1 137 c0 s0 r-129", hex2, sizeof(hex2));
  snprintf(line2, sizeof(line2), "+EVT:RXP2P:-129:-10:%s", hex2);
  f = parseRxp2p(line2);
  assert(f.collar == 0 && f.collarSeq == 0 && f.collarRssi == -129);

  // The older 4-field form (no collar seq) still parses, with collarSeq left -1
  hexEncode("CAT1 3 c1 r-85", hex2, sizeof(hex2));
  snprintf(line2, sizeof(line2), "+EVT:RXP2P:-80:6:%s", hex2);
  f = parseRxp2p(line2);
  assert(f.node == 1 && f.seq == 3 && f.collar == 1 && f.collarSeq == -1 && f.collarRssi == -85);

  // "nothing heard" is c-1, and must not be mistaken for a collar
  hexEncode("CAT1 4 c-1 r0", hex2, sizeof(hex2));
  snprintf(line2, sizeof(line2), "+EVT:RXP2P:-80:6:%s", hex2);
  f = parseRxp2p(line2);
  assert(f.node == 1 && f.seq == 4 && f.collar == -1 && f.collarRssi == 0);

  // A plain talker frame has no tail: collar stays -1, seq still parses
  hexEncode("CAT1 12", hex2, sizeof(hex2));
  snprintf(line2, sizeof(line2), "+EVT:RXP2P:-70:8:%s", hex2);
  f = parseRxp2p(line2);
  assert(f.node == 1 && f.seq == 12 && f.collar == -1);

  printf("all rak_event tests passed\n");
  return 0;
}
