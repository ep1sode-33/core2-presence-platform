#include "feedback_bundle.h"

#include <cstring>
#include <limits>

namespace {

constexpr uint8_t kMagic[4] = {'M', '5', 'F', 'B'};

uint16_t readLittleEndian16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t readLittleEndian32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8U) |
         (static_cast<uint32_t>(input[2]) << 16U) |
         (static_cast<uint32_t>(input[3]) << 24U);
}

void writeLittleEndian16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value & 0xffU);
  output[1] = static_cast<uint8_t>((value >> 8U) & 0xffU);
}

void writeLittleEndian32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value & 0xffU);
  output[1] = static_cast<uint8_t>((value >> 8U) & 0xffU);
  output[2] = static_cast<uint8_t>((value >> 16U) & 0xffU);
  output[3] = static_cast<uint8_t>((value >> 24U) & 0xffU);
}

uint32_t updateCrc32(uint32_t crc, const uint8_t* data, size_t size) {
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t lowBitMask =
          static_cast<uint32_t>(0U - (crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & lowBitMask);
    }
  }
  return crc;
}

bool addressRangeOverflows(const void* pointer, size_t size) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
  return size > std::numeric_limits<uintptr_t>::max() - start;
}

bool rangesOverlap(const void* left, size_t leftSize, const void* right,
                   size_t rightSize) {
  if (leftSize == 0 || rightSize == 0) {
    return false;
  }
  if (addressRangeOverflows(left, leftSize) ||
      addressRangeOverflows(right, rightSize)) {
    return true;
  }
  const uintptr_t leftStart = reinterpret_cast<uintptr_t>(left);
  const uintptr_t rightStart = reinterpret_cast<uintptr_t>(right);
  const uintptr_t leftEnd = leftStart + leftSize;
  const uintptr_t rightEnd = rightStart + rightSize;
  return leftStart < rightEnd && rightStart < leftEnd;
}

}  // namespace

FeedbackBundleError feedbackBundleEncodedSize(size_t telemetryJsonLength,
                                              size_t feedbackJsonLength,
                                              size_t& outputSize) {
  if (telemetryJsonLength == 0 ||
      telemetryJsonLength > kFeedbackBundleMaxTelemetryJsonBytes) {
    return FeedbackBundleError::kTelemetryLengthOutOfRange;
  }
  if (feedbackJsonLength == 0 ||
      feedbackJsonLength > kFeedbackBundleMaxFeedbackJsonBytes) {
    return FeedbackBundleError::kFeedbackLengthOutOfRange;
  }
  if (telemetryJsonLength >
      std::numeric_limits<size_t>::max() - kFeedbackBundleHeaderSize ||
      feedbackJsonLength >
          std::numeric_limits<size_t>::max() - kFeedbackBundleHeaderSize -
              telemetryJsonLength) {
    return FeedbackBundleError::kLengthOverflow;
  }

  outputSize =
      kFeedbackBundleHeaderSize + telemetryJsonLength + feedbackJsonLength;
  return FeedbackBundleError::kNone;
}

FeedbackBundleError encodeFeedbackBundle(
    const uint8_t* telemetryJson, size_t telemetryJsonLength,
    const uint8_t* feedbackJson, size_t feedbackJsonLength, uint8_t* output,
    size_t outputCapacity, size_t& outputSize) {
  if (telemetryJson == nullptr || feedbackJson == nullptr || output == nullptr) {
    return FeedbackBundleError::kNullInput;
  }

  size_t encodedSize = 0;
  const FeedbackBundleError sizeError = feedbackBundleEncodedSize(
      telemetryJsonLength, feedbackJsonLength, encodedSize);
  if (sizeError != FeedbackBundleError::kNone) {
    return sizeError;
  }
  if (outputCapacity < encodedSize) {
    return FeedbackBundleError::kOutputTooSmall;
  }
  if (rangesOverlap(output, encodedSize, telemetryJson,
                    telemetryJsonLength) ||
      rangesOverlap(output, encodedSize, feedbackJson, feedbackJsonLength)) {
    return FeedbackBundleError::kOverlappingBuffers;
  }

  std::memcpy(output + kFeedbackBundleHeaderSize, telemetryJson,
              telemetryJsonLength);
  std::memcpy(output + kFeedbackBundleHeaderSize + telemetryJsonLength,
              feedbackJson, feedbackJsonLength);

  std::memcpy(output, kMagic, sizeof(kMagic));
  writeLittleEndian16(output + 4, kFeedbackBundleVersion);
  writeLittleEndian16(output + 6,
                      static_cast<uint16_t>(kFeedbackBundleHeaderSize));
  writeLittleEndian32(output + 8,
                      static_cast<uint32_t>(telemetryJsonLength));
  writeLittleEndian32(output + 12,
                      static_cast<uint32_t>(feedbackJsonLength));
  const size_t bodySize = telemetryJsonLength + feedbackJsonLength;
  uint32_t frameCrc = updateCrc32(0xffffffffU, output, 16);
  frameCrc = updateCrc32(frameCrc, output + kFeedbackBundleHeaderSize,
                         bodySize) ^
             0xffffffffU;
  writeLittleEndian32(output + 16, frameCrc);

  outputSize = encodedSize;
  return FeedbackBundleError::kNone;
}

