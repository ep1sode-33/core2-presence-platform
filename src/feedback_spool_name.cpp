#include "feedback_spool_name.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaxSigned64 =
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
constexpr size_t kMaximumPathLength =
    sizeof(FeedbackSpoolFileMetadata::path) - 1;
constexpr size_t kFilenameLength =
    sizeof("q_") - 1 + 32 + 1 + 16 + 1 + 16 + 1 + 1 + 1 + 1 +
    sizeof(".m5fb") - 1;

bool isLowerHex(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f');
}

size_t boundedLength(const char* value, size_t capacity) {
  if (value == nullptr) {
    return capacity;
  }
  size_t length = 0;
  while (length < capacity && value[length] != '\0') {
    ++length;
  }
  return length;
}

bool validBootId(const char* bootId, size_t capacity) {
  if (boundedLength(bootId, capacity) != 32) {
    return false;
  }
  for (size_t index = 0; index < 32; ++index) {
    if (!isLowerHex(bootId[index])) {
      return false;
    }
  }
  return true;
}

bool parseFixedHex(const char* input, size_t length, uint64_t& output) {
  uint64_t parsed = 0;
  for (size_t index = 0; index < length; ++index) {
    const char character = input[index];
    uint8_t digit = 0;
    if (character >= '0' && character <= '9') {
      digit = static_cast<uint8_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<uint8_t>(character - 'a' + 10);
    } else {
      return false;
    }
    parsed = (parsed << 4U) | digit;
  }
  output = parsed;
  return true;
}

char actualPresenceCode(ActualPresence value) {
  switch (value) {
    case ActualPresence::kPresent:
      return 'p';
    case ActualPresence::kAbsent:
      return 'a';
  }
  return '\0';
}

bool parseActualPresenceCode(char code, ActualPresence& output) {
  if (code == 'p') {
    output = ActualPresence::kPresent;
    return true;
  }
  if (code == 'a') {
    output = ActualPresence::kAbsent;
    return true;
  }
  return false;
}

char observedStateCode(PresenceState value) {
  switch (value) {
    case PresenceState::kCalibrating:
      return 'a';
    case PresenceState::kIdle:
      return 'i';
    case PresenceState::kPresent:
      return 'p';
    case PresenceState::kCooldown:
      return 'c';
  }
  return '\0';
}

bool parseObservedStateCode(char code, PresenceState& output) {
  switch (code) {
    case 'a':
      output = PresenceState::kCalibrating;
      return true;
    case 'i':
      output = PresenceState::kIdle;
      return true;
    case 'p':
      output = PresenceState::kPresent;
      return true;
    case 'c':
      output = PresenceState::kCooldown;
      return true;
    default:
      return false;
  }
}

bool buildFromRecord(const FeedbackRecord& record,
                     FeedbackSpoolFileMetadata& output) {
  if (!feedbackRecordIsValid(record)) {
    return false;
  }
  FeedbackSpoolFileMetadata candidate = {};
  std::memcpy(candidate.bootId, record.bootId, sizeof(candidate.bootId));
  std::memcpy(candidate.feedbackId, record.feedbackId,
              sizeof(candidate.feedbackId));
  candidate.seq = record.seq;
  candidate.linkedSampleUptimeMs = record.linkedSampleUptimeMs;
  candidate.actualPresence = record.actualPresence;
  candidate.observedState = record.observedState;
  const int idLength =
      static_cast<int>(std::strlen(candidate.feedbackId));
  const int pathLength =
      std::snprintf(candidate.path, sizeof(candidate.path),
                    "/feedback/wait/q_%s_%016llx_%016llx_%c_%c.m5fb",
                    record.bootId,
                    static_cast<unsigned long long>(record.seq),
                    static_cast<unsigned long long>(
                        record.linkedSampleUptimeMs),
                    actualPresenceCode(record.actualPresence),
                    observedStateCode(record.observedState));
  const int readyPathLength =
      std::snprintf(candidate.readyPath, sizeof(candidate.readyPath),
                    "/feedback/ready/q_%s_%016llx_%016llx_%c_%c.m5fb",
                    record.bootId,
                    static_cast<unsigned long long>(record.seq),
                    static_cast<unsigned long long>(
                        record.linkedSampleUptimeMs),
                    actualPresenceCode(record.actualPresence),
                    observedStateCode(record.observedState));
  const int batchIdLength =
      std::snprintf(candidate.telemetryBatchId,
                    sizeof(candidate.telemetryBatchId),
                    "b:%s:%016llx:%016llx:001", record.bootId,
                    static_cast<unsigned long long>(record.seq),
                    static_cast<unsigned long long>(record.seq));
  if (idLength != 51 || pathLength <= 0 || readyPathLength <= 0 ||
      batchIdLength <= 0 ||
      static_cast<size_t>(pathLength) >= sizeof(candidate.path) ||
      static_cast<size_t>(readyPathLength) >= sizeof(candidate.readyPath) ||
      static_cast<size_t>(batchIdLength) >=
          sizeof(candidate.telemetryBatchId)) {
    return false;
  }
  output = candidate;
  return true;
}

}  // namespace

