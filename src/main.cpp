#include <Arduino.h>
#include <M5Unified.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <inttypes.h>
#include <limits>
#include <esp_system.h>

#include "device_config.h"
#include "device_config_mailbox.h"
#include "device_config_storage.h"
#include "device_settings.h"
#include "dashboard_mailbox.h"
#include "dashboard_time.h"
#include "presence_types.h"
#include "provisioning_protocol.h"
#include "runtime_identity.h"
#include "telemetry.h"
#include "telemetry_uploader.h"
#include "touch_feedback_queue.h"

namespace {

// M5GO PORT.B: white wire / digital input is GPIO36.
constexpr gpio_num_t kPirPin = GPIO_NUM_36;

constexpr uint64_t kCalibrationMs = 5000;
constexpr uint64_t kDisplayMinimumIntervalMs = 50;
constexpr uint64_t kDisplayHeartbeatMs = 60 * 1000;
constexpr int32_t kDashboardHeaderHeight = 32;
constexpr uint64_t kSerialIntervalMs = 250;
constexpr uint64_t kProvisioningChallengeLifetimeMs = 2 * 60 * 1000;
constexpr uint64_t kProvisioningRestartDelayMs = 750;
constexpr size_t kProvisioningLineCapacity = 768;
constexpr uint32_t kEnvironmentPollIntervalMs = 30 * 1000;
constexpr uint32_t kWeatherPollIntervalMs = 15 * 60 * 1000;
constexpr char kEnvironmentMetricsUrl[] =
    "http://192.168.0.46:8080/v1/metrics";
constexpr char kWeatherForecastUrl[] =
    "http://api.open-meteo.com/v1/forecast?latitude=37.2296&longitude="
    "-80.4139&current=temperature_2m,relative_humidity_2m,"
    "apparent_temperature,weather_code&daily=weather_code,"
    "temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
    "rain_sum,showers_sum,snowfall_sum&temperature_unit=celsius&"
    "precipitation_unit=inch&timezone=America%2FNew_York&forecast_days=1";

// The M5GO base exposes its analog microphone on ADC1 GPIO34. M5Unified
// identifies the same input, but its I2S ADC compatibility path returns a
// constant signal on this ESP32 Arduino runtime. Direct ADC1 sampling is
// sufficient for an on-device energy envelope and remains usable with Wi-Fi.
constexpr gpio_num_t kMicPin = GPIO_NUM_34;
constexpr size_t kMicSamples = 128;
constexpr uint32_t kMicSampleRate = 8000;
constexpr uint32_t kMicSamplePeriodUs = 1000000 / kMicSampleRate;
constexpr uint8_t kOnBrightness = 255;
constexpr uint8_t kCooldownBrightness = 60;

int16_t micSamples[kMicSamples] = {};
uint16_t micProbeRaw = 0;
uint16_t micLastValidRaw = 2048;
M5Canvas displayFrame(&M5.Display);
M5Canvas displayHeaderFrame(&M5.Display);
RuntimeIdentity runtimeIdentity;
TelemetryQueue telemetryQueue;
TouchFeedbackQueue touchFeedbackQueue;
DeviceConfigMailbox configMailbox;
DashboardMailbox dashboardMailbox;
DashboardSnapshot dashboardSnapshot = {};
time_t dashboardClockSeconds = 0;
int64_t dashboardClockSecondToken = std::numeric_limits<int64_t>::min();
PresenceConfig activeConfig = defaultPresenceConfig();

PresenceState state = PresenceState::kCalibrating;
uint64_t bootMs = 0;
uint64_t stateSinceMs = 0;
uint64_t lastPirMs = 0;
uint64_t lastSoundMs = 0;
uint64_t lastDisplayMs = 0;
uint64_t lastFullDisplayMs = 0;
uint64_t lastSerialMs = 0;
uint64_t lastTelemetrySampleMs = 0;
uint64_t nextTelemetrySeq = 0;
uint64_t provisioningChallengeExpiresMs = 0;
uint64_t provisioningRestartMs = 0;

float micRms = 0.0f;
float micEnvelope = 0.0f;
int16_t micMin = 0;
int16_t micMax = 0;
float noiseFloor = 100.0f;
float soundThreshold = 300.0f;
bool pirHigh = false;
bool previousPirHigh = false;
bool soundActive = false;
bool tmosDetected = false;
bool baseImuDetected = false;
bool micBeginOk = false;
bool micEnvelopeInitialized = false;
bool displayFrameReady = false;
bool displayHeaderFrameReady = false;
bool displayDirty = true;
bool displayHeaderDirty = true;
bool deviceSettingsConfigured = false;
bool provisioningChallengeActive = false;
bool provisioningLineOverflow = false;
bool provisioningBrightnessOverride = false;
uint8_t currentBrightness = 255;
char provisioningChallenge[9] = {};
char provisioningLine[kProvisioningLineCapacity] = {};
size_t provisioningLineLength = 0;

const char* stateName(PresenceState value) {
  return presenceStateDisplayName(value);
}

uint16_t stateColor(PresenceState value) {
  switch (value) {
    case PresenceState::kCalibrating:
      return TFT_YELLOW;
    case PresenceState::kIdle:
      return TFT_DARKGREY;
    case PresenceState::kPresent:
      return TFT_GREEN;
    case PresenceState::kCooldown:
      return TFT_ORANGE;
  }
  return TFT_WHITE;
}

void setBrightness(uint8_t brightness) {
  if (brightness == currentBrightness) {
    return;
  }
  currentBrightness = brightness;
  M5.Display.setBrightness(brightness);
}

void ensureDisplayAwake() {
  M5.Display.wakeup();
  M5.Display.powerSaveOff();
  currentBrightness = 254;
  setBrightness(255);
}

void restoreStateBrightness() {
  switch (state) {
    case PresenceState::kCalibrating:
    case PresenceState::kPresent:
      setBrightness(kOnBrightness);
      break;
    case PresenceState::kCooldown:
      setBrightness(kCooldownBrightness);
      break;
    case PresenceState::kIdle:
      setBrightness(0);
      break;
  }
}

void securelyClear(void* data, size_t size) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  while (size-- > 0) {
    *bytes++ = 0;
  }
}

