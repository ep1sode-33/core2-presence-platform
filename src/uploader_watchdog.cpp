#include "uploader_watchdog.h"

#include <limits>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_task_wdt.h>
#endif

static_assert(
    static_cast<uint64_t>(kUploaderWatchdogPolicy.timeoutSeconds) * 1000ULL >
        static_cast<uint64_t>(
            kUploaderWatchdogPolicy.maximumConnectTimeoutMs) +
            kUploaderWatchdogPolicy.maximumIoTimeoutMs +
            kUploaderWatchdogPolicy.schedulingMarginMs,
    "uploader watchdog must exceed the longest bounded network operation");

uint32_t uploaderWatchdogMinimumTimeoutMs(
    const UploaderWatchdogPolicy& policy) {
  const uint64_t required =
      static_cast<uint64_t>(policy.maximumConnectTimeoutMs) +
      policy.maximumIoTimeoutMs + policy.schedulingMarginMs;
  return required > std::numeric_limits<uint32_t>::max()
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(required);
}

bool uploaderWatchdogPolicyIsValid(const UploaderWatchdogPolicy& policy) {
  if (policy.timeoutSeconds == 0 || policy.maximumIoTimeoutMs == 0 ||
      policy.schedulingMarginMs == 0 || policy.timeoutSeconds > 60) {
    return false;
  }
  const uint64_t configuredMs =
      static_cast<uint64_t>(policy.timeoutSeconds) * 1000ULL;
  return configuredMs > uploaderWatchdogMinimumTimeoutMs(policy);
}

UploaderWatchdogStartResult startUploaderTaskWatchdog() {
  if (!uploaderWatchdogPolicyIsValid(kUploaderWatchdogPolicy)) {
    return UploaderWatchdogStartResult::kInvalidPolicy;
  }
#if defined(ARDUINO_ARCH_ESP32)
  // ESP-IDF 4.x has one timeout shared by all subscribed tasks. Updating it to
  // 30 seconds also gives the Arduino loop task the same conservative bound.
  // Panic mode is required so a real deadlock records a watchdog reset and
  // reboots instead of merely printing a warning forever.
  if (esp_task_wdt_init(kUploaderWatchdogPolicy.timeoutSeconds, true) !=
      ESP_OK) {
    return UploaderWatchdogStartResult::kInitializationFailed;
  }
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
    return esp_task_wdt_reset() == ESP_OK
               ? UploaderWatchdogStartResult::kAlreadySubscribed
               : UploaderWatchdogStartResult::kSubscriptionFailed;
  }
  if (esp_task_wdt_add(nullptr) != ESP_OK ||
      esp_task_wdt_reset() != ESP_OK) {
    return UploaderWatchdogStartResult::kSubscriptionFailed;
  }
  return UploaderWatchdogStartResult::kStarted;
#else
  return UploaderWatchdogStartResult::kUnsupportedPlatform;
#endif
}

bool feedUploaderTaskWatchdog() {
#if defined(ARDUINO_ARCH_ESP32)
  return esp_task_wdt_reset() == ESP_OK;
#else
  return false;
#endif
}

const char* uploaderWatchdogStartResultName(
    UploaderWatchdogStartResult result) {
  switch (result) {
    case UploaderWatchdogStartResult::kStarted:
      return "started";
    case UploaderWatchdogStartResult::kAlreadySubscribed:
      return "already_subscribed";
    case UploaderWatchdogStartResult::kInvalidPolicy:
      return "invalid_policy";
    case UploaderWatchdogStartResult::kInitializationFailed:
      return "initialization_failed";
    case UploaderWatchdogStartResult::kSubscriptionFailed:
      return "subscription_failed";
    case UploaderWatchdogStartResult::kUnsupportedPlatform:
      return "unsupported_platform";
  }
  return "unknown";
}
