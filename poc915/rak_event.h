#pragma once
// Decode one RAK3172 P2P receive event line, and the POC's payload text.
//
// Header-only so the parsing is testable on the host: a parse bug is a
// gateway that "receives nothing", which is indistinguishable from a dead
// module. Run `c++ -std=c++17 -o /tmp/t poc915/test_rak_event.cpp && /tmp/t`.
//
// Event format (RUI3 firmware): +EVT:RXP2P:<rssi>:<snr>:<hex payload>
// Payload in the POC is ASCII text "CAT<node> <seq>", sent as AT+PSEND=<hex>.

#include <cstdio>
#include <cstring>

struct RakFrame {
  bool ok;        // line was a parseable RXP2P event
  int rssi;       // dBm, as the module reports it
  int snr;        // dB
  int node;       // -1 unless the payload matched CAT<node> <seq>
  long seq;       // -1 likewise
  int collar;     // -1 unless the payload carried "c<id>" (node's 433 hearing)
  int collarRssi; // 0 unless the payload carried "r<dBm>"
  char text[64];  // payload decoded to ASCII (non-printables become '.')
};

inline int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline RakFrame parseRxp2p(const char *line) {
  RakFrame f = {false, 0, 0, -1, -1, -1, 0, ""};
  if (strncmp(line, "+EVT:RXP2P:", 11) != 0) return f;

  char payload[128] = "";
  if (sscanf(line, "+EVT:RXP2P:%d:%d:%127s", &f.rssi, &f.snr, payload) < 2) return f;

  // two hex chars per byte; strtol is not safe here (it eats the whole string)
  size_t i = 0;
  while (payload[i] && payload[i + 1] && strlen(f.text) < 60) {
    int hi = hexval(payload[i]), lo = hexval(payload[i + 1]);
    if (hi < 0 || lo < 0) break;
    int b = hi * 16 + lo;
    f.text[strlen(f.text)] = (b >= 32 && b < 127) ? (char)b : '.';
    i += 2;
  }

  f.ok = true;
  long s = -1;
  int n = -1;
  if (sscanf(f.text, "CAT%d %ld", &n, &s) == 2) { f.node = n; f.seq = s; }

  // Optional tail from a relay node: "CAT1 137 c2 r-88". The collar id is -1 when the
  // node has not heard that collar, so "c-1" is a legitimate value meaning "nothing".
  // Parsed separately so a plain talker frame ("CAT1 137") is still accepted.
  int c = -1, r = 0;
  if (sscanf(f.text, "CAT%d %ld c%d r%d", &n, &s, &c, &r) == 4) {
    f.node = n; f.seq = s; f.collar = c; f.collarRssi = r;
  }
  return f;
}

// Encode ASCII text to the hex string AT+PSEND wants. Returns length, 0 on overflow.
inline int hexEncode(const char *text, char *out, size_t outCap) {
  size_t n = strlen(text);
  if (n * 2 + 1 > outCap) return 0;
  for (size_t i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02X", (unsigned char)text[i]);
  out[n * 2] = '\0';
  return (int)(n * 2);
}