const char* provisioningErrorCode(ProvisioningError error) {
  switch (error) {
    case ProvisioningError::kOk:
      return "OK";
    case ProvisioningError::kNullArgument:
      return "NULL_ARGUMENT";
    case ProvisioningError::kInvalidCommand:
      return "INVALID_COMMAND";
    case ProvisioningError::kInvalidFieldCount:
      return "INVALID_FIELD_COUNT";
    case ProvisioningError::kInvalidChallenge:
      return "INVALID_CHALLENGE";
    case ProvisioningError::kInvalidBase64Url:
      return "INVALID_BASE64URL";
    case ProvisioningError::kDecodedFieldTooLong:
      return "FIELD_TOO_LONG";
    case ProvisioningError::kEmbeddedNul:
      return "EMBEDDED_NUL";
    case ProvisioningError::kInvalidSsid:
      return "INVALID_SSID";
    case ProvisioningError::kInvalidBaseUrl:
      return "INVALID_BASE_URL";
    case ProvisioningError::kInvalidToken:
      return "INVALID_TOKEN";
    case ProvisioningError::kInvalidSettings:
      return "INVALID_SETTINGS";
  }
  return "UNKNOWN";
}

void beginProvisioningChallenge(uint64_t now) {
  if (!runtimeIdentity.deviceIdValid) {
    Serial.println("PROVISION,ERROR,IDENTITY_INVALID");
    return;
  }
  std::snprintf(provisioningChallenge, sizeof(provisioningChallenge), "%08" PRIx32,
                esp_random());
  provisioningChallengeActive = true;
  provisioningChallengeExpiresMs =
      now + kProvisioningChallengeLifetimeMs;
  provisioningBrightnessOverride = true;
  displayDirty = true;
  M5.Display.wakeup();
  M5.Display.powerSaveOff();
  setBrightness(kOnBrightness);
  Serial.printf("PROVISION,CHALLENGE,%s,%s,%d\n", runtimeIdentity.deviceId,
                provisioningChallenge, deviceSettingsConfigured ? 1 : 0);
}

void processProvisioningLine(const char* line, size_t length, uint64_t now) {
  constexpr char kHello[] = "PROVISION,HELLO";
  constexpr char kSetPrefix[] = "PROVISION,SET,";
  if (length == sizeof(kHello) - 1 &&
      std::memcmp(line, kHello, sizeof(kHello) - 1) == 0) {
    beginProvisioningChallenge(now);
    return;
  }
  if (length < sizeof(kSetPrefix) - 1 ||
      std::memcmp(line, kSetPrefix, sizeof(kSetPrefix) - 1) != 0) {
    if (length >= sizeof("PROVISION,") - 1 &&
        std::memcmp(line, "PROVISION,", sizeof("PROVISION,") - 1) == 0) {
      Serial.println("PROVISION,ERROR,INVALID_COMMAND");
    }
    return;
  }
  if (!provisioningChallengeActive ||
      now >= provisioningChallengeExpiresMs) {
    provisioningChallengeActive = false;
    Serial.println("PROVISION,ERROR,INVALID_CHALLENGE");
    return;
  }

  DeviceSettings candidate;
  const ProvisioningError parseResult = parseProvisioningSetCommand(
      line, length, provisioningChallenge, std::strlen(provisioningChallenge),
      &candidate);
  if (parseResult != ProvisioningError::kOk) {
    securelyClear(&candidate, sizeof(candidate));
    Serial.printf("PROVISION,ERROR,%s\n",
                  provisioningErrorCode(parseResult));
    return;
  }

  const DeviceSettingsStorageResult saveResult = saveDeviceSettings(candidate);
  securelyClear(&candidate, sizeof(candidate));
  if (saveResult != DeviceSettingsStorageResult::kOk) {
    Serial.println("PROVISION,ERROR,STORAGE_WRITE_FAILED");
    return;
  }

  deviceSettingsConfigured = true;
  provisioningChallengeActive = false;
  provisioningChallenge[0] = '\0';
  Serial.printf("PROVISION,OK,%s,restart_required\n",
                runtimeIdentity.deviceId);
  Serial.flush();
  provisioningRestartMs = now + kProvisioningRestartDelayMs;
}

void pollProvisioningSerial(uint64_t now) {
  while (Serial.available() > 0) {
    const int incoming = Serial.read();
    if (incoming < 0) {
      break;
    }
    const char character = static_cast<char>(incoming);
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      if (provisioningLineOverflow) {
        Serial.println("PROVISION,ERROR,LINE_TOO_LONG");
      } else if (provisioningLineLength > 0) {
        processProvisioningLine(provisioningLine, provisioningLineLength, now);
      }
      securelyClear(provisioningLine, sizeof(provisioningLine));
      provisioningLineLength = 0;
      provisioningLineOverflow = false;
      continue;
    }
    if (provisioningLineLength < sizeof(provisioningLine)) {
      provisioningLine[provisioningLineLength++] = character;
    } else {
      provisioningLineOverflow = true;
    }
  }
}