FeedbackBundleError inspectFeedbackBundleHeader(
    const uint8_t* headerBytes, size_t availableHeaderBytes,
    size_t totalBundleLength, FeedbackBundleSlices& output) {
  if (headerBytes == nullptr) {
    return FeedbackBundleError::kNullInput;
  }
  if (availableHeaderBytes < kFeedbackBundleHeaderSize ||
      totalBundleLength < kFeedbackBundleHeaderSize) {
    return FeedbackBundleError::kHeaderTruncated;
  }
  if (std::memcmp(headerBytes, kMagic, sizeof(kMagic)) != 0) {
    return FeedbackBundleError::kBadMagic;
  }
  if (readLittleEndian16(headerBytes + 4) != kFeedbackBundleVersion) {
    return FeedbackBundleError::kUnsupportedVersion;
  }
  if (readLittleEndian16(headerBytes + 6) != kFeedbackBundleHeaderSize) {
    return FeedbackBundleError::kBadHeaderLength;
  }

  const size_t telemetryLength = readLittleEndian32(headerBytes + 8);
  const size_t feedbackLength = readLittleEndian32(headerBytes + 12);
  size_t expectedSize = 0;
  const FeedbackBundleError sizeError =
      feedbackBundleEncodedSize(telemetryLength, feedbackLength, expectedSize);
  if (sizeError != FeedbackBundleError::kNone) {
    return sizeError;
  }
  if (totalBundleLength < expectedSize) {
    return FeedbackBundleError::kBodyTruncated;
  }
  if (totalBundleLength > expectedSize) {
    return FeedbackBundleError::kTrailingBytes;
  }

  FeedbackBundleSlices candidate = {};
  candidate.telemetryOffset = kFeedbackBundleHeaderSize;
  candidate.telemetryLength = telemetryLength;
  candidate.feedbackOffset = kFeedbackBundleHeaderSize + telemetryLength;
  candidate.feedbackLength = feedbackLength;
  candidate.totalLength = expectedSize;
  candidate.frameCrc32 = readLittleEndian32(headerBytes + 16);
  output = candidate;
  return FeedbackBundleError::kNone;
}

FeedbackBundleError validateFeedbackBundle(const uint8_t* bundle,
                                           size_t bundleLength,
                                           FeedbackBundleSlices& output) {
  FeedbackBundleSlices candidate = {};
  const FeedbackBundleError headerError = inspectFeedbackBundleHeader(
      bundle, bundleLength, bundleLength, candidate);
  if (headerError != FeedbackBundleError::kNone) {
    return headerError;
  }

  const size_t bodyLength = candidate.telemetryLength + candidate.feedbackLength;
  uint32_t actualCrc = updateCrc32(0xffffffffU, bundle, 16);
  actualCrc = updateCrc32(actualCrc, bundle + candidate.telemetryOffset,
                          bodyLength) ^
              0xffffffffU;
  if (actualCrc != candidate.frameCrc32) {
    return FeedbackBundleError::kCrcMismatch;
  }

  output = candidate;
  return FeedbackBundleError::kNone;
}

uint32_t feedbackBundleCrc32(const uint8_t* data, size_t size) {
  if (data == nullptr && size != 0) {
    return 0;
  }
  return updateCrc32(0xffffffffU, data, size) ^ 0xffffffffU;
}

const char* feedbackBundleErrorName(FeedbackBundleError error) {
  switch (error) {
    case FeedbackBundleError::kNone:
      return "none";
    case FeedbackBundleError::kNullInput:
      return "null_input";
    case FeedbackBundleError::kTelemetryLengthOutOfRange:
      return "telemetry_length_out_of_range";
    case FeedbackBundleError::kFeedbackLengthOutOfRange:
      return "feedback_length_out_of_range";
    case FeedbackBundleError::kLengthOverflow:
      return "length_overflow";
    case FeedbackBundleError::kOutputTooSmall:
      return "output_too_small";
    case FeedbackBundleError::kOverlappingBuffers:
      return "overlapping_buffers";
    case FeedbackBundleError::kHeaderTruncated:
      return "header_truncated";
    case FeedbackBundleError::kBadMagic:
      return "bad_magic";
    case FeedbackBundleError::kUnsupportedVersion:
      return "unsupported_version";
    case FeedbackBundleError::kBadHeaderLength:
      return "bad_header_length";
    case FeedbackBundleError::kBodyTruncated:
      return "body_truncated";
    case FeedbackBundleError::kTrailingBytes:
      return "trailing_bytes";
    case FeedbackBundleError::kCrcMismatch:
      return "crc_mismatch";
  }
  return "unknown";
}
