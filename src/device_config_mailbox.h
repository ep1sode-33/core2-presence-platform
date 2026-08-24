#pragma once

#include <cstdint>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

#include "device_config.h"

enum class DeviceConfigPublishResult : uint8_t {
  kPublished,
  kReplacedPending,
  kIgnoredAlreadyApplied,
  kIgnoredStalePending,
  kInvalidConfig,
};

// One-slot worker-to-main handoff. Newer revisions replace an unconsumed older
// revision. The applied-revision acknowledgement is monotonic and lets the
// worker suppress refetched or stale responses. No secret material is stored.
class DeviceConfigMailbox {
 public:
  DeviceConfigPublishResult publish(const PresenceConfig& config);
  bool take(PresenceConfig* output);

  void acknowledgeAppliedRevision(uint64_t revision);
  uint64_t acknowledgedAppliedRevision() const;
  bool hasPending() const;

 private:
  void lock() const;
  void unlock() const;

#if defined(ARDUINO_ARCH_ESP32)
  mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#endif
  PresenceConfig pending_ = {};
  uint64_t acknowledgedRevision_ = 0;
  bool hasPending_ = false;
};