void enqueueTransition(bool hasFromState, PresenceState fromState,
                       PresenceState toState, TransitionReason reason,
                       uint64_t now) {
  TelemetryRecord record;
  record.kind = TelemetryKind::kTransition;
  record.seq = nextTelemetrySeq++;
  record.uptimeMs = now;
  record.appliedConfigRevision = activeConfig.revision;
  record.transition.hasFromState = hasFromState;
  record.transition.fromState = fromState;
  record.transition.toState = toState;
  record.transition.reason = reason;

  const QueuePushResult result = telemetryQueue.push(record);
  if (result == QueuePushResult::kCriticalDropped) {
    Serial.printf("EVENT,telemetry_critical_drop,%" PRIu64 ",%" PRIu64 "\n",
                  now, record.seq);
  }
}

TelemetryRecord captureSampleRecord(uint64_t now, uint64_t seq) {
  TelemetryRecord record;
  record.kind = TelemetryKind::kSample;
  record.seq = seq;
  record.uptimeMs = now;
  record.appliedConfigRevision = activeConfig.revision;
  record.sample.pir = pirHigh;
  record.sample.micRms = micRms;
  record.sample.micEnvelope = micEnvelope;
  record.sample.micMin = micMin;
  record.sample.micMax = micMax;
  record.sample.noiseFloor = noiseFloor;
  record.sample.soundThreshold = soundThreshold;
  record.sample.soundActive = soundActive;
  record.sample.state = state;
  record.sample.brightness = currentBrightness;
  return record;
}

void enqueueSample(uint64_t now) {
  if (now - lastTelemetrySampleMs < activeConfig.telemetryIntervalMs) {
    return;
  }
  lastTelemetrySampleMs = now;

  const TelemetryRecord record = captureSampleRecord(now, nextTelemetrySeq++);

  const QueuePushResult result = telemetryQueue.push(record);
  if (result == QueuePushResult::kSampleDropped &&
      telemetryQueue.droppedSamples() % 60 == 1) {
    Serial.printf("EVENT,telemetry_sample_drop,%" PRIu64 ",%u\n", now,
                  telemetryQueue.droppedSamples());
  }
}

void applyPendingConfig(uint64_t now) {
  PresenceConfig pending = {};
  if (!configMailbox.take(&pending)) {
    return;
  }
  if (validatePresenceConfig(pending) !=
          PresenceConfigValidationError::kNone ||
      validatePresenceConfigDeviceCapabilities(pending) !=
          PresenceConfigCapabilityError::kNone) {
    Serial.println("EVENT,config,main_rejected_invalid");
    return;
  }
  if (pending.revision <= activeConfig.revision) {
    configMailbox.acknowledgeAppliedRevision(activeConfig.revision);
    Serial.printf("EVENT,config,main_ignored_stale,%" PRIu64 ",%" PRIu64
                  "\n",
                  pending.revision, activeConfig.revision);
    return;
  }

  const uint64_t previousRevision = activeConfig.revision;
  activeConfig = pending;
  enqueueTransition(true, state, state, TransitionReason::kConfigChange, now);
  // Publish the acknowledgement only after the revision boundary record is in
  // the telemetry queue. The worker cannot switch batching semantics earlier.
  configMailbox.acknowledgeAppliedRevision(activeConfig.revision);
  Serial.printf("EVENT,config,main_applied,%" PRIu64 ",%" PRIu64 "\n",
                previousRevision, activeConfig.revision);
}

void enterState(PresenceState next, TransitionReason reason, uint64_t now) {
  if (next == state) {
    return;
  }

  const PresenceState previous = state;
  state = next;
  stateSinceMs = now;
  displayDirty = true;

  switch (state) {
    case PresenceState::kCalibrating:
    case PresenceState::kPresent:
      setBrightness(kOnBrightness);
      break;
    case PresenceState::kCooldown:
      setBrightness(kCooldownBrightness);
      break;
    case PresenceState::kIdle:
      setBrightness(0);
      break;
  }

  enqueueTransition(true, previous, state, reason, now);
  Serial.printf("EVENT,state,%" PRIu64 ",%s,%s\n", now, stateName(state),
                transitionReasonWireName(reason));
}

float calculateRms(const int16_t* samples, size_t count) {
  int64_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    sum += samples[i];
  }

  const float mean = static_cast<float>(sum) / static_cast<float>(count);
  double squaredSum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const float centered = static_cast<float>(samples[i]) - mean;
    squaredSum += static_cast<double>(centered) * centered;
  }

  return sqrtf(static_cast<float>(squaredSum / count));
}

