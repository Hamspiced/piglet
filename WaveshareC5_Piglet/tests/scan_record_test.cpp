#include "../src/ScanRecord.h"
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <cstdlib>
int main() {
  ScanRecord::Fields row;
  assert(ScanRecord::parse("02:00:00:00:00:01,Shop,Guest,WPA2,6,-50,W",row));
  assert(!std::strcmp(row.ssid,"Shop,Guest") && !std::strcmp(row.auth,"WPA2") && row.channel==6 && row.rssi==-50);
  assert(ScanRecord::parse("02:00:00:00:00:01,,OPEN,1,-127,W",row) && !*row.ssid);
  assert(ScanRecord::parse("02:00:00:00:00:01,quote\"name\nline,WPA3,177,-60",row));
  assert(!std::strcmp(row.ssid,"quote\"name\nline"));
  assert(ScanRecord::parse("02:00:00:00:00:01,12345678901234567890123456789012,OPEN,14,-1,W",row));
  assert(!ScanRecord::parse("02:00:00:00:00:01,123456789012345678901234567890123,OPEN,14,-1,W",row));
  for (auto value : {"bad,ssid,OPEN,1,-50,W", "02:00:00:00:00:01,x,OPEN,0,-50,W", "02:00:00:00:00:01,x,OPEN,6x,-50,W", "02:00:00:00:00:01,x,OPEN,6,-50junk,W", "02:00:00:00:00:01,x,,6,-50,W", "02:00:00:00:00:01,x,OPEN,6,1,W", "", "02:00:00:00:00:01"}) assert(!ScanRecord::parse(value,row));
  // Exercise malformed suffix/boundary combinations under ASan/UBSan.
  const char alphabet[] = "abcABC0123456789,:-\"\n";
  for (int trial=0;trial<10000;++trial) {
    char text[201]; size_t length=std::rand()%201;
    for (size_t i=0;i<length;++i) text[i]=alphabet[std::rand()%(sizeof(alphabet)-1)];
    if (length>=18 && trial%2==0) std::memcpy(text,"02:00:00:00:00:01,",18);
    text[length]=0; ScanRecord::parse(text,row);
  }
}
