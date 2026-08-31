#pragma once

#include <cstddef>
#include <cstdint>

#include "core_dump_upload.h"

using CoreDumpJsonWrite = bool (*)(void* context, const uint8_t* data,
                                   size_t size);

struct CoreDumpJsonSink {
  void* context = nullptr;
  CoreDumpJsonWrite write = nullptr;
};

// Pull-based, fixed-memory JSON generator. It supports the Stream shape used
// by HTTPClient without retaining the dump or the full Base64 body.
class CoreDumpJsonReader {
 public:
  CoreDumpJsonReader() = default;
  ~CoreDumpJsonReader();
  CoreDumpJsonReader(const CoreDumpJsonReader&) = delete;
  CoreDumpJsonReader& operator=(const CoreDumpJsonReader&) = delete;

  bool begin(const PendingCoreDump& pending);

  size_t read(uint8_t* destination, size_t capacity);
  size_t available() const;
  size_t contentLength() const { return contentLength_; }
  size_t bytesRead() const { return bytesRead_; }
  bool failed() const { return failed_; }
  bool complete() const {
    return started_ && !failed_ && bytesRead_ == contentLength_;
  }

 private:
  static constexpr size_t kPrefixCapacity = 544;
  static constexpr size_t kRawBufferSize = 192;
  static constexpr size_t kBase64BufferSize = 256;

  bool fillBase64();

  const PendingCoreDump* pending_ = nullptr;
  char prefix_[kPrefixCapacity] = {};
  size_t prefixSize_ = 0;
  size_t prefixOffset_ = 0;
  uint8_t raw_[kRawBufferSize] = {};
  uint8_t base64_[kBase64BufferSize] = {};
  size_t base64Size_ = 0;
  size_t base64Offset_ = 0;
  size_t dumpOffset_ = 0;
  size_t suffixOffset_ = 0;
  size_t contentLength_ = 0;
  size_t bytesRead_ = 0;
  bool started_ = false;
  bool failed_ = false;
};

// Convenience push API for callbacks such as a bounded network writer. It
// returns false on a flash read or sink failure.
bool writeCoreDumpJson(const PendingCoreDump& pending,
                       const CoreDumpJsonSink& sink);

#if defined(ARDUINO_ARCH_ESP32)
#include <Stream.h>

// Direct HTTPClient integration:
//
//   CoreDumpArduinoJsonStream body(pending);
//   http.addHeader("Content-Type", "application/json");
//   int status = http.sendRequest("POST", &body, body.contentLength());
//
// A flash read failure makes available() return -1; HTTPClient then stops and
// detects the short body against Content-Length as a failed request.
class CoreDumpArduinoJsonStream : public Stream {
 public:
  explicit CoreDumpArduinoJsonStream(const PendingCoreDump& pending);

  int available() override;
  int read() override;
  int peek() override;
  size_t readBytes(uint8_t* buffer, size_t length) override;
  size_t write(uint8_t) override { return 0; }

  size_t contentLength() const { return reader_.contentLength(); }
  bool valid() const { return valid_; }
  bool failed() const { return !valid_ || reader_.failed(); }
  bool complete() const { return reader_.complete() && peeked_ < 0; }

 private:
  CoreDumpJsonReader reader_;
  int peeked_ = -1;
  bool valid_ = false;
};
#endif
