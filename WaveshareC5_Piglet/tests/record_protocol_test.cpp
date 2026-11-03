#include "../src/RecordProtocol.h"
#include <cassert>
int main() {
  using namespace RecordProtocol;
  assert(sizeof(Packet) <= 250);
  assert(requestSession("CTRL2:1234ABCD", 14) == 0x1234ABCD);
  assert(requestSession("CTRL2:FFFFFFFF", 14) == 0xffffffff);
  assert(requestSession("CTRL2:00000000", 14) == 0);
  assert(requestSession("CTRL1", 5) == 0);
  assert(requestSession("CTRL2:1234ABCG", 14) == 0);
  assert(requestSession("CTRL2:1234ABCD", 13) == 0);
  assert(requestSession("OTHER:1234ABCD", 14) == 0);
  assert(requestSession("CTRL2:1234ABCDx", 15) == 0);
}