bool beginAnalogMicrophone() {
  pinMode(kMicPin, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(static_cast<uint8_t>(kMicPin), ADC_11db);
  micProbeRaw = analogRead(static_cast<uint8_t>(kMicPin));
  if (micProbeRaw > 0 && micProbeRaw < 4095) {
    micLastValidRaw = micProbeRaw;
  }
  return micProbeRaw <= 4095;
}

void sampleMicrophone(uint64_t now) {
  if (!micBeginOk) {
    micRms = 0.0f;
    soundActive = false;
    return;
  }

  uint32_t nextSampleUs = micros();
  for (size_t i = 0; i < kMicSamples; ++i) {
    while (static_cast<int32_t>(micros() - nextSampleUs) < 0) {
      delayMicroseconds(8);
    }
    uint16_t raw = analogRead(static_cast<uint8_t>(kMicPin));
    // The legacy ESP32 ADC occasionally returns an isolated exact rail value
    // while Wi-Fi and the LCD are active. Hold the previous valid sample so a
    // single conversion glitch cannot masquerade as a loud sound event.
    if (raw == 0 || raw == 4095) {
      raw = micLastValidRaw;
    } else {
      micLastValidRaw = raw;
    }
    const int32_t signedSample =
        (static_cast<int32_t>(raw) - 2048) * 16;
    micSamples[i] = static_cast<int16_t>(signedSample);
    nextSampleUs += kMicSamplePeriodUs;
  }

  micMin = micSamples[0];
  micMax = micSamples[0];
  for (size_t i = 1; i < kMicSamples; ++i) {
    micMin = std::min(micMin, micSamples[i]);
    micMax = std::max(micMax, micSamples[i]);
  }
  micRms = calculateRms(micSamples, kMicSamples);

  if (!micEnvelopeInitialized) {
    micEnvelope = micRms;
    noiseFloor = micRms;
    micEnvelopeInitialized = true;
  } else {
    // Fast enough to catch speech while the slower release bridges gaps
    // between syllables into one sound event.
    const float envelopeAlpha = micRms > micEnvelope ? 0.15f : 0.02f;
    micEnvelope += (micRms - micEnvelope) * envelopeAlpha;
  }

  if (state == PresenceState::kCalibrating) {
    // Fast averaging while the user keeps the room reasonably quiet.
    noiseFloor += (micEnvelope - noiseFloor) * 0.01f;
    soundActive = false;
  } else {
    // Follow HVAC/fan changes over tens of seconds. Above-threshold sound is
    // excluded so a person speaking cannot immediately teach the detector to
    // ignore their own voice.
    const float learningLimit = noiseFloor * 1.18f;
    if (micEnvelope < learningLimit) {
      noiseFloor += (micEnvelope - noiseFloor) * 0.0005f;
    }

    soundThreshold =
        fmaxf(noiseFloor * activeConfig.soundFactor, noiseFloor + 350.0f);
    const float releaseThreshold =
        fmaxf(noiseFloor *
                  (1.0f + (activeConfig.soundFactor - 1.0f) * 0.55f),
              noiseFloor + 175.0f);
    soundActive = soundActive ? micEnvelope > releaseThreshold
                              : micEnvelope > soundThreshold;
    if (soundActive && state != PresenceState::kIdle &&
        now - lastPirMs < activeConfig.maxSoundBridgeMs) {
      lastSoundMs = now;
    }
  }
}

bool enqueueTouchCorrection(TouchPresenceChoice choice, uint64_t now) {
  // Reserve this sequence exactly once even if the bounded queue is full. A
  // failed correction is observable as a gap, never as a reused identity.
  const TelemetryRecord preTouchSample =
      captureSampleRecord(now, nextTelemetrySeq++);
  TouchFeedbackEvidence evidence = {};
  if (!buildTouchFeedbackEvidence(runtimeIdentity.bootId, preTouchSample,
                                  choice, evidence)) {
    Serial.printf("EVENT,touch_feedback,rejected_build,%" PRIu64 "\n",
                  preTouchSample.seq);
    return false;
  }

  const TouchFeedbackQueuePushResult result = touchFeedbackQueue.push(evidence);
  if (result != TouchFeedbackQueuePushResult::kStored) {
    Serial.printf("EVENT,touch_feedback,rejected_queue,%" PRIu64 ",%u\n",
                  preTouchSample.seq, static_cast<unsigned>(result));
    return false;
  }
  Serial.printf("EVENT,touch_feedback,queued,%" PRIu64 ",%s\n",
                preTouchSample.seq,
                choice == TouchPresenceChoice::kPersonWasPresent ? "present"
                                                                  : "absent");
  return true;
}

bool readUserInput(uint64_t now) {
  // M5GO has three active-low physical buttons. Use the debounced press edge
  // instead of click/release so a long press can never disappear as a hold.
  const bool chosePresent = M5.BtnA.wasPressed();
  const bool choseWake = M5.BtnB.wasPressed();
  const bool choseAbsent = M5.BtnC.wasPressed();

  if (!chosePresent && !choseWake && !choseAbsent) {
    return false;
  }

  if (chosePresent) {
    Serial.printf("EVENT,button,A,pressed,%" PRIu64 "\n", now);
  }
  if (choseWake) {
    Serial.printf("EVENT,button,B,pressed,%" PRIu64 "\n", now);
  }
  if (choseAbsent) {
    Serial.printf("EVENT,button,C,pressed,%" PRIu64 "\n", now);
  }

  // The evidence pair snapshots the state before any button-induced mutation.
  // A full/invalid feedback queue therefore cannot alter the state machine.
  if (chosePresent) {
    if (!enqueueTouchCorrection(TouchPresenceChoice::kPersonWasPresent, now)) {
      return false;
    }
    lastPirMs = now;
    lastSoundMs = now;
    enterState(PresenceState::kPresent, TransitionReason::kTouchWake, now);
  } else if (choseAbsent) {
    if (!enqueueTouchCorrection(TouchPresenceChoice::kRoomWasAbsent, now)) {
      return false;
    }
    // ABSENT is a training label for the pre-touch observation, not a
    // fabricated state transition. Preserve the live state and merely skip
    // this iteration's automatic update so the evidence stays pre-touch.
  } else {
    // The center button remains a generic wake gesture and intentionally does
    // not create correction feedback.
    lastPirMs = now;
    lastSoundMs = now;
    enterState(PresenceState::kPresent, TransitionReason::kTouchWake, now);
  }

  // Do not let the normal state update immediately overwrite an explicit
  // button result in this same loop iteration.
  previousPirHigh = pirHigh;
  return true;
}

void logButtonPinChanges(uint64_t now) {
  const uint8_t pressedMask =
      (digitalRead(GPIO_NUM_39) == LOW ? 0x01 : 0x00) |
      (digitalRead(GPIO_NUM_38) == LOW ? 0x02 : 0x00) |
      (digitalRead(GPIO_NUM_37) == LOW ? 0x04 : 0x00);
  static uint8_t previousMask = 0xFF;
  if (pressedMask == previousMask) {
    return;
  }
  previousMask = pressedMask;
  Serial.printf("EVENT,button_gpio,%" PRIu64 ",A,%d,B,%d,C,%d\n", now,
                (pressedMask & 0x01) != 0 ? 1 : 0,
                (pressedMask & 0x02) != 0 ? 1 : 0,
                (pressedMask & 0x04) != 0 ? 1 : 0);
}

void updatePresenceState(uint64_t now) {
  const bool pirRising = pirHigh && !previousPirHigh;
  if (pirHigh) {
    lastPirMs = now;
  }

  switch (state) {
    case PresenceState::kCalibrating:
      if (now - bootMs >= kCalibrationMs) {
        if (pirHigh) {
          lastPirMs = now;
          lastSoundMs = now;
          enterState(PresenceState::kPresent,
                     TransitionReason::kCalibrationComplete, now);
        } else {
          enterState(PresenceState::kIdle,
                     TransitionReason::kCalibrationComplete, now);
        }
      }
      break;

    case PresenceState::kIdle:
      // Sound alone does not wake the screen: televisions and fans should not
      // create a false arrival. PIR is the authoritative wake signal.
      if (pirRising || pirHigh) {
        lastPirMs = now;
        lastSoundMs = now;
        enterState(PresenceState::kPresent, TransitionReason::kPirMotion, now);
      }
      break;

    case PresenceState::kPresent: {
      const bool minimumOnElapsed =
          now - stateSinceMs >= activeConfig.minimumOnMs;
      const bool pirQuiet = now - lastPirMs >= activeConfig.pirHoldMs;
      const bool soundQuiet = now - lastSoundMs >= activeConfig.soundHoldMs;
      if (minimumOnElapsed && pirQuiet && soundQuiet) {
        enterState(PresenceState::kCooldown, TransitionReason::kQuietTimeout,
                   now);
      }
      break;
    }

    case PresenceState::kCooldown:
      const bool soundCanBridge =
          soundActive && now - lastPirMs < activeConfig.maxSoundBridgeMs;
      if (pirHigh || soundCanBridge) {
        if (pirHigh) {
          lastPirMs = now;
        }
        if (soundCanBridge) {
          lastSoundMs = now;
        }
        enterState(PresenceState::kPresent,
                   pirHigh ? TransitionReason::kPirMotion
                           : TransitionReason::kSoundBridge,
                   now);
      } else if (now - stateSinceMs >= activeConfig.cooldownMs) {
        enterState(PresenceState::kIdle, TransitionReason::kCooldownTimeout,
                   now);
      }
      break;
  }

  previousPirHigh = pirHigh;
}

template <typename Canvas>
void drawProvisioningOverlay(Canvas& canvas) {
  canvas.fillRoundRect(18, 54, 284, 132, 10, TFT_NAVY);
  canvas.drawRoundRect(18, 54, 284, 132, 10, TFT_CYAN);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_WHITE, TFT_NAVY);
  canvas.setTextSize(2);
  canvas.drawString("USB PROVISIONING", 160, 78);
  canvas.setTextSize(1);
  canvas.drawString("Challenge shown over serial:", 160, 108);
  canvas.setTextSize(3);
  canvas.setTextColor(TFT_YELLOW, TFT_NAVY);
  canvas.drawString(provisioningChallenge, 160, 143);
  canvas.setTextDatum(top_left);
}

