#include "../src/CheckedCsvStorage.h"
#include <cassert>
#include <cstring>
#include <string>
#include <cstdlib>
static int flushCalls, syncCalls;
static int trackedFlush(FILE* file) { ++flushCalls; return std::fflush(file); }
static int trackedSync(int fd) { ++syncCalls; return ::fsync(fd); }
static int badFlush(FILE*) { ++flushCalls; errno = EIO; return EOF; }
static int badSync(int) { ++syncCalls; errno = EIO; return -1; }
static size_t shortWrite(const void* p, size_t, size_t n, FILE* file) { return std::fwrite(p,1,n/2,file); }
static int badClose(FILE* file) { std::fclose(file); errno = EIO; return EOF; }
int main() {
  char path[] = "/tmp/piglet-storage-test-XXXXXX";
  int fd = mkstemp(path); assert(fd >= 0); ::close(fd);
  const uint8_t row[] = {'a','\r','\n'};
  {
    CheckedCsvStorage::Operations ops = {std::fwrite, trackedFlush, trackedSync, std::fclose};
    CheckedCsvStorage file(ops); assert(file.open(path));
    assert(file.write(row,sizeof(row)) == sizeof(row)); assert(file.commit());
    assert(flushCalls == 1 && syncCalls == 1 && file.position() == sizeof(row));
    FILE* read = std::fopen(path,"rb"); assert(read); char actual[3];
    assert(std::fread(actual,1,3,read)==3 && !std::memcmp(actual,row,3)); std::fclose(read);
    assert(file.close());
  }
  {
    flushCalls=syncCalls=0;
    CheckedCsvStorage::Operations ops = {shortWrite, trackedFlush, trackedSync, std::fclose};
    CheckedCsvStorage file(ops); assert(file.open(path));
    assert(file.write(row,sizeof(row)) < sizeof(row));
    assert(!file.healthy() && !file.commit() && file.write(row,sizeof(row)) == 0);
    assert(flushCalls==0 && syncCalls==0); assert(!file.close());
  }
  {
    flushCalls=syncCalls=0;
    CheckedCsvStorage::Operations ops = {std::fwrite, badFlush, trackedSync, std::fclose};
    CheckedCsvStorage file(ops); assert(file.open(path)); assert(file.write(row,3)==3);
    assert(!file.commit() && flushCalls==1 && syncCalls==0);
    assert(!file.healthy() && file.write(row,3)==0 && !file.close());
  }
  {
    flushCalls=syncCalls=0;
    CheckedCsvStorage::Operations ops = {std::fwrite, trackedFlush, badSync, std::fclose};
    CheckedCsvStorage file(ops); assert(file.open(path)); assert(file.write(row,3)==3);
    assert(!file.commit() && flushCalls==1 && syncCalls==1);
    assert(file.error()==EIO && !file.healthy() && file.write(row,3)==0 && !file.close());
  }
  {
    CheckedCsvStorage::Operations ops = {std::fwrite, trackedFlush, trackedSync, badClose};
    CheckedCsvStorage file(ops); assert(file.open(path)); assert(!file.close()); assert(!file.isOpen());
  }
  {
    CheckedCsvStorage file;
    assert(!file.open((std::string(path)+"/missing").c_str())); assert(!file.healthy());
  }
  std::remove(path);
}
