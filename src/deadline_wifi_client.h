#pragma once

#include <WiFi.h>

#include <cstddef>
#include <cstdint>

#include "telemetry_write_deadline.h"

enum class DeadlineWiFiClientFailure : uint8_t {
  kNone = 0,
  kInvalidArgument,
  kNotConnected,
  kNoProgressTimeout,
  kAbsoluteTimeout,
  kSelectFailed,
  kSendFailed,
  kPeerClosed,
};

const char* deadlineWiFiClientFailureName(DeadlineWiFiClientFailure failure);

// A telemetry-only WiFiClient write adapter. HTTPClient's stock ESP32 writer
// can exhaust ten EAGAIN retries in milliseconds or reset its ten-second retry
// budget after every partial write. This adapter instead retains one request
// deadline across header and body writes, sends small chunks, yields on EAGAIN,
// and closes the half-written socket on a bounded failure.
class DeadlineWiFiClient final : public WiFiClient {
 public:
  using WiFiClient::write;

  void beginBoundedWrite();
  void endBoundedWrite();

  size_t write(uint8_t value) override;
  size_t write(const uint8_t* bytes, size_t size) override;

  DeadlineWiFiClientFailure boundedWriteFailure() const;
  int boundedWriteErrno() const;
  size_t boundedBytesWritten() const;

 private:
  void failBoundedWrite(DeadlineWiFiClientFailure failure, int errorNumber);

  bool boundedWriteActive_ = false;
  bool deadlineStarted_ = false;
  TelemetryWriteDeadlineTracker deadline_;
  DeadlineWiFiClientFailure failure_ = DeadlineWiFiClientFailure::kNone;
  int errorNumber_ = 0;
  size_t bytesWritten_ = 0;
};
