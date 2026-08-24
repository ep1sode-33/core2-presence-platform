#pragma once

#include <cstdint>

enum class PresenceState : uint8_t {
  kCalibrating,
  kIdle,
  kPresent,
  kCooldown,
};

enum class TransitionReason : uint8_t {
  kBoot,
  kCalibrationComplete,
  kPirMotion,
  kSoundBridge,
  kQuietTimeout,
  kCooldownTimeout,
  kTouchWake,
  kBenchOverride,
  kConfigChange,
  kUnknown,
};

inline const char* presenceStateDisplayName(PresenceState value) {
  switch (value) {
    case PresenceState::kCalibrating:
      return "CALIBRATING";
    case PresenceState::kIdle:
      return "IDLE / SCREEN OFF";
    case PresenceState::kPresent:
      return "PRESENT";
    case PresenceState::kCooldown:
      return "COOLDOWN";
  }
  return "UNKNOWN";
}

inline const char* presenceStateWireName(PresenceState value) {
  switch (value) {
    case PresenceState::kCalibrating:
      return "calibrating";
    case PresenceState::kIdle:
      return "idle";
    case PresenceState::kPresent:
      return "present";
    case PresenceState::kCooldown:
      return "cooldown";
  }
  return "idle";
}

inline const char* transitionReasonWireName(TransitionReason value) {
  switch (value) {
    case TransitionReason::kBoot:
      return "boot";
    case TransitionReason::kCalibrationComplete:
      return "calibration_complete";
    case TransitionReason::kPirMotion:
      return "pir_motion";
    case TransitionReason::kSoundBridge:
      return "sound_bridge";
    case TransitionReason::kQuietTimeout:
      return "quiet_timeout";
    case TransitionReason::kCooldownTimeout:
      return "cooldown_timeout";
    case TransitionReason::kTouchWake:
      return "touch_wake";
    case TransitionReason::kBenchOverride:
      return "bench_override";
    case TransitionReason::kConfigChange:
      return "config_change";
    case TransitionReason::kUnknown:
      return "unknown";
  }
  return "unknown";
}
