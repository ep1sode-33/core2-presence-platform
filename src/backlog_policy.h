#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

inline constexpr size_t kTelemetrySpoolHardFileLimit = 256;
inline constexpr size_t kTelemetrySpoolSoftFileLimit = 224;
inline constexpr size_t kCriticalFilesystemReserveBytes = 128 * 1024;

inline uint64_t adaptiveTelemetryIntervalMs(uint64_t configuredIntervalMs,
                                            size_t spoolFiles) {
  if (spoolFiles >= 192) {
    return std::max<uint64_t>(configuredIntervalMs, 60000);
  }
  if (spoolFiles >= 128) {
    return std::max<uint64_t>(configuredIntervalMs, 15000);
  }
  if (spoolFiles >= 64) {
    return std::max<uint64_t>(configuredIntervalMs, 5000);
  }
  return configuredIntervalMs;
}
inline bool mayFreezeTelemetry(size_t spoolFiles, size_t freeBytes,
                               bool criticalWaiting) {
  if (spoolFiles >= kTelemetrySpoolHardFileLimit) {
    return false;
  }
  if (criticalWaiting) {
    return true;
  }
  return spoolFiles < kTelemetrySpoolSoftFileLimit &&
         freeBytes >= kCriticalFilesystemReserveBytes;
}

inline bool shouldDrainCriticalReserve(size_t spoolFiles, size_t freeBytes,
                                       bool criticalWaiting) {
  return criticalWaiting &&
         (spoolFiles >= kTelemetrySpoolSoftFileLimit ||
          freeBytes < kCriticalFilesystemReserveBytes);
}