const char* weatherCondition(uint8_t code) {
  if (code == 0) {
    return "Clear";
  }
  if (code == 1) {
    return "Mostly clear";
  }
  if (code == 2) {
    return "Partly cloudy";
  }
  if (code == 3) {
    return "Overcast";
  }
  if (code == 45 || code == 48) {
    return "Fog";
  }
  if (code >= 51 && code <= 57) {
    return "Drizzle";
  }
  if (code >= 61 && code <= 67) {
    return "Rain";
  }
  if (code >= 71 && code <= 77) {
    return "Snow";
  }
  if (code >= 80 && code <= 82) {
    return "Showers";
  }
  if (code >= 85 && code <= 86) {
    return "Snow showers";
  }
  if (code >= 95 && code <= 99) {
    return "Thunderstorm";
  }
  return "Weather";
}

template <typename Canvas>
void drawDashboardHeader(Canvas& canvas) {
  canvas.fillRect(0, 0, canvas.width(), kDashboardHeaderHeight, TFT_BLACK);
  canvas.setTextDatum(top_left);
  canvas.setTextSize(2);
  canvas.setTextColor(TFT_CYAN, TFT_BLACK);
  canvas.drawString("BLACKSBURG", 8, 3);
  canvas.setTextSize(1);
  canvas.setTextDatum(top_right);
  canvas.setTextColor(stateColor(state), TFT_BLACK);
  canvas.drawString(stateName(state), 312, 6);

  char dateTime[kDashboardDateTimeCapacity] = {};
  if (!formatDashboardEasternDateTime(dashboardClockSeconds, dateTime,
                                      sizeof(dateTime))) {
    std::snprintf(dateTime, sizeof(dateTime), "TIME SYNCING...");
  }
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString(dateTime, 160, 24);
  canvas.setTextDatum(top_left);
  canvas.drawFastHLine(8, 31, 304, TFT_DARKGREY);
}

