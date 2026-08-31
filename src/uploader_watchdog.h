#pragma once

#include <cstdint>

struct UploaderWatchdogPolicy {
  uint32_t timeoutSeconds = 0;
  uint32_t maximumConnectTimeoutMs = 0;
  uint32_t maximumIoTimeoutMs = 0;
  uint32_t schedulingMarginMs = 0;
};

// HTTPClient can spend the connect timeout and the I/O timeout back-to-back.
// Keep an explicit margin for flash/NVS work and scheduler latency, while still
// recovering a genuinely stuck worker promptly.
inline constexpr UploaderWatchdogPolicy kUploaderWatchdogPolicy{
    30, 3000, 15000, 5000};

bool uploaderWatchdogPolicyIsValid(const UploaderWatchdogPolicy& policy);
uint32_t uploaderWatchdogMinimumTimeoutMs(
    const UploaderWatchdogPolicy& policy);

enum class UploaderWatchdogStartResult : uint8_t {
  kStarted = 0,
  kAlreadySubscribed,
  kInvalidPolicy,
  kInitializationFailed,
  kSubscriptionFailed,
  kUnsupportedPlatform,
};

UploaderWatchdogStartResult startUploaderTaskWatchdog();
bool feedUploaderTaskWatchdog();
const char* uploaderWatchdogStartResultName(
    UploaderWatchdogStartResult result);
