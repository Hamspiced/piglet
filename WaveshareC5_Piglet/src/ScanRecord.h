#pragma once
#include <cstddef>
#include <cstdlib>
#include <cstring>

// Existing wire format: MAC,SSID,AUTH,CHANNEL,RSSI[,W]. Parse the fixed tail
// from the right so commas inside the SSID cannot shift the other fields.
namespace ScanRecord {
struct Fields { char mac[18], ssid[33], auth[32]; int channel, rssi; };
inline bool copy(char* out, size_t capacity, const char* first, const char* last) {
  size_t n = static_cast<size_t>(last - first);
  if (n >= capacity) return false;
  std::memcpy(out, first, n); out[n] = 0; return true;
}
inline bool integer(const char* first, const char* last, int low, int high, int& value) {
  char token[12]; if (first == last || !copy(token, sizeof(token), first, last)) return false;
  const char* digit = token; if (*digit == '-') ++digit;
  if (!*digit) return false;
  for (const char* p = digit; *p; ++p) if (*p < '0' || *p > '9') return false;
  char* end; long n = std::strtol(token, &end, 10);
  if (*end || n < low || n > high) return false;
  value = static_cast<int>(n); return true;
}
inline bool parse(const char* input, Fields& out) {
  if (!input) return false;
  const char* end = input + std::strlen(input);
  if (end-input >= 2 && end[-2] == ',' && end[-1] == 'W') end -= 2;
  const char* first = std::strchr(input, ','); if (!first || first-input != 17) return false;
  for (int i=0;i<17;++i) {
    char c=input[i];
    if (i%3==2) { if(c!=':') return false; }
    else if (!((c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f'))) return false;
  }
  const char* cuts[3]; const char* cursor = end;
  for (int i=2;i>=0;--i) {
    do { if (cursor <= first+1) return false; --cursor; } while (*cursor != ',');
    cuts[i]=cursor;
  }
  if (!copy(out.mac,sizeof(out.mac),input,first) || !copy(out.ssid,sizeof(out.ssid),first+1,cuts[0]) ||
      !copy(out.auth,sizeof(out.auth),cuts[0]+1,cuts[1]) || cuts[0]+1==cuts[1]) return false;
  return integer(cuts[1]+1,cuts[2],1,196,out.channel) && integer(cuts[2]+1,end,-127,0,out.rssi);
}
}
