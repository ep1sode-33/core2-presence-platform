#include "deadline_wifi_client.h"

#include <Arduino.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cerrno>

#include "uploader_watchdog.h"

namespace {

constexpr size_t kMaximumSocketWriteBytes = 512;
constexpr uint32_t kSocketSelectSliceMs = 50;
constexpr uint32_t kEagainYieldMs = 1;

static_assert(kMaximumSocketWriteBytes > 0);
static_assert(kSocketSelectSliceMs < kTelemetryWriteNoProgressTimeoutMs);

}  // namespace

const char* deadlineWiFiClientFailureName(DeadlineWiFiClientFailure failure) {
  switch (failure) {
    case DeadlineWiFiClientFailure::kNone:
      return "none";
    case DeadlineWiFiClientFailure::kInvalidArgument:
      return "invalid_argument";
    case DeadlineWiFiClientFailure::kNotConnected:
      return "not_connected";
    case DeadlineWiFiClientFailure::kNoProgressTimeout:
      return "no_progress_timeout";
    case DeadlineWiFiClientFailure::kAbsoluteTimeout:
      return "absolute_timeout";
    case DeadlineWiFiClientFailure::kSelectFailed:
      return "select_failed";
    case DeadlineWiFiClientFailure::kSendFailed:
      return "send_failed";
    case DeadlineWiFiClientFailure::kPeerClosed:
      return "peer_closed";
  }
  return "unknown";
}

void DeadlineWiFiClient::beginBoundedWrite() {
  boundedWriteActive_ = true;
  deadlineStarted_ = false;
  failure_ = DeadlineWiFiClientFailure::kNone;
  errorNumber_ = 0;
  bytesWritten_ = 0;
  clearWriteError();
}

void DeadlineWiFiClient::endBoundedWrite() { boundedWriteActive_ = false; }

size_t DeadlineWiFiClient::write(uint8_t value) {
  return write(&value, 1);
}

size_t DeadlineWiFiClient::write(const uint8_t* bytes, size_t size) {
  if (!boundedWriteActive_) {
    return WiFiClient::write(bytes, size);
  }
  if (failure_ != DeadlineWiFiClientFailure::kNone) {
    return 0;
  }
  if (bytes == nullptr || size == 0) {
    if (size != 0) {
      failBoundedWrite(DeadlineWiFiClientFailure::kInvalidArgument, EINVAL);
    }
    return 0;
  }

  if (!deadlineStarted_) {
    deadline_.begin(millis());
    deadlineStarted_ = true;
  }

  const int socketFd = fd();
  if (!connected() || socketFd < 0) {
    failBoundedWrite(DeadlineWiFiClientFailure::kNotConnected, ENOTCONN);
    return 0;
  }

  size_t sentTotal = 0;
  while (sentTotal < size) {
    feedUploaderTaskWatchdog();
    const TelemetryWriteDeadlineExpiry expiry = deadline_.expiry(millis());
    if (expiry == TelemetryWriteDeadlineExpiry::kNoProgress) {
      failBoundedWrite(DeadlineWiFiClientFailure::kNoProgressTimeout,
                       ETIMEDOUT);
      break;
    }
    if (expiry == TelemetryWriteDeadlineExpiry::kAbsolute) {
      failBoundedWrite(DeadlineWiFiClientFailure::kAbsoluteTimeout, ETIMEDOUT);
      break;
    }

    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socketFd, &writable);
    timeval timeout = {};
    timeout.tv_usec = kSocketSelectSliceMs * 1000;
    const int selected =
        select(socketFd + 1, nullptr, &writable, nullptr, &timeout);
    if (selected < 0) {
      if (errno == EINTR) {
        continue;
      }
      failBoundedWrite(DeadlineWiFiClientFailure::kSelectFailed, errno);
      break;
    }
    if (selected == 0 || !FD_ISSET(socketFd, &writable)) {
      delay(0);
      continue;
    }

    const size_t requested =
        std::min(size - sentTotal, kMaximumSocketWriteBytes);
    const int sent = send(socketFd, bytes + sentTotal, requested, MSG_DONTWAIT);
    if (sent > 0) {
      const size_t progressed = static_cast<size_t>(sent);
      sentTotal += progressed;
      bytesWritten_ += progressed;
      deadline_.noteProgress(millis());
      feedUploaderTaskWatchdog();
      continue;
    }
    if (sent == 0) {
      failBoundedWrite(DeadlineWiFiClientFailure::kPeerClosed, 0);
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      delay(kEagainYieldMs);
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    failBoundedWrite(DeadlineWiFiClientFailure::kSendFailed, errno);
    break;
  }
  return sentTotal;
}

DeadlineWiFiClientFailure DeadlineWiFiClient::boundedWriteFailure() const {
  return failure_;
}

int DeadlineWiFiClient::boundedWriteErrno() const { return errorNumber_; }

size_t DeadlineWiFiClient::boundedBytesWritten() const {
  return bytesWritten_;
}

void DeadlineWiFiClient::failBoundedWrite(DeadlineWiFiClientFailure failure,
                                          int errorNumber) {
  if (failure_ == DeadlineWiFiClientFailure::kNone) {
    failure_ = failure;
    errorNumber_ = errorNumber;
    setWriteError(errorNumber == 0 ? 1 : errorNumber);
  }
  WiFiClient::stop();
}