bool buildFeedbackSpoolFileMetadata(const FeedbackRecord& record,
                                    FeedbackSpoolFileMetadata& output) {
  FeedbackSpoolFileMetadata candidate = {};
  if (!buildFromRecord(record, candidate)) {
    return false;
  }
  output = candidate;
  return true;
}

bool parseFeedbackSpoolFileMetadata(const char* path,
                                    FeedbackSpoolFileMetadata& output) {
  const size_t pathLength = boundedLength(path, kMaximumPathLength + 1);
  if (pathLength == 0 || pathLength > kMaximumPathLength) {
    return false;
  }

  size_t filenameStart = 0;
  for (size_t index = 0; index < pathLength; ++index) {
    if (path[index] == '/') {
      filenameStart = index + 1;
    }
  }
  if (pathLength - filenameStart != kFilenameLength) {
    return false;
  }
  const char* filename = path + filenameStart;
  if (std::memcmp(filename, "q_", 2) != 0 || filename[34] != '_' ||
      filename[51] != '_' || filename[68] != '_' || filename[70] != '_' ||
      std::memcmp(filename + kFilenameLength - 5, ".m5fb", 5) != 0) {
    return false;
  }

  char bootId[33] = {};
  std::memcpy(bootId, filename + 2, 32);
  uint64_t seq = 0;
  uint64_t uptimeMs = 0;
  ActualPresence actualPresence = ActualPresence::kAbsent;
  PresenceState observedState = PresenceState::kIdle;
  if (!validBootId(bootId, sizeof(bootId)) ||
      !parseFixedHex(filename + 35, 16, seq) ||
      !parseFixedHex(filename + 52, 16, uptimeMs) || seq > kMaxSigned64 ||
      uptimeMs > kMaxSigned64 ||
      !parseActualPresenceCode(filename[69], actualPresence) ||
      !parseObservedStateCode(filename[71], observedState)) {
    return false;
  }

  FeedbackRecord record = {};
  std::memcpy(record.bootId, bootId, sizeof(record.bootId));
  record.seq = seq;
  record.linkedSampleUptimeMs = uptimeMs;
  record.actualPresence = actualPresence;
  record.observedState = observedState;
  const int idLength =
      std::snprintf(record.feedbackId, sizeof(record.feedbackId),
                    "f:%s:%016llx", bootId,
                    static_cast<unsigned long long>(seq));
  if (idLength != 51) {
    return false;
  }
  return buildFromRecord(record, output);
}

bool feedbackRecordFromSpoolFileMetadata(
    const FeedbackSpoolFileMetadata& metadata, FeedbackRecord& output) {
  FeedbackRecord candidate = {};
  std::memcpy(candidate.feedbackId, metadata.feedbackId,
              sizeof(candidate.feedbackId));
  std::memcpy(candidate.bootId, metadata.bootId, sizeof(candidate.bootId));
  candidate.seq = metadata.seq;
  candidate.linkedSampleUptimeMs = metadata.linkedSampleUptimeMs;
  candidate.actualPresence = metadata.actualPresence;
  candidate.observedState = metadata.observedState;
  if (!feedbackRecordIsValid(candidate)) {
    return false;
  }
  FeedbackSpoolFileMetadata canonical = {};
  if (!buildFromRecord(candidate, canonical) ||
      std::memcmp(metadata.path, canonical.path, sizeof(canonical.path)) != 0 ||
      std::memcmp(metadata.readyPath, canonical.readyPath,
                  sizeof(canonical.readyPath)) != 0 ||
      std::memcmp(metadata.telemetryBatchId, canonical.telemetryBatchId,
                  sizeof(canonical.telemetryBatchId)) != 0) {
    return false;
  }
  output = candidate;
  return true;
}
