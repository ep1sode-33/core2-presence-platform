#pragma once

#include <cstddef>
#include <cstdint>

// On-disk framing for one indivisible touch-feedback evidence item.
//
//   0..3   magic "M5FB"
//   4..5   version (little-endian, currently 1)
//   6..7   header length (little-endian, currently 20)
//   8..11  exact telemetry JSON byte length
//  12..15  exact feedback JSON byte length
//  16..19  IEEE CRC-32 of bytes 0..15, then telemetry and feedback bytes
//  20..    telemetry JSON, then feedback JSON (neither rewritten)
//
// Including both length fields in the CRC protects the boundary between the
// two otherwise opaque bodies, not merely their concatenated bytes.
//
// A single-sample telemetry envelope fits comfortably below 2048 bytes and
// writeTouchFeedbackJson is bounded below 320 bytes. The complete frame is
// therefore at most 2388 bytes, suitable for a 12 KiB uploader task stack when
// the caller avoids keeping redundant full-size copies.
constexpr size_t kFeedbackBundleHeaderSize = 20;
constexpr uint16_t kFeedbackBundleVersion = 1;
constexpr size_t kFeedbackBundleMaxTelemetryJsonBytes = 2048;
constexpr size_t kFeedbackBundleMaxFeedbackJsonBytes = 320;
constexpr size_t kFeedbackBundleMaxEncodedBytes =
    kFeedbackBundleHeaderSize + kFeedbackBundleMaxTelemetryJsonBytes +
    kFeedbackBundleMaxFeedbackJsonBytes;

enum class FeedbackBundleError : uint8_t {
  kNone,
  kNullInput,
  kTelemetryLengthOutOfRange,
  kFeedbackLengthOutOfRange,
  kLengthOverflow,
  kOutputTooSmall,
  kOverlappingBuffers,
  kHeaderTruncated,
  kBadMagic,
  kUnsupportedVersion,
  kBadHeaderLength,
  kBodyTruncated,
  kTrailingBytes,
  kCrcMismatch,
};

// Offset-based metadata remains valid across file close/reopen and does not
// retain pointers into a temporary read buffer.
struct FeedbackBundleSlices {
  size_t telemetryOffset = 0;
  size_t telemetryLength = 0;
  size_t feedbackOffset = 0;
  size_t feedbackLength = 0;
  size_t totalLength = 0;
  uint32_t frameCrc32 = 0;
};

FeedbackBundleError feedbackBundleEncodedSize(size_t telemetryJsonLength,
                                              size_t feedbackJsonLength,
                                              size_t& outputSize);

// Writes one complete frame into caller-owned storage. Input byte ranges must
// not overlap the output range; all inputs and outputSize are unchanged on
// failure.
FeedbackBundleError encodeFeedbackBundle(
    const uint8_t* telemetryJson, size_t telemetryJsonLength,
    const uint8_t* feedbackJson, size_t feedbackJsonLength, uint8_t* output,
    size_t outputCapacity, size_t& outputSize);

// Validates only the fixed header and its declared file length. `headerBytes`
// may be a 20-byte prefix read from LittleFS; `totalBundleLength` is the file
// size. CRC validation requires validateFeedbackBundle below.
FeedbackBundleError inspectFeedbackBundleHeader(
    const uint8_t* headerBytes, size_t availableHeaderBytes,
    size_t totalBundleLength, FeedbackBundleSlices& output);

// Validates framing, exact total length, and CRC before returning slice
// offsets. Output is unchanged on failure.
FeedbackBundleError validateFeedbackBundle(const uint8_t* bundle,
                                           size_t bundleLength,
                                           FeedbackBundleSlices& output);

// Standard IEEE CRC-32 helper ("123456789" -> 0xcbf43926).
uint32_t feedbackBundleCrc32(const uint8_t* data, size_t size);

const char* feedbackBundleErrorName(FeedbackBundleError error);
