#include "spool_name.h"

#include <cstdio>
#include <cstring>

namespace {

bool isLowerHex(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f');
}

bool validBootId(const char* bootId) {
  if (bootId == nullptr || std::strlen(bootId) != 32) {
    return false;
  }
  for (size_t index = 0; index < 32; ++index) {
    if (!isLowerHex(bootId[index])) {
      return false;
    }
  }
  return true;
}

bool parseFixedHex(const char* source, size_t length, uint64_t& output) {
  uint64_t value = 0;
  for (size_t index = 0; index < length; ++index) {
    const char character = source[index];
    uint8_t digit = 0;
    if (character >= '0' && character <= '9') {
      digit = static_cast<uint8_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<uint8_t>(character - 'a' + 10);
    } else {
      return false;
    }
    value = (value << 4U) | digit;
  }
  output = value;
  return true;
}

}  // namespace

bool buildSpoolFileMetadata(const char* bootId, uint64_t firstSeq,
                            uint64_t maxSeq, uint16_t recordCount,
                            SpoolFileMetadata& output) {
  if (!validBootId(bootId) || firstSeq > maxSeq || recordCount == 0 ||
      recordCount > 256) {
    return false;
  }

  SpoolFileMetadata candidate;
  std::memcpy(candidate.bootId, bootId, 33);
  candidate.firstSeq = firstSeq;
  candidate.maxSeq = maxSeq;
  candidate.recordCount = recordCount;
  const int pathLength =
      std::snprintf(candidate.path, sizeof(candidate.path),
                    "/spool/q_%s_%016llx_%016llx_%03x.json", bootId,
                    static_cast<unsigned long long>(firstSeq),
                    static_cast<unsigned long long>(maxSeq),
                    static_cast<unsigned>(recordCount));
  const int batchLength =
      std::snprintf(candidate.batchId, sizeof(candidate.batchId),
                    "b:%s:%016llx:%016llx:%03x", bootId,
                    static_cast<unsigned long long>(firstSeq),
                    static_cast<unsigned long long>(maxSeq),
                    static_cast<unsigned>(recordCount));
  if (pathLength <= 0 || static_cast<size_t>(pathLength) >=
                             sizeof(candidate.path) ||
      batchLength <= 0 || static_cast<size_t>(batchLength) >=
                              sizeof(candidate.batchId)) {
    return false;
  }
  output = candidate;
  return true;
}

bool parseSpoolFileMetadata(const char* path, SpoolFileMetadata& output) {
  if (path == nullptr) {
    return false;
  }
  const char* filename = std::strrchr(path, '/');
  filename = filename == nullptr ? path : filename + 1;
  constexpr size_t kExpectedFilenameLength =
      sizeof("q_") - 1 + 32 + 1 + 16 + 1 + 16 + 1 + 3 +
      sizeof(".json") - 1;
  if (std::strlen(filename) != kExpectedFilenameLength ||
      std::strncmp(filename, "q_", 2) != 0 ||
      std::strcmp(filename + kExpectedFilenameLength - 5, ".json") != 0) {
    return false;
  }

  const char* boot = filename + 2;
  const char* first = boot + 32 + 1;
  const char* maximum = first + 16 + 1;
  const char* count = maximum + 16 + 1;
  if (boot[32] != '_' || first[16] != '_' || maximum[16] != '_' ||
      !isLowerHex(count[0]) || !isLowerHex(count[1]) ||
      !isLowerHex(count[2])) {
    return false;
  }

  char bootId[33] = {};
  std::memcpy(bootId, boot, 32);
  uint64_t firstSeq = 0;
  uint64_t maxSeq = 0;
  uint64_t parsedCount = 0;
  if (!validBootId(bootId) || !parseFixedHex(first, 16, firstSeq) ||
      !parseFixedHex(maximum, 16, maxSeq) ||
      !parseFixedHex(count, 3, parsedCount) || parsedCount == 0 ||
      parsedCount > 256) {
    return false;
  }

  return buildSpoolFileMetadata(bootId, firstSeq, maxSeq,
                                static_cast<uint16_t>(parsedCount), output);
}
