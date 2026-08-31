#pragma once

#include <cstddef>
#include <cstdint>

#include "health_snapshot.h"

struct MicrophoneWindowHealthInput {
  bool driverStarted = false;
  size_t sampleCount = 0;
  size_t railSampleCount = 0;
  size_t repeatedSampleCount = 0;
  uint16_t rawMinimum = 0;
  uint16_t rawMaximum = 0;
  uint32_t elapsedUs = 0;
  uint32_t expectedUs = 0;
};

SensorHealthStatus classifyMicrophoneWindow(
    const MicrophoneWindowHealthInput& input);

class SensorHealthLatch {
 public:
  // Transient ADC timing outliers are common while the Wi-Fi stack services an
  // interrupt. Require consecutive bad windows before surfacing degradation,
  // then use a longer healthy run to recover and avoid status chatter.
  explicit SensorHealthLatch(uint8_t badWindowsToFault = 8,
                             uint8_t goodWindowsToRecover = 120,
                             uint8_t badWindowsToDegraded = 60)
      : badWindowsToFault_(badWindowsToFault),
        goodWindowsToRecover_(goodWindowsToRecover),
        badWindowsToDegraded_(badWindowsToDegraded) {}

  SensorHealthStatus observe(SensorHealthStatus windowStatus);
  SensorHealthStatus status() const { return status_; }

 private:
  uint8_t badWindowsToFault_ = 8;
  uint8_t goodWindowsToRecover_ = 120;
  uint8_t badWindowsToDegraded_ = 60;
  uint8_t badWindows_ = 0;
  uint8_t hardFaultWindows_ = 0;
  uint8_t goodWindows_ = 0;
  SensorHealthStatus status_ = SensorHealthStatus::kUnknown;
};

class PirHealthTracker {
 public:
  SensorHealthStatus observe(bool high, uint64_t nowMs);

 private:
  uint64_t levelSinceMs_ = 0;
  uint64_t lastEdgeMs_ = 0;
  bool initialized_ = false;
  bool previousHigh_ = false;
};
