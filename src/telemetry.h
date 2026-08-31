#pragma once

#include <cstddef>
#include <cstdint>

#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

#include "presence_types.h"

enum class TelemetryKind : uint8_t {
  kSample,
  kTransition,
};

struct SampleTelemetry {
  bool pir = false;
  float micRms = 0.0f;
  float micEnvelope = 0.0f;
  int16_t micMin = 0;
  int16_t micMax = 0;
  float noiseFloor = 0.0f;
  float soundThreshold = 0.0f;
  bool soundActive = false;
  PresenceState state = PresenceState::kCalibrating;
  uint8_t brightness = 0;
};

struct TransitionTelemetry {
  bool hasFromState = true;
  PresenceState fromState = PresenceState::kCalibrating;
  PresenceState toState = PresenceState::kCalibrating;
  TransitionReason reason = TransitionReason::kUnknown;
  bool pir = false;
  uint64_t pirAgeMs = 0;
  bool soundActive = false;
  uint64_t soundAgeMs = 0;
  float micEnvelope = 0.0f;
  float noiseFloor = 0.0f;
  float soundThreshold = 0.0f;
  uint8_t brightnessBefore = 0;
  uint8_t brightnessAfter = 0;
};

struct TelemetryRecord {
  TelemetryKind kind = TelemetryKind::kSample;
  uint64_t seq = 0;
  uint64_t uptimeMs = 0;
  // Internal envelope metadata. It is serialized once at batch level, and the
  // uploader must never combine records carrying different revisions.
  uint64_t appliedConfigRevision = 0;
  SampleTelemetry sample = {};
  TransitionTelemetry transition = {};
};

// Returns the leading record count that can share one batch-level applied
// configuration revision.
size_t contiguousRevisionPrefix(const TelemetryRecord* records, size_t count);

enum class QueuePushResult : uint8_t {
  kStored,
  kSampleDropped,
  kCriticalDropped,
};

class TelemetryQueue {
 public:
  static constexpr size_t kCapacity = 120;
  static constexpr size_t kReservedTransitionSlots = 8;

  QueuePushResult push(const TelemetryRecord& record);
  bool peek(TelemetryRecord& output) const;
  size_t copyPrefix(TelemetryRecord* output, size_t outputCapacity) const;
  bool commitPrefix(const TelemetryRecord* expected, size_t count);

  // During a blocking OTA transfer, reject ordinary 1 Hz samples so the full
  // fixed queue remains available for state transitions. The worker drains
  // existing critical records before enabling this mode.
  void setCriticalOnly(bool enabled);
  bool criticalOnly() const;

  size_t size() const;
  bool hasCritical() const;
  constexpr size_t capacity() const { return kCapacity; }
  uint32_t droppedSamples() const;
  uint32_t droppedCritical() const;

 private:
  void lock() const;
  void unlock() const;

#ifdef ARDUINO_ARCH_ESP32
  mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#endif
  TelemetryRecord records_[kCapacity] = {};
  size_t head_ = 0;
  size_t size_ = 0;
  uint32_t droppedSamples_ = 0;
  uint32_t droppedCritical_ = 0;
  bool criticalOnly_ = false;
};
