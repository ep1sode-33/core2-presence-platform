#pragma once

#include <cstddef>
#include <cstdint>

#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

// Compact, structured events only. Human-readable rendering happens at the
// backend so producers never allocate or format strings in the sampling loop.
enum class OperationalLogLevel : uint8_t {
  kError,
  kWarning,
  kInfo,
  kDebug,
  kSensorDetail,
};

enum class OperationalLogCode : uint16_t {
  kBoot,
  kHealthChanged,
  kPresenceTransition,
  kWifiChanged,
  kBackendRequest,
  kStorageChanged,
  kSensorChanged,
  kRecoveryAction,
  kOtaChanged,
  kDebugSessionChanged,
  kCommandChanged,
};

struct OperationalLogEvent {
  uint64_t sequence = 0;
  uint64_t uptimeMs = 0;
  OperationalLogLevel level = OperationalLogLevel::kInfo;
  OperationalLogCode code = OperationalLogCode::kBoot;
  int32_t value0 = 0;
  int32_t value1 = 0;
};

bool operationalLogEventIsValid(const OperationalLogEvent& event);
bool operationalLogEventIsCritical(const OperationalLogEvent& event);

enum class OperationalLogPushResult : uint8_t {
  kStored,
  kDroppedVerbose,
  kDroppedCritical,
  kInvalid,
};

// A bounded cross-core FIFO. Ordinary/debug events cannot consume the slots
// reserved for warnings and errors. Producers never wait for the network.
class OperationalLogRing {
 public:
  static constexpr size_t kCapacity = 64;
  static constexpr size_t kReservedCriticalSlots = 8;

  OperationalLogPushResult push(const OperationalLogEvent& event);
  size_t copyPrefix(OperationalLogEvent* output,
                    size_t outputCapacity) const;
  bool commitPrefix(const OperationalLogEvent* expected, size_t count);

  size_t size() const;
  constexpr size_t capacity() const { return kCapacity; }
  uint32_t droppedVerbose() const;
  uint32_t droppedCritical() const;
  uint32_t rejectedInvalid() const;

 private:
  void lock() const;
  void unlock() const;

#ifdef ARDUINO_ARCH_ESP32
  mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#endif
  OperationalLogEvent events_[kCapacity] = {};
  size_t head_ = 0;
  size_t size_ = 0;
  uint32_t droppedVerbose_ = 0;
  uint32_t droppedCritical_ = 0;
  uint32_t rejectedInvalid_ = 0;
};

enum class RemoteLogMode : uint8_t {
  kOperational,
  kDetailed,
};

// Detailed sensor logging is always time-limited. Calling beginDetailed again
// replaces (but never extends beyond) a fresh ten-minute window.
class RemoteLogSession {
 public:
  static constexpr uint64_t kMaximumDurationMs = 10ULL * 60ULL * 1000ULL;

  bool beginDetailed(uint64_t nowMs, uint64_t requestedDurationMs);
  void stop();
  RemoteLogMode mode(uint64_t nowMs);
  uint64_t remainingMs(uint64_t nowMs);
  bool accepts(OperationalLogLevel level, uint64_t nowMs);

 private:
  bool detailed_ = false;
  uint64_t expiresAtMs_ = 0;
};