template <typename Canvas>
void drawDashboard(Canvas& canvas, uint64_t now) {
  canvas.fillScreen(TFT_BLACK);
  drawDashboardHeader(canvas);

  canvas.drawRoundRect(6, 36, 151, 134, 7, TFT_DARKGREY);
  canvas.setTextSize(1);
  const bool weatherCached =
      dashboardSnapshot.weather.valid &&
      dashboardSnapshot.weatherHealth.consecutiveFailures != 0;
  canvas.setTextColor(weatherCached ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString(weatherCached ? "OUTSIDE - CACHED" : "OUTSIDE", 14, 43);
  if (dashboardSnapshot.weather.valid) {
    const WeatherReading& weather = dashboardSnapshot.weather;
    char value[40] = {};
    canvas.setTextSize(4);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    std::snprintf(value, sizeof(value), "%.0fC", weather.currentTemperatureC);
    canvas.drawString(value, 13, 57);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.drawString(weatherCondition(weather.currentWeatherCode), 13, 91);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    std::snprintf(value, sizeof(value), "Feels %.0fC  RH %.0f%%",
                  weather.apparentTemperatureC,
                  weather.currentHumidityPct);
    canvas.drawString(value, 13, 119);
    std::snprintf(value, sizeof(value), "High %.0fC Low %.0fC",
                  weather.temperatureMaxC, weather.temperatureMinC);
    canvas.drawString(value, 13, 136);
    std::snprintf(value, sizeof(value), "Precip %.0f%%",
                  weather.precipitationProbabilityMaxPct);
    canvas.drawString(value, 13, 153);
  } else {
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.drawString(
        dashboardSnapshot.weatherHealth.consecutiveFailures == 0
            ? "Loading..."
            : "Unavailable",
        13, 76);
    canvas.setTextSize(1);
    canvas.drawString("Waiting for Open-Meteo", 13, 110);
  }

  canvas.drawRoundRect(163, 36, 151, 134, 7, TFT_DARKGREY);
  canvas.setTextSize(1);
  const bool environmentStale =
      dashboardSnapshot.environment.valid &&
      (dashboardSnapshot.environment.status == EnvironmentStatus::kOutDated ||
       dashboardSnapshot.environmentHealth.consecutiveFailures != 0);
  canvas.setTextColor(environmentStale ? TFT_ORANGE : TFT_LIGHTGREY,
                      TFT_BLACK);
  canvas.drawString(environmentStale ? "INDOOR - CACHED" : "INDOOR", 171, 43);
  if (dashboardSnapshot.environment.valid) {
    const EnvironmentReading& environment = dashboardSnapshot.environment;
    char value[40] = {};
    canvas.setTextSize(3);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    std::snprintf(value, sizeof(value), "%.1fC",
                  environment.temperatureC);
    canvas.drawString(value, 170, 62);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    std::snprintf(value, sizeof(value), "RH %.0f%%", environment.humidityPct);
    canvas.drawString(value, 170, 98);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    std::snprintf(value, sizeof(value), "Pressure %.0f hPa",
                  environment.pressureHpa);
    canvas.drawString(value, 170, 128);
  } else {
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.drawString(
        dashboardSnapshot.environmentHealth.consecutiveFailures == 0
            ? "Loading..."
            : "Unavailable",
        170, 76);
    canvas.setTextSize(1);
    canvas.drawString("Waiting for devb", 170, 110);
  }

  char weatherAge[24] = {};
  char environmentAge[24] = {};
  formatDashboardFeedFreshness(
      dashboardSnapshot.weather.valid, now,
      dashboardSnapshot.weatherHealth.fetchedAtUptimeMs, weatherAge,
      sizeof(weatherAge));
  formatDashboardFeedFreshness(
      dashboardSnapshot.environment.valid, now,
      dashboardSnapshot.environmentHealth.fetchedAtUptimeMs, environmentAge,
      sizeof(environmentAge));
  canvas.setTextSize(1);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  char footer[64] = {};
  if (dashboardSnapshot.weather.valid) {
    std::snprintf(footer, sizeof(footer),
                  "Rain %.2fin  Showers %.2fin  Snow %.2fin",
                  dashboardSnapshot.weather.rainSumIn,
                  dashboardSnapshot.weather.showersSumIn,
                  dashboardSnapshot.weather.snowfallSumIn);
    canvas.drawString(footer, 8, 176);
  }
  std::snprintf(footer, sizeof(footer),
                "Fetched: Open-Meteo %s | Room %s",
                weatherAge, environmentAge);
  canvas.drawString(footer, 8, 193);

  canvas.drawRect(0, 207, 105, 33, TFT_DARKGREY);
  canvas.drawRect(105, 207, 110, 33, TFT_DARKGREY);
  canvas.drawRect(215, 207, 105, 33, TFT_DARKGREY);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_GREEN, TFT_BLACK);
  canvas.drawString("A PRESENT", 52, 223);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("B WAKE", 160, 223);
  canvas.setTextColor(TFT_RED, TFT_BLACK);
  canvas.drawString("C ABSENT", 267, 223);
  canvas.setTextDatum(top_left);
}

void refreshDashboardSnapshot() {
  DashboardSnapshot latest = {};
  if (!dashboardMailbox.copySnapshot(&latest) ||
      latest.version == dashboardSnapshot.version) {
    return;
  }
  dashboardSnapshot = latest;
  displayDirty = true;
}

