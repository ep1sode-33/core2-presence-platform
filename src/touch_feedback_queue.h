#pragma once

#include <cstddef>
#include <cstdint>

#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

#include "feedback_protocol.h"
#include "telemetry.h"

// One indivisible touch-correction item. The complete pre-touch sample is kept
// beside the feedback that names it, so a producer can never persist one half
// while losing the other half.
struct TouchFeedbackEvidence {
  TelemetryRecord preTouchSample = {};
  FeedbackRecord feedback = {};
};

// Builds a self-consistent evidence pair without first enqueueing into a
// separate TelemetryQueue. Output is unchanged on failure.
bool buildTouchFeedbackEvidence(const char* bootId,
                                const TelemetryRecord& preTouchSample,
                                TouchPresenceChoice choice,
                                TouchFeedbackEvidence& output);

// Checks the complete sample payload plus the immutable feedback linkage.
bool touchFeedbackEvidenceIsValid(const TouchFeedbackEvidence& evidence);

enum class TouchFeedbackQueuePushResult : uint8_t {
  kStored,
  kFull,
  kInvalid,
};

// A bounded static queue. The public evidence type intentionally carries a
// complete TelemetryRecord, but a touch correction can only reference a sample;
// keeping the unused transition payload and deterministic feedback strings in
// every ring slot would waste scarce internal DRAM. The queue therefore stores
// only the fields needed to reconstruct both public records exactly.
//
// One hundred twenty-eight pairs cover rapid physical input throughout the
// bounded OTA watchdog envelope. The worker still copies only one reconstructed
// front item onto its task stack.
class TouchFeedbackQueue {
 public:
  static constexpr size_t kCapacity = 128;

  TouchFeedbackQueuePushResult push(const TouchFeedbackEvidence& evidence);

  // Copies at most outputCapacity complete pairs in FIFO order. A caller that
  // serializes one immutable bundle at a time should pass capacity 1.
  size_t copyPrefix(TouchFeedbackEvidence* output,
                    size_t outputCapacity) const;

  // Removes exactly the copied prefix only if every semantic field still
  // matches. Failure leaves the queue unchanged.
  bool commitPrefix(const TouchFeedbackEvidence* expected, size_t count);

  size_t size() const;
  constexpr size_t capacity() const { return kCapacity; }
  uint32_t droppedFull() const;
  uint32_t rejectedInvalid() const;

 private:
  struct CompactSlot {
    uint64_t seq = 0;
    uint64_t uptimeMs = 0;
    uint64_t appliedConfigRevision = 0;
    SampleTelemetry sample = {};
    char bootId[33] = {};
    ActualPresence actualPresence = ActualPresence::kPresent;
  };

  static CompactSlot compact(const TouchFeedbackEvidence& evidence);
  static TouchFeedbackEvidence expand(const CompactSlot& slot);
  static bool slotMatchesEvidence(const CompactSlot& slot,
                                  const TouchFeedbackEvidence& evidence);

  void lock() const;
  void unlock() const;

#ifdef ARDUINO_ARCH_ESP32
  mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#endif
  CompactSlot records_[kCapacity] = {};
  size_t head_ = 0;
  size_t size_ = 0;
  uint32_t droppedFull_ = 0;
  uint32_t rejectedInvalid_ = 0;
};
