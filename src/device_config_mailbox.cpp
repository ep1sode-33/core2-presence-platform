#include "device_config_mailbox.h"

#include <type_traits>

namespace {

static_assert(std::is_trivially_copyable<PresenceConfig>::value,
              "mailbox payload must remain POD-copyable");

bool validForDevice(const PresenceConfig& config) {
  return validatePresenceConfig(config) ==
             PresenceConfigValidationError::kNone &&
         validatePresenceConfigDeviceCapabilities(config) ==
             PresenceConfigCapabilityError::kNone;
}

}  // namespace

void DeviceConfigMailbox::lock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&mutex_);
#endif
}

void DeviceConfigMailbox::unlock() const {
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&mutex_);
#endif
}

DeviceConfigPublishResult DeviceConfigMailbox::publish(
    const PresenceConfig& config) {
  // Validation is deliberately outside the critical section.
  if (!validForDevice(config)) {
    return DeviceConfigPublishResult::kInvalidConfig;
  }

  lock();
  if (config.revision <= acknowledgedRevision_) {
    unlock();
    return DeviceConfigPublishResult::kIgnoredAlreadyApplied;
  }
  if (hasPending_ && config.revision <= pending_.revision) {
    unlock();
    return DeviceConfigPublishResult::kIgnoredStalePending;
  }
  const bool replaced = hasPending_;
  pending_ = config;
  hasPending_ = true;
  unlock();
  return replaced ? DeviceConfigPublishResult::kReplacedPending
                  : DeviceConfigPublishResult::kPublished;
}

bool DeviceConfigMailbox::take(PresenceConfig* output) {
  if (output == nullptr) {
    return false;
  }
  lock();
  if (!hasPending_) {
    unlock();
    return false;
  }
  *output = pending_;
  hasPending_ = false;
  unlock();
  return true;
}

void DeviceConfigMailbox::acknowledgeAppliedRevision(uint64_t revision) {
  lock();
  if (revision > acknowledgedRevision_) {
    acknowledgedRevision_ = revision;
  }
  if (hasPending_ && pending_.revision <= acknowledgedRevision_) {
    hasPending_ = false;
  }
  unlock();
}

uint64_t DeviceConfigMailbox::acknowledgedAppliedRevision() const {
  lock();
  const uint64_t revision = acknowledgedRevision_;
  unlock();
  return revision;
}

bool DeviceConfigMailbox::hasPending() const {
  lock();
  const bool result = hasPending_;
  unlock();
  return result;
}