void refreshDashboardClock() {
  time_t currentSeconds = 0;
  int64_t secondToken = -1;
  if (dashboardSnapshot.clockSynchronized) {
    currentSeconds = std::time(nullptr);
    if (currentSeconds >= kMinimumDashboardEpochSeconds) {
      secondToken = static_cast<int64_t>(currentSeconds);
    }
  }
  if (secondToken == dashboardClockSecondToken) {
    return;
  }
  dashboardClockSecondToken = secondToken;
  dashboardClockSeconds = secondToken >= 0 ? currentSeconds : 0;
  displayHeaderDirty = true;
}

void drawDisplay(uint64_t now) {
  if (provisioningChallengeActive &&
      now >= provisioningChallengeExpiresMs) {
    provisioningChallengeActive = false;
    provisioningChallenge[0] = '\0';
    displayDirty = true;
  }
  if (provisioningChallengeActive) {
    setBrightness(kOnBrightness);
  } else if (provisioningBrightnessOverride) {
    provisioningBrightnessOverride = false;
    restoreStateBrightness();
  }
  const bool heartbeatDue = now - lastFullDisplayMs >= kDisplayHeartbeatMs;
  const bool fullRedraw = displayDirty || heartbeatDue;
  if (currentBrightness == 0 || (!fullRedraw && !displayHeaderDirty) ||
      now - lastDisplayMs < kDisplayMinimumIntervalMs) {
    return;
  }
  lastDisplayMs = now;

  if (!fullRedraw) {
    if (displayHeaderFrameReady) {
      drawDashboardHeader(displayHeaderFrame);
      M5.Display.startWrite();
      displayHeaderFrame.pushSprite(0, 0);
      M5.Display.endWrite();
    } else {
      M5.Display.startWrite();
      drawDashboardHeader(M5.Display);
      M5.Display.endWrite();
    }
    displayHeaderDirty = false;
    return;
  }

  lastFullDisplayMs = now;
  if (!displayFrameReady) {
    drawDashboard(M5.Display, now);
    if (provisioningChallengeActive) {
      drawProvisioningOverlay(M5.Display);
    }
    displayDirty = false;
    displayHeaderDirty = false;
    return;
  }

  drawDashboard(displayFrame, now);

  if (provisioningChallengeActive) {
    drawProvisioningOverlay(displayFrame);
  }

  M5.Display.startWrite();
  displayFrame.pushSprite(0, 0);
  M5.Display.endWrite();
  displayDirty = false;
  displayHeaderDirty = false;
}

void printSerial(uint64_t now) {
  if (now - lastSerialMs < kSerialIntervalMs) {
    return;
  }
  lastSerialMs = now;
  Serial.printf(
      "DATA,%" PRIu64 ",%d,%.1f,%.1f,%d,%d,%.1f,%.1f,%d,%s,%u\n", now,
      pirHigh ? 1 : 0, micRms, micEnvelope, micMin, micMax, noiseFloor,
      soundThreshold, soundActive ? 1 : 0, stateName(state), currentBrightness);
}

}  // namespace

