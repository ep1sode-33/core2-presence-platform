#pragma once

#include <cstddef>
#include <cstdint>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#else
#include <mutex>
#endif

#include "command_journal.h"

struct MainControlRequest {
  char commandId[RemoteCommandEnvelope::kIdCapacity] = {};
  RemoteCommandAction action = RemoteCommandAction::kDiagnosticSnapshot;
  uint64_t expiresAtMs = 0;
  uint16_t durationSeconds = 0;
  bool detailedLog = false;
  bool requiresLocalConfirmation = false;
  uint32_t version = 0;
};

enum class MainControlResultCode : uint8_t {
  kSucceeded,
  kFailed,
  kRejected,
  kExpired,
};

struct MainControlResult {
  char commandId[RemoteCommandEnvelope::kIdCapacity] = {};
  MainControlResultCode code = MainControlResultCode::kFailed;
  uint32_t requestVersion = 0;
};

// Single-flight mailbox between the Core 0 network owner and the Core 1
// hardware/UI loop. The worker durably accepts and ACKs before publishRequest;
// the main loop never performs HTTP, flash, or NVS work.
class ControlMailbox {
 public:
  bool publishRequest(const MainControlRequest& request);
  bool takeRequest(MainControlRequest* output);
  bool publishResult(const MainControlResult& result);
  bool takeResult(MainControlResult* output);
  bool busy() const;

 private:
  void lock() const;
  void unlock() const;

#if defined(ARDUINO_ARCH_ESP32)
  mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#else
  mutable std::mutex mutex_;
#endif
  MainControlRequest request_ = {};
  MainControlResult result_ = {};
  bool requestPending_ = false;
  bool requestInFlight_ = false;
  bool resultPending_ = false;
  uint32_t nextVersion_ = 1;
};
