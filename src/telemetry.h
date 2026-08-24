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
};

struct TelemetryRecord {
  TelemetryKind kind = TelemetryKind::kSample;
  uint64_t seq = 0;
  uint64_t uptimeMs = 0;
  SampleTelemetry sample = {};
  TransitionTelemetry transition = {};
};

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

  size_t size() const;
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
};