void setup() {
  auto config = M5.config();
  config.serial_baudrate = 115200;
  config.clear_display = true;
  config.output_power = true;
  config.fallback_board = m5::board_t::board_M5Stack;
  config.internal_mic = false;
  config.internal_spk = false;
  M5.begin(config);
  baseImuDetected = M5.Imu.isEnabled();

  Serial.begin(115200);
  delay(200);

  M5.Display.setRotation(1);
  M5.Display.setTextWrap(false);
  ensureDisplayAwake();
  displayHeaderFrame.setPsram(false);
  displayHeaderFrame.setColorDepth(8);
  displayHeaderFrameReady = displayHeaderFrame.createSprite(
      M5.Display.width(), kDashboardHeaderHeight);
  displayFrame.setPsram(false);
  displayFrame.setColorDepth(8);
  displayFrameReady =
      displayFrame.createSprite(M5.Display.width(), M5.Display.height());

  // Brief boot card; the production dashboard replaces it as soon as setup
  // completes. Keep this short so restart does not feel like a display test.
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.drawString("M5GO DESK", M5.Display.width() / 2,
                        M5.Display.height() / 2 - 12);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Display.drawString("Starting sensors...", M5.Display.width() / 2,
                        M5.Display.height() / 2 + 24);
  delay(600);
  M5.Display.setTextDatum(top_left);

  pinMode(kPirPin, INPUT);
  // The attached motion sensor is the digital GPIO36 input on M5GO PORT.B.
  // GPIO36 is input-only and has no internal pull resistor, so preserve the
  // sensor module's own output conditioning and use plain INPUT mode.
  tmosDetected = false;

  // Keep the unused M5GO base speaker at a defined low level, then sample its
  // analog microphone directly from ADC1 GPIO34. ADC1 remains available while
  // Wi-Fi is active (unlike ADC2).
  pinMode(GPIO_NUM_25, OUTPUT);
  digitalWrite(GPIO_NUM_25, LOW);
  ensureDisplayAwake();
  micBeginOk = beginAnalogMicrophone();

  runtimeIdentity = createRuntimeIdentity();

  PresenceConfig storedConfig = defaultPresenceConfig();
  const DeviceConfigStorageResult configStorageResult =
      loadStoredDeviceConfig(&storedConfig);
  if (configStorageResult == DeviceConfigStorageResult::kOk) {
    activeConfig = storedConfig;
  } else {
    activeConfig = defaultPresenceConfig();
  }
  configMailbox.acknowledgeAppliedRevision(activeConfig.revision);

  // NVS access is outside the sensor calibration window. The monotonic boot
  // baseline begins only after the configuration for this run is known.
  bootMs = monotonicMillis();
  stateSinceMs = bootMs;
  lastPirMs = bootMs;
  lastSoundMs = bootMs;
  lastTelemetrySampleMs = bootMs;

  // The first telemetry record must describe the configuration that was
  // actually loaded for this boot, never an earlier compile-time placeholder.
  enqueueTransition(false, PresenceState::kCalibrating,
                    PresenceState::kCalibrating, TransitionReason::kBoot,
                    bootMs);

  Serial.println("M5GO Presence Lab v0.6.2");
  Serial.printf("IDENTITY,device_id,%s,boot_id,%s,valid,%d\n",
                runtimeIdentity.deviceId, runtimeIdentity.bootId,
                runtimeIdentity.deviceIdValid ? 1 : 0);
  Serial.printf("CONFIG,revision,%" PRIu64 ",storage_status,%u\n",
                activeConfig.revision,
                static_cast<unsigned>(configStorageResult));
  Serial.printf(
      "DEVICE,pir_gpio,%d,tmos_0x5a,%d,mic_started,%d,mic_gpio,34,"
      "mic_probe,%u,driver,adc1_poll,base_imu,%d,board,%d,pmic,%d,"
      "input_mode,buttons,button_gpio,A39_B38_C37,display,%dx%d,"
      "frame_buffer,%d,header_buffer,%d,psram,%u\n",
                static_cast<int>(kPirPin), tmosDetected ? 1 : 0,
                micBeginOk ? 1 : 0, static_cast<unsigned>(micProbeRaw),
                baseImuDetected ? 1 : 0,
                static_cast<int>(M5.getBoard()),
                static_cast<int>(M5.Power.getType()),
                M5.Display.width(), M5.Display.height(),
                displayFrameReady ? 1 : 0,
                displayHeaderFrameReady ? 1 : 0, ESP.getPsramSize());
  Serial.println(
      "CSV,type,ms,pir,mic_rms,mic_envelope,mic_min,mic_max,noise,"
      "threshold,sound,state,brightness");

  DeviceSettings loadedSettings;
  const DeviceSettingsStorageResult settingsResult =
      loadDeviceSettings(&loadedSettings);
  deviceSettingsConfigured = settingsResult == DeviceSettingsStorageResult::kOk;
  TelemetryUploaderSettings uploaderSettings;
  uploaderSettings.configured = deviceSettingsConfigured;
  uploaderSettings.initialConfig = activeConfig;
  uploaderSettings.startAfterUptimeMs = bootMs + kCalibrationMs + 1000;
  uploaderSettings.environmentPollIntervalMs = kEnvironmentPollIntervalMs;
  const int environmentUrlLength =
      std::snprintf(uploaderSettings.environmentUrl,
                    sizeof(uploaderSettings.environmentUrl), "%s",
                    kEnvironmentMetricsUrl);
  uploaderSettings.environmentEnabled =
      deviceSettingsConfigured && environmentUrlLength > 0 &&
      static_cast<size_t>(environmentUrlLength) <
          sizeof(uploaderSettings.environmentUrl);
  uploaderSettings.weatherPollIntervalMs = kWeatherPollIntervalMs;
  const int weatherUrlLength =
      std::snprintf(uploaderSettings.weatherUrl,
                    sizeof(uploaderSettings.weatherUrl), "%s",
                    kWeatherForecastUrl);
  uploaderSettings.weatherEnabled =
      deviceSettingsConfigured && weatherUrlLength > 0 &&
      static_cast<size_t>(weatherUrlLength) <
          sizeof(uploaderSettings.weatherUrl);
  if (deviceSettingsConfigured) {
    std::memcpy(uploaderSettings.wifiSsid, loadedSettings.ssid,
                sizeof(loadedSettings.ssid));
    std::memcpy(uploaderSettings.wifiPassword, loadedSettings.password,
                sizeof(loadedSettings.password));
    std::memcpy(uploaderSettings.serverBaseUrl, loadedSettings.baseUrl,
                sizeof(loadedSettings.baseUrl));
    std::memcpy(uploaderSettings.apiToken, loadedSettings.token,
                sizeof(loadedSettings.token));
  }
  const bool uploaderStarted =
      startTelemetryUploader(runtimeIdentity, uploaderSettings, telemetryQueue,
                             configMailbox, touchFeedbackQueue,
                             dashboardMailbox);
  securelyClear(&loadedSettings, sizeof(loadedSettings));
  securelyClear(&uploaderSettings, sizeof(uploaderSettings));
  Serial.printf("NETWORK,configured,%d,uploader_started,%d,settings_status,%u\n",
                deviceSettingsConfigured ? 1 : 0, uploaderStarted ? 1 : 0,
                static_cast<unsigned>(settingsResult));
}

void loop() {
  const uint64_t now = monotonicMillis();
  applyPendingConfig(now);
  refreshDashboardSnapshot();
  refreshDashboardClock();
  M5.update();
  logButtonPinChanges(now);

  pirHigh = digitalRead(kPirPin) == HIGH;
  sampleMicrophone(now);
  const bool userInputChangedState = readUserInput(now);
  if (!userInputChangedState) {
    updatePresenceState(now);
  }
  drawDisplay(now);
  printSerial(now);
  enqueueSample(now);
  pollProvisioningSerial(now);

  if (provisioningRestartMs != 0 && now >= provisioningRestartMs) {
    Serial.flush();
    ESP.restart();
  }

  delay(1);
}
