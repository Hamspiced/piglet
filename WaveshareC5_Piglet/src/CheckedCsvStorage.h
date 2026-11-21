#pragma once
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <unistd.h>

// Own the stdio handle so both flush and fsync failures remain observable.
// A failed/partial write or commit latches the handle failed. Never append a
// retry to a file containing an uncertain row; recovery requires reopening.
class CheckedCsvStorage {
public:
  struct Operations {
    size_t (*write)(const void*, size_t, size_t, FILE*);
    int (*flush)(FILE*);
    int (*sync)(int);
    int (*close)(FILE*);
  };
  static const Operations& defaults() {
    static const Operations ops = {std::fwrite, std::fflush, ::fsync, std::fclose};
    return ops;
  }
  explicit CheckedCsvStorage(const Operations& ops = defaults()) : ops_(ops) {}
  CheckedCsvStorage(const CheckedCsvStorage&) = delete;
  CheckedCsvStorage& operator=(const CheckedCsvStorage&) = delete;
  ~CheckedCsvStorage() { close(); }
  bool open(const char* path) {
    if (file_ && !close()) return false;
    failed_ = false; error_ = 0; errno = 0;
    file_ = std::fopen(path, "wb");
    if (!file_) return fail();
    return true;
  }
  bool isOpen() const { return file_ != nullptr; }
  bool healthy() const { return file_ && !failed_; }
  int error() const { return error_; }
  size_t write(const uint8_t* bytes, size_t length) {
    if (!healthy()) return 0;
    errno = 0;
    size_t written = ops_.write(bytes, 1, length, file_);
    if (written != length || std::ferror(file_)) fail();
    return written;
  }
  bool commit() {
    if (!healthy()) return false;
    errno = 0;
    if (ops_.flush(file_) != 0) return fail();
    if (ops_.sync(::fileno(file_)) != 0) return fail();
    return true;
  }
  size_t position() {
    if (!healthy()) return SIZE_MAX;
    errno = 0;
    long offset = std::ftell(file_);
    if (offset < 0) { fail(); return SIZE_MAX; }
    return static_cast<size_t>(offset);
  }
  bool close() {
    if (!file_) return !failed_;
    bool ok = commit();
    errno = 0;
    if (ops_.close(file_) != 0) { fail(); ok = false; }
    file_ = nullptr;
    return ok;
  }
private:
  bool fail() { failed_ = true; error_ = errno ? errno : EIO; return false; }
  const Operations& ops_;
  FILE* file_ = nullptr;
  bool failed_ = false;
  int error_ = 0;
};

#ifdef ARDUINO
#include <Print.h>
class CheckedCsvLog : public Print {
public:
  explicit CheckedCsvLog(const CheckedCsvStorage::Operations& ops = CheckedCsvStorage::defaults()) : storage_(ops) {}
  using Print::write;
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t length) override {
    size_t count = storage_.write(bytes, length);
    if (count != length) setWriteError();
    return count;
  }
  bool open(const char* path) { clearWriteError(); return storage_.open(path); }
  void flush() override { storage_.commit(); }
  bool commit() { return storage_.commit(); }
  bool close() { return storage_.close(); }
  size_t position() { return storage_.position(); }
  int error() const { return storage_.error(); }
  explicit operator bool() const { return storage_.isOpen(); }
private:
  CheckedCsvStorage storage_;
};
#endif
