#include "sensor_health.h"

#include <algorithm>
#include <limits>

namespace {

constexpr uint64_t kPirHighFaultMs = 30ULL * 60ULL * 1000ULL;
constexpr uint64_t kPirNoEdgeDegradedMs = 24ULL * 60ULL * 60ULL * 1000ULL;

void incrementSaturated(uint8_t& value) {
  if (value != std::numeric_limits<uint8_t>::max()) {
    ++value;
  }
}

}  // namespace

SensorHealthStatus classifyMicrophoneWindow(
    const MicrophoneWindowHealthInput& input) {
  if (!input.driverStarted || input.sampleCount < 8 || input.expectedUs == 0 ||
      input.rawMaximum < input.rawMinimum) {
    return SensorHealthStatus::kFault;
  }
  const float railRatio = static_cast<float>(input.railSampleCount) /
                          static_cast<float>(input.sampleCount);
  const float repeatedRatio = static_cast<float>(input.repeatedSampleCount) /
                              static_cast<float>(input.sampleCount - 1);
  const uint32_t dynamicRange = input.rawMaximum - input.rawMinimum;
  if (railRatio >= 0.25f ||
      (repeatedRatio >= 0.985f && dynamicRange <= 2U)) {
    return SensorHealthStatus::kFault;
  }
  if (input.elapsedUs > input.expectedUs + input.expectedUs / 4U ||
      input.elapsedUs < input.expectedUs - input.expectedUs / 4U ||
      railRatio >= 0.05f ||
      (repeatedRatio >= 0.95f && dynamicRange <= 4U)) {
    return SensorHealthStatus::kDegraded;
  }
  return SensorHealthStatus::kHealthy;
}

SensorHealthStatus SensorHealthLatch::observe(
    SensorHealthStatus windowStatus) {
  if (windowStatus == SensorHealthStatus::kHealthy) {
    badWindows_ = 0;
    incrementSaturated(goodWindows_);
    if (status_ == SensorHealthStatus::kUnknown ||
        goodWindows_ >= goodWindowsToRecover_) {
      status_ = SensorHealthStatus::kHealthy;
    }
    return status_;
  }

  goodWindows_ = 0;
  incrementSaturated(badWindows_);
  if (windowStatus == SensorHealthStatus::kFault &&
      badWindows_ >= badWindowsToFault_) {
    status_ = SensorHealthStatus::kFault;
  } else if (status_ != SensorHealthStatus::kFault &&
             badWindows_ >= badWindowsToDegraded_) {
    status_ = SensorHealthStatus::kDegraded;
  }
  return status_;
}

SensorHealthStatus PirHealthTracker::observe(bool high, uint64_t nowMs) {
  if (!initialized_) {
    initialized_ = true;
    previousHigh_ = high;
    levelSinceMs_ = nowMs;
    lastEdgeMs_ = nowMs;
    return SensorHealthStatus::kHealthy;
  }
  if (high != previousHigh_) {
    previousHigh_ = high;
    levelSinceMs_ = nowMs;
    lastEdgeMs_ = nowMs;
  }
  if (high && nowMs - levelSinceMs_ >= kPirHighFaultMs) {
    return SensorHealthStatus::kFault;
  }
  if (!high && nowMs - lastEdgeMs_ >= kPirNoEdgeDegradedMs) {
    return SensorHealthStatus::kDegraded;
  }
  return SensorHealthStatus::kHealthy;
}
