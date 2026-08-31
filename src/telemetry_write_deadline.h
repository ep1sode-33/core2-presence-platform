#pragma once

#include <cstdint>

inline constexpr uint32_t kTelemetryWriteNoProgressTimeoutMs = 1500;
inline constexpr uint32_t kTelemetryWriteAbsoluteTimeoutMs = 5000;

static_assert(kTelemetryWriteNoProgressTimeoutMs > 0);
static_assert(kTelemetryWriteNoProgressTimeoutMs <
              kTelemetryWriteAbsoluteTimeoutMs);

enum class TelemetryWriteDeadlineExpiry : uint8_t {
  kNone = 0,
  kNoProgress,
  kAbsolute,
};

// Wrap-safe wall-clock policy shared by the ESP32 socket adapter and host
// tests. Progress may extend the short deadline, but never the absolute one.
class TelemetryWriteDeadlineTracker {
 public:
  void begin(uint32_t nowMs) {
    startedMs_ = nowMs;
    lastProgressMs_ = nowMs;
  }

  void noteProgress(uint32_t nowMs) { lastProgressMs_ = nowMs; }

  TelemetryWriteDeadlineExpiry expiry(uint32_t nowMs) const {
    if (static_cast<uint32_t>(nowMs - startedMs_) >=
        kTelemetryWriteAbsoluteTimeoutMs) {
      return TelemetryWriteDeadlineExpiry::kAbsolute;
    }
    if (static_cast<uint32_t>(nowMs - lastProgressMs_) >=
        kTelemetryWriteNoProgressTimeoutMs) {
      return TelemetryWriteDeadlineExpiry::kNoProgress;
    }
    return TelemetryWriteDeadlineExpiry::kNone;
  }

 private:
  uint32_t startedMs_ = 0;
  uint32_t lastProgressMs_ = 0;
};
