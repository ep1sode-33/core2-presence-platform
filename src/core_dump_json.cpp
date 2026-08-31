#include "core_dump_json.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

constexpr uint8_t kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr uint8_t kJsonSuffix[] = {'"', '}'};

size_t encodedSize(size_t rawSize) {
  return ((rawSize + 2U) / 3U) * 4U;
}

void secureZero(void* bytes, size_t size) {
  volatile uint8_t* cursor = static_cast<volatile uint8_t*>(bytes);
  while (size-- != 0) {
    *cursor++ = 0;
  }
}

size_t encodeBase64(const uint8_t* source, size_t sourceSize,
                    uint8_t* destination, size_t capacity) {
  const size_t required = encodedSize(sourceSize);
  if ((source == nullptr && sourceSize != 0) || destination == nullptr ||
      required > capacity) {
    return 0;
  }
  size_t input = 0;
  size_t output = 0;
  while (input + 3 <= sourceSize) {
    const uint32_t word = static_cast<uint32_t>(source[input]) << 16U |
                          static_cast<uint32_t>(source[input + 1]) << 8U |
                          static_cast<uint32_t>(source[input + 2]);
    destination[output++] = kBase64Alphabet[(word >> 18U) & 0x3fU];
    destination[output++] = kBase64Alphabet[(word >> 12U) & 0x3fU];
    destination[output++] = kBase64Alphabet[(word >> 6U) & 0x3fU];
    destination[output++] = kBase64Alphabet[word & 0x3fU];
    input += 3;
  }
  const size_t trailing = sourceSize - input;
  if (trailing == 1) {
    const uint32_t word = static_cast<uint32_t>(source[input]) << 16U;
    destination[output++] = kBase64Alphabet[(word >> 18U) & 0x3fU];
    destination[output++] = kBase64Alphabet[(word >> 12U) & 0x3fU];
    destination[output++] = '=';
    destination[output++] = '=';
  } else if (trailing == 2) {
    const uint32_t word = static_cast<uint32_t>(source[input]) << 16U |
                          static_cast<uint32_t>(source[input + 1]) << 8U;
    destination[output++] = kBase64Alphabet[(word >> 18U) & 0x3fU];
    destination[output++] = kBase64Alphabet[(word >> 12U) & 0x3fU];
    destination[output++] = kBase64Alphabet[(word >> 6U) & 0x3fU];
    destination[output++] = '=';
  }
  return output;
}

}  // namespace

CoreDumpJsonReader::~CoreDumpJsonReader() {
  // A raw core dump can contain stack-resident credentials. Base64 is just
  // another representation of those bytes, so clear both fixed workspaces.
  secureZero(raw_, sizeof(raw_));
  secureZero(base64_, sizeof(base64_));
}

bool CoreDumpJsonReader::begin(const PendingCoreDump& pending) {
  pending_ = nullptr;
  std::memset(prefix_, 0, sizeof(prefix_));
  prefixSize_ = 0;
  prefixOffset_ = 0;
  secureZero(raw_, sizeof(raw_));
  secureZero(base64_, sizeof(base64_));
  base64Size_ = 0;
  base64Offset_ = 0;
  dumpOffset_ = 0;
  suffixOffset_ = 0;
  contentLength_ = 0;
  bytesRead_ = 0;
  started_ = false;
  failed_ = false;
  if (!pending.ready()) {
    return false;
  }

  const CoreDumpReportMetadata& metadata = pending.metadata();
  const int length = std::snprintf(
      prefix_, sizeof(prefix_),
      "{\"schema_version\":1,\"crash_id\":\"%s\",\"boot_id\":\"%s\","
      "\"build_id\":\"%s\",\"reset_reason\":\"%s\",\"dump_size\":%lu,"
      "\"dump_sha256\":\"%s\",\"dump_base64\":\"",
      metadata.crashId, metadata.bootId, metadata.buildId,
      metadata.resetReason, static_cast<unsigned long>(metadata.dumpSize),
      metadata.dumpSha256Hex);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(prefix_)) {
    return false;
  }
  prefixSize_ = static_cast<size_t>(length);
  const size_t bodySize = encodedSize(metadata.dumpSize);
  if (prefixSize_ > std::numeric_limits<size_t>::max() - bodySize ||
      prefixSize_ + bodySize >
          std::numeric_limits<size_t>::max() - sizeof(kJsonSuffix)) {
    return false;
  }
  contentLength_ = prefixSize_ + bodySize + sizeof(kJsonSuffix);
  pending_ = &pending;
  started_ = true;
  return true;
}

