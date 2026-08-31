#include "control_mailbox.h"

#include <cstring>

namespace {

bool validId(const char* value, size_t capacity) {
  if (value == nullptr) return false;
  const void* end = std::memchr(value, '\0', capacity);
  if (end == nullptr) return false;
  const size_t length = static_cast<const char*>(end) - value;
  return length >= 8;
}

bool validAction(RemoteCommandAction action) {
  switch (action) {
    case RemoteCommandAction::kDiagnosticSnapshot:
    case RemoteCommandAction::kSetLogLevel:
    case RemoteCommandAction::kRecalibrateMicrophone:
    case RemoteCommandAction::kRetryUpload:
    case RemoteCommandAction::kReboot:
    case RemoteCommandAction::kOpenDevOta:
      return true;
  }
  return false;
}

bool validResult(MainControlResultCode code) {
  switch (code) {
    case MainControlResultCode::kSucceeded:
    case MainControlResultCode::kFailed:
    case MainControlResultCode::kRejected:
    case MainControlResultCode::kExpired:
      return true;
  }
  return false;
}

}  // namespace

void ControlMailbox::lock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&mutex_);
#else
  mutex_.lock();
#endif
}

void ControlMailbox::unlock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&mutex_);
#else
  mutex_.unlock();
#endif
}

bool ControlMailbox::publishRequest(const MainControlRequest& request) {
  if (!validId(request.commandId, sizeof(request.commandId)) ||
      !validAction(request.action)) {
    return false;
  }
  lock();
  if (requestPending_ || requestInFlight_ || resultPending_) {
    unlock();
    return false;
  }
  request_ = request;
  request_.version = nextVersion_++;
  requestPending_ = true;
  unlock();
  return true;
}

bool ControlMailbox::takeRequest(MainControlRequest* output) {
  if (output == nullptr) return false;
  lock();
  if (!requestPending_) {
    unlock();
    return false;
  }
  *output = request_;
  requestPending_ = false;
  requestInFlight_ = true;
  unlock();
  return true;
}

bool ControlMailbox::publishResult(const MainControlResult& result) {
  if (!validId(result.commandId, sizeof(result.commandId)) ||
      !validResult(result.code)) {
    return false;
  }
  lock();
  if (!requestInFlight_ || resultPending_ ||
      result.requestVersion != request_.version ||
      std::strcmp(result.commandId, request_.commandId) != 0) {
    unlock();
    return false;
  }
  result_ = result;
  resultPending_ = true;
  requestInFlight_ = false;
  unlock();
  return true;
}

bool ControlMailbox::takeResult(MainControlResult* output) {
  if (output == nullptr) return false;
  lock();
  if (!resultPending_) {
    unlock();
    return false;
  }
  *output = result_;
  resultPending_ = false;
  unlock();
  return true;
}

bool ControlMailbox::busy() const {
  lock();
  const bool result = requestPending_ || requestInFlight_ || resultPending_;
  unlock();
  return result;
}
