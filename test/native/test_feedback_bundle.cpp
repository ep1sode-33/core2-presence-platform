#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

#include "feedback_bundle.h"

namespace {

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

void expectError(FeedbackBundleError actual, FeedbackBundleError expected) {
  assert(actual == expected);
  assert(std::strcmp(feedbackBundleErrorName(actual), "unknown") != 0);
}

}  // namespace

int main() {
  const uint8_t crcVector[] = {'1', '2', '3', '4', '5',
                               '6', '7', '8', '9'};
  assert(feedbackBundleCrc32(crcVector, sizeof(crcVector)) == 0xcbf43926U);
  assert(feedbackBundleCrc32(nullptr, 0) == 0U);

  const std::string telemetry =
      "{\"schema_version\":1,\"records\":[{\"seq\":42}]}";
  const std::string feedback =
      "{\"feedback_id\":\"f:boot:000000000000002a\"}";
  std::array<uint8_t, kFeedbackBundleMaxEncodedBytes + 1> bundle = {};
  size_t encodedSize = 999;
  assert(encodeFeedbackBundle(
             reinterpret_cast<const uint8_t*>(telemetry.data()),
             telemetry.size(),
             reinterpret_cast<const uint8_t*>(feedback.data()),
             feedback.size(), bundle.data(), bundle.size(), encodedSize) ==
         FeedbackBundleError::kNone);
  assert(encodedSize ==
         kFeedbackBundleHeaderSize + telemetry.size() + feedback.size());

  FeedbackBundleSlices slices = {};
  assert(validateFeedbackBundle(bundle.data(), encodedSize, slices) ==
         FeedbackBundleError::kNone);
  assert(slices.telemetryOffset == kFeedbackBundleHeaderSize);
  assert(slices.telemetryLength == telemetry.size());
  assert(slices.feedbackOffset ==
         kFeedbackBundleHeaderSize + telemetry.size());
  assert(slices.feedbackLength == feedback.size());
  assert(slices.totalLength == encodedSize);
  assert(std::memcmp(bundle.data() + slices.telemetryOffset, telemetry.data(),
                     telemetry.size()) == 0);
  assert(std::memcmp(bundle.data() + slices.feedbackOffset, feedback.data(),
                     feedback.size()) == 0);

  FeedbackBundleSlices headerSlices = {};
  assert(inspectFeedbackBundleHeader(bundle.data(), kFeedbackBundleHeaderSize,
                                     encodedSize, headerSlices) ==
         FeedbackBundleError::kNone);
  assert(headerSlices.telemetryOffset == slices.telemetryOffset);
  assert(headerSlices.feedbackOffset == slices.feedbackOffset);
  assert(headerSlices.frameCrc32 == slices.frameCrc32);

  // Every strict prefix is a truncation, never a valid partial frame.
  for (size_t length = 0; length < encodedSize; ++length) {
    FeedbackBundleSlices ignored = {};
    assert(validateFeedbackBundle(bundle.data(), length, ignored) !=
           FeedbackBundleError::kNone);
  }

  bundle[encodedSize] = 0;
  expectError(validateFeedbackBundle(bundle.data(), encodedSize + 1, slices),
              FeedbackBundleError::kTrailingBytes);

  std::array<uint8_t, kFeedbackBundleMaxEncodedBytes + 1> corrupted = bundle;
  corrupted[kFeedbackBundleHeaderSize + 1] ^= 0x01U;
  expectError(validateFeedbackBundle(corrupted.data(), encodedSize, slices),
              FeedbackBundleError::kCrcMismatch);
  corrupted = bundle;
  corrupted[16] ^= 0x80U;
  expectError(validateFeedbackBundle(corrupted.data(), encodedSize, slices),
              FeedbackBundleError::kCrcMismatch);
  corrupted = bundle;
  corrupted[0] = 'X';
  expectError(validateFeedbackBundle(corrupted.data(), encodedSize, slices),
              FeedbackBundleError::kBadMagic);
  corrupted = bundle;
  writeLittleEndian16(corrupted.data() + 4, 2);
  expectError(validateFeedbackBundle(corrupted.data(), encodedSize, slices),
              FeedbackBundleError::kUnsupportedVersion);
  corrupted = bundle;
  writeLittleEndian16(corrupted.data() + 6, 19);
  expectError(validateFeedbackBundle(corrupted.data(), encodedSize, slices),
              FeedbackBundleError::kBadHeaderLength);
  corrupted = bundle;
  writeLittleEndian32(corrupted.data() + 8, 0);
  expectError(validateFeedbackBundle(corrupted.data(), encodedSize, slices),
              FeedbackBundleError::kTelemetryLengthOutOfRange);
  corrupted = bundle;
  writeLittleEndian32(
      corrupted.data() + 12,
      static_cast<uint32_t>(kFeedbackBundleMaxFeedbackJsonBytes + 1));
  expectError(validateFeedbackBundle(corrupted.data(), encodedSize, slices),
              FeedbackBundleError::kFeedbackLengthOutOfRange);
  corrupted = bundle;
  writeLittleEndian32(corrupted.data() + 8,
                      static_cast<uint32_t>(telemetry.size() + 1));
  writeLittleEndian32(corrupted.data() + 12,
                      static_cast<uint32_t>(feedback.size() - 1));
  expectError(validateFeedbackBundle(corrupted.data(), encodedSize, slices),
              FeedbackBundleError::kCrcMismatch);
  expectError(inspectFeedbackBundleHeader(bundle.data(),
                                          kFeedbackBundleHeaderSize - 1,
                                          encodedSize, slices),
              FeedbackBundleError::kHeaderTruncated);
  expectError(inspectFeedbackBundleHeader(nullptr, 0, 0, slices),
              FeedbackBundleError::kNullInput);

  size_t unchangedSize = 777;
  expectError(encodeFeedbackBundle(
                  reinterpret_cast<const uint8_t*>(telemetry.data()),
                  telemetry.size(),
                  reinterpret_cast<const uint8_t*>(feedback.data()),
                  feedback.size(), bundle.data(), encodedSize - 1,
                  unchangedSize),
              FeedbackBundleError::kOutputTooSmall);
  assert(unchangedSize == 777);
  expectError(encodeFeedbackBundle(
                  bundle.data(), telemetry.size(),
                  reinterpret_cast<const uint8_t*>(feedback.data()),
                  feedback.size(), bundle.data(), bundle.size(),
                  unchangedSize),
              FeedbackBundleError::kOverlappingBuffers);
  assert(unchangedSize == 777);
  expectError(encodeFeedbackBundle(
                  nullptr, telemetry.size(),
                  reinterpret_cast<const uint8_t*>(feedback.data()),
                  feedback.size(), bundle.data(), bundle.size(),
                  unchangedSize),
              FeedbackBundleError::kNullInput);
  assert(unchangedSize == 777);

  size_t calculatedSize = 0;
  expectError(feedbackBundleEncodedSize(
                  0, feedback.size(), calculatedSize),
              FeedbackBundleError::kTelemetryLengthOutOfRange);
  expectError(feedbackBundleEncodedSize(
                  telemetry.size(), 0, calculatedSize),
              FeedbackBundleError::kFeedbackLengthOutOfRange);

  // Exercise both declared upper bounds and verify the exact slice boundary.
  std::array<uint8_t, kFeedbackBundleMaxTelemetryJsonBytes> maxTelemetry = {};
  std::array<uint8_t, kFeedbackBundleMaxFeedbackJsonBytes> maxFeedback = {};
  maxTelemetry.fill('t');
  maxFeedback.fill('f');
  assert(encodeFeedbackBundle(
             maxTelemetry.data(), maxTelemetry.size(), maxFeedback.data(),
             maxFeedback.size(), bundle.data(), bundle.size(), encodedSize) ==
         FeedbackBundleError::kNone);
  assert(encodedSize == kFeedbackBundleMaxEncodedBytes);
  assert(validateFeedbackBundle(bundle.data(), encodedSize, slices) ==
         FeedbackBundleError::kNone);
  assert(slices.feedbackOffset ==
         kFeedbackBundleHeaderSize + kFeedbackBundleMaxTelemetryJsonBytes);
  assert(slices.feedbackOffset + slices.feedbackLength == encodedSize);

  return 0;
}