size_t CoreDumpJsonReader::read(uint8_t* destination, size_t capacity) {
  if (!started_ || failed_ || (destination == nullptr && capacity != 0) ||
      capacity == 0) {
    return 0;
  }
  size_t written = 0;
  while (written < capacity && bytesRead_ < contentLength_) {
    if (prefixOffset_ < prefixSize_) {
      const size_t copied = std::min(capacity - written,
                                     prefixSize_ - prefixOffset_);
      std::memcpy(destination + written, prefix_ + prefixOffset_, copied);
      prefixOffset_ += copied;
      written += copied;
      bytesRead_ += copied;
      continue;
    }

    if (dumpOffset_ < pending_->metadata().dumpSize ||
        base64Offset_ < base64Size_) {
      if (base64Offset_ == base64Size_ && !fillBase64()) {
        failed_ = true;
        return written;
      }
      const size_t copied =
          std::min(capacity - written, base64Size_ - base64Offset_);
      std::memcpy(destination + written, base64_ + base64Offset_, copied);
      base64Offset_ += copied;
      written += copied;
      bytesRead_ += copied;
      continue;
    }

    const size_t copied = std::min(capacity - written,
                                   sizeof(kJsonSuffix) - suffixOffset_);
    std::memcpy(destination + written, kJsonSuffix + suffixOffset_, copied);
    suffixOffset_ += copied;
    written += copied;
    bytesRead_ += copied;
  }
  return written;
}

size_t CoreDumpJsonReader::available() const {
  if (!started_ || failed_ || bytesRead_ >= contentLength_) {
    return 0;
  }
  return contentLength_ - bytesRead_;
}

bool CoreDumpJsonReader::fillBase64() {
  if (pending_ == nullptr || base64Offset_ != base64Size_ ||
      dumpOffset_ >= pending_->metadata().dumpSize) {
    return false;
  }
  const size_t remaining = pending_->metadata().dumpSize - dumpOffset_;
  const size_t rawSize = std::min(remaining, sizeof(raw_));
  if (!pending_->readChunk(dumpOffset_, raw_, rawSize)) {
    return false;
  }
  base64Size_ = encodeBase64(raw_, rawSize, base64_, sizeof(base64_));
  if (base64Size_ == 0) {
    return false;
  }
  base64Offset_ = 0;
  dumpOffset_ += rawSize;
  secureZero(raw_, rawSize);
  return true;
}

bool writeCoreDumpJson(const PendingCoreDump& pending,
                       const CoreDumpJsonSink& sink) {
  if (sink.write == nullptr) {
    return false;
  }
  CoreDumpJsonReader reader;
  if (!reader.begin(pending)) {
    return false;
  }
  uint8_t buffer[256] = {};
  while (reader.available() != 0) {
    const size_t size = reader.read(buffer, sizeof(buffer));
    if (size == 0 || !sink.write(sink.context, buffer, size)) {
      secureZero(buffer, sizeof(buffer));
      return false;
    }
  }
  secureZero(buffer, sizeof(buffer));
  return reader.complete();
}

#if defined(ARDUINO_ARCH_ESP32)
CoreDumpArduinoJsonStream::CoreDumpArduinoJsonStream(
    const PendingCoreDump& pending)
    : valid_(reader_.begin(pending)) {}

int CoreDumpArduinoJsonStream::available() {
  if (!valid_ || reader_.failed()) {
    return -1;
  }
  const size_t remaining = reader_.available() + (peeked_ >= 0 ? 1U : 0U);
  return remaining > static_cast<size_t>(INT_MAX)
             ? INT_MAX
             : static_cast<int>(remaining);
}

int CoreDumpArduinoJsonStream::read() {
  if (peeked_ >= 0) {
    const int value = peeked_;
    peeked_ = -1;
    return value;
  }
  uint8_t value = 0;
  return valid_ && reader_.read(&value, 1) == 1 ? value : -1;
}

int CoreDumpArduinoJsonStream::peek() {
  if (peeked_ < 0) {
    peeked_ = read();
  }
  return peeked_;
}

size_t CoreDumpArduinoJsonStream::readBytes(uint8_t* buffer, size_t length) {
  if (!valid_ || buffer == nullptr || length == 0) {
    return 0;
  }
  size_t copied = 0;
  if (peeked_ >= 0) {
    buffer[copied++] = static_cast<uint8_t>(peeked_);
    peeked_ = -1;
  }
  if (copied < length) {
    copied += reader_.read(buffer + copied, length - copied);
  }
  return copied;
}
#endif
