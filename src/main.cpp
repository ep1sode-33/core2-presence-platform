#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <inttypes.h>
#include <limits>
#include <esp_system.h>

#include "device_config.h"
#include "backlog_policy.h"
#include "device_config_mailbox.h"
#include "device_config_storage.h"
#include "device_settings.h"
#include "dashboard_mailbox.h"
#include "dashboard_time.h"
#include "control_mailbox.h"
#include "crash_context.h"
#include "firmware_info.h"
#include "health_snapshot.h"
#include "operational_log.h"
#include "ota_boot_validation.h"
#include "ota_runtime_mailbox.h"
#include "ota_update.h"
#include "presence_types.h"
#include "provisioning_protocol.h"
#include "runtime_identity.h"
#include "sensor_health.h"
#include "telemetry.h"
#include "telemetry_uploader.h"
#include "touch_feedback_queue.h"

namespace {

// M5GO PORT.B: white wire / digital input is GPIO36.
constexpr gpio_num_t kPirPin = GPIO_NUM_36;

constexpr uint64_t kCalibrationMs = 5000;
constexpr uint64_t kDisplayMinimumIntervalMs = 50;
constexpr uint64_t kDisplayHeartbeatMs = 60 * 1000;
constexpr uint64_t kHealthRefreshMs = 1000;
constexpr uint64_t kDiagnosticsTimeoutMs = 60 * 1000;
constexpr uint32_t kDiagnosticsHoldMs = 1500;
constexpr uint64_t kDiagnosticsOtaGestureMs = 3000;
constexpr uint64_t kRemoteOtaConfirmationTimeoutMs = 120 * 1000;
constexpr uint64_t kMicrophoneRecalibrationMs = 5000;
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

int16_t micSamples[kMicSamples] = {};
uint16_t micProbeRaw = 0;
uint16_t micLastValidRaw = 2048;
M5Canvas displayFrame(&M5.Display);
M5Canvas displayHeaderFrame(&M5.Display);
RuntimeIdentity runtimeIdentity;
CrashContextRotationResult crashContextRotation;
CrashDumpAttribution activeCrashDumpAttribution;
CrashDumpAttributionStorageResult crashAttributionStorageResult =
    CrashDumpAttributionStorageResult::kNotStored;
TelemetryQueue telemetryQueue;
TouchFeedbackQueue touchFeedbackQueue;
DeviceConfigMailbox configMailbox;
DashboardMailbox dashboardMailbox;
DeviceHealthMailbox healthMailbox;
ControlMailbox controlMailbox;
OperationalLogRing operationalLogRing;
OtaRuntimeMailbox otaRuntimeMailbox;
OtaDelayedBootValidator otaBootValidator;
DashboardSnapshot dashboardSnapshot = {};
DeviceHealthSnapshot healthSnapshot = {};
OtaRuntimeSnapshot otaRuntimeSnapshot = {};
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
uint64_t lastHealthRefreshMs = 0;
uint64_t lastDiagnosticsActivityMs = 0;
uint64_t lastDiagnosticsRenderMs = 0;
uint64_t lastTransitionMs = 0;
uint64_t lastDetailedLogMs = 0;
uint64_t diagnosticsOtaGestureStartedMs = 0;
uint64_t remoteOtaConfirmationDeadlineMs = 0;
uint64_t micRecalibrationUntilMs = 0;
uint64_t nextTelemetrySeq = 0;
uint64_t nextOperationalLogSeq = 0;
uint64_t provisioningChallengeExpiresMs = 0;
uint64_t provisioningRestartMs = 0;
uint64_t otaSafetyPreviousLoopMs = 0;

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
SensorHealthStatus microphoneHealth = SensorHealthStatus::kUnknown;
SensorHealthStatus pirHealth = SensorHealthStatus::kUnknown;
SensorHealthLatch microphoneHealthLatch;
PirHealthTracker pirHealthTracker;
bool displayFrameReady = false;
bool displayHeaderFrameReady = false;
bool displayDirty = true;
bool displayHeaderDirty = true;
bool deviceSettingsConfigured = false;
bool provisioningChallengeActive = false;
bool provisioningLineOverflow = false;
bool crashDumpAttributionAvailable = false;
bool provisioningBrightnessOverride = false;
bool diagnosticsVisible = false;
bool bootStabilityRecorded = false;
bool safeMode = false;
bool displayInitializationHealthy = false;
bool configStorageHealthy = false;
bool settingsStorageHealthy = false;
bool otaBootValidatorStarted = false;
bool otaBootNoticePublished = false;
bool diagnosticsOtaGestureTriggered = false;
bool remoteOtaConfirmationPending = false;
bool mainControlRequestActive = false;
bool otaSafetyMetricsActive = false;
bool otaSafetyAbortRequested = false;
uint8_t currentBrightness = 255;
uint32_t bootCount = 0;
uint32_t otaMaximumMainLoopGapMs = 0;
uint32_t otaInvalidMicrophoneWindows = 0;
uint32_t otaTotalMicrophoneWindows = 0;
uint8_t otaConsecutiveInvalidMicrophoneWindows = 0;
TransitionReason lastTransitionReason = TransitionReason::kBoot;
char resetReasonName[DeviceHealthSnapshot::kResetReasonCapacity] = "unknown";
char provisioningChallenge[9] = {};
char provisioningLine[kProvisioningLineCapacity] = {};
size_t provisioningLineLength = 0;
MainControlRequest activeMainControlRequest = {};

void enterDiagnostics(uint64_t now);

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

void enqueueOperationalEvent(OperationalLogLevel level,
                             OperationalLogCode code, int32_t value0,
                             int32_t value1, uint64_t now) {
  OperationalLogEvent event = {};
  event.sequence = nextOperationalLogSeq++;
  event.uptimeMs = now;
  event.level = level;
  event.code = code;
  event.value0 = value0;
  event.value1 = value1;
  const OperationalLogPushResult result = operationalLogRing.push(event);
  if (result == OperationalLogPushResult::kDroppedCritical) {
    Serial.printf("EVENT,operational_log,critical_drop,%" PRIu64 "\n", now);
  }
}

bool otaTransferPhase(OtaRuntimePhase phase) {
  return phase == OtaRuntimePhase::kDevelopmentUploading ||
         phase == OtaRuntimePhase::kProductionDownloading ||
         phase == OtaRuntimePhase::kProductionVerifying;
}

OtaSafetyAbortRequest currentOtaSafetyMetrics() {
  OtaSafetyAbortRequest metrics = {};
  metrics.maximumMainLoopGapMs = otaMaximumMainLoopGapMs;
  metrics.invalidMicrophoneWindows = otaInvalidMicrophoneWindows;
  metrics.totalMicrophoneWindows = otaTotalMicrophoneWindows;
  metrics.consecutiveInvalidMicrophoneWindows =
      otaConsecutiveInvalidMicrophoneWindows;
  return metrics;
}

void publishOtaSafetyMetrics() {
  if (!otaSafetyMetricsActive) {
    return;
  }
  otaRuntimeMailbox.publishSafetyMetrics(currentOtaSafetyMetrics());
}

void requestOtaSafetyAbort(uint64_t now) {
  if (otaSafetyAbortRequested) {
    return;
  }
  const OtaSafetyAbortRequest request = currentOtaSafetyMetrics();
  if (otaRuntimeMailbox.requestSafetyAbort(request)) {
    otaSafetyAbortRequested = true;
    enqueueOperationalEvent(OperationalLogLevel::kWarning,
                            OperationalLogCode::kOtaChanged,
                            static_cast<int32_t>(request.maximumMainLoopGapMs),
                            static_cast<int32_t>(request.invalidMicrophoneWindows),
                            now);
    Serial.printf("EVENT,ota,safety_abort_request,gap,%u,mic,%u,%u\n",
                  request.maximumMainLoopGapMs,
                  request.invalidMicrophoneWindows,
                  request.totalMicrophoneWindows);
  }
}

void updateOtaLoopSafety(uint64_t now) {
  const bool active = otaTransferPhase(otaRuntimeSnapshot.phase);
  if (!active) {
    if (otaSafetyMetricsActive) {
      otaRuntimeMailbox.publishSafetyMetrics(currentOtaSafetyMetrics());
      enqueueOperationalEvent(
          otaMaximumMainLoopGapMs >= 250
              ? OperationalLogLevel::kWarning
              : OperationalLogLevel::kInfo,
          OperationalLogCode::kOtaChanged,
          static_cast<int32_t>(otaMaximumMainLoopGapMs),
          static_cast<int32_t>(otaInvalidMicrophoneWindows), now);
      Serial.printf("EVENT,ota,safety_metrics,gap,%u,mic,%u,%u\n",
                    otaMaximumMainLoopGapMs,
                    otaInvalidMicrophoneWindows,
                    otaTotalMicrophoneWindows);
    }
    otaSafetyMetricsActive = false;
    otaSafetyPreviousLoopMs = now;
    return;
  }

  if (!otaSafetyMetricsActive) {
    otaSafetyMetricsActive = true;
    otaSafetyAbortRequested = false;
    otaMaximumMainLoopGapMs = 0;
    otaInvalidMicrophoneWindows = 0;
    otaTotalMicrophoneWindows = 0;
    otaConsecutiveInvalidMicrophoneWindows = 0;
    otaSafetyPreviousLoopMs = now;
    publishOtaSafetyMetrics();
    return;
  }

  const uint64_t gap64 = now >= otaSafetyPreviousLoopMs
                             ? now - otaSafetyPreviousLoopMs
                             : 0;
  otaSafetyPreviousLoopMs = now;
  const uint32_t gap = static_cast<uint32_t>(
      std::min<uint64_t>(gap64, std::numeric_limits<uint32_t>::max()));
  otaMaximumMainLoopGapMs = std::max(otaMaximumMainLoopGapMs, gap);
  if (gap >= 500) {
    requestOtaSafetyAbort(now);
  }
}

void recordOtaMicrophoneWindow(bool timingValid, uint64_t now) {
  if (!otaSafetyMetricsActive) {
    return;
  }
  if (otaTotalMicrophoneWindows != std::numeric_limits<uint32_t>::max()) {
    ++otaTotalMicrophoneWindows;
  }
  if (timingValid) {
    otaConsecutiveInvalidMicrophoneWindows = 0;
  } else {
    if (otaInvalidMicrophoneWindows != std::numeric_limits<uint32_t>::max()) {
      ++otaInvalidMicrophoneWindows;
    }
    if (otaConsecutiveInvalidMicrophoneWindows !=
        std::numeric_limits<uint8_t>::max()) {
      ++otaConsecutiveInvalidMicrophoneWindows;
    }
    if (otaConsecutiveInvalidMicrophoneWindows > 5) {
      requestOtaSafetyAbort(now);
    }
  }
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
  setBrightness(presenceStateBrightness(state));
}

const char* resetReasonWireName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power_on";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
      return "task_watchdog";
    case ESP_RST_WDT:
      return "other_watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep_sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    case ESP_RST_UNKNOWN:
      return "unknown";
  }
  return "unknown";
}

bool resetReasonMayHaveCoreDump(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

void initializeCrashDumpAttribution() {
  const esp_reset_reason_t reason = esp_reset_reason();
  activeCrashDumpAttribution = {};
  crashDumpAttributionAvailable = false;

  if (!resetReasonMayHaveCoreDump(reason)) {
    crashAttributionStorageResult =
        loadPinnedCrashDumpAttribution(&activeCrashDumpAttribution);
    crashDumpAttributionAvailable =
        crashAttributionStorageResult ==
        CrashDumpAttributionStorageResult::kOk;
    return;
  }

  // A fresh crash supersedes any pin for an older, possibly lingering dump.
  // Clear first so a failed replacement cannot be mistaken for this crash on
  // a later ordinary reboot.
  clearPinnedCrashDumpAttribution();
  if (!crashContextRotation.hasPrevious()) {
    crashAttributionStorageResult =
        CrashDumpAttributionStorageResult::kNotStored;
    return;
  }
  std::snprintf(activeCrashDumpAttribution.bootId,
                sizeof(activeCrashDumpAttribution.bootId), "%s",
                crashContextRotation.previous.bootId);
  std::snprintf(activeCrashDumpAttribution.buildId,
                sizeof(activeCrashDumpAttribution.buildId), "%s",
                crashContextRotation.previous.buildId);
  std::snprintf(activeCrashDumpAttribution.resetReason,
                sizeof(activeCrashDumpAttribution.resetReason), "%s",
                resetReasonWireName(reason));
  crashAttributionStorageResult =
      savePinnedCrashDumpAttribution(activeCrashDumpAttribution);
  crashDumpAttributionAvailable =
      crashAttributionStorageResult ==
      CrashDumpAttributionStorageResult::kOk;
  if (!crashDumpAttributionAvailable) {
    activeCrashDumpAttribution = {};
    clearPinnedCrashDumpAttribution();
  }
}

void initializeBootDiagnostics() {
  const esp_reset_reason_t resetReason = esp_reset_reason();
  std::snprintf(resetReasonName, sizeof(resetReasonName), "%s",
                resetReasonWireName(resetReason));
  Preferences preferences;
  if (!preferences.begin("m5health", false)) {
    safeMode = false;
    return;
  }
  const uint32_t priorBootCount = preferences.getUInt("boot_count", 0);
  bootCount = priorBootCount == UINT32_MAX ? UINT32_MAX : priorBootCount + 1;
  const bool priorBootPending = preferences.getBool("boot_pending", false);
  uint32_t earlyBoots = preferences.getUInt("early_boots", 0);
  const bool abnormalReset =
      resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT ||
      resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT ||
      resetReason == ESP_RST_BROWNOUT;
  earlyBoots = priorBootPending && abnormalReset
                   ? (earlyBoots == UINT32_MAX ? UINT32_MAX : earlyBoots + 1)
                   : 0;
  safeMode = earlyBoots >= 3;
  preferences.putUInt("boot_count", bootCount);
  preferences.putUInt("early_boots", earlyBoots);
  preferences.putBool("boot_pending", true);
  preferences.end();
}

void markBootStableIfNeeded(uint64_t now) {
  if (bootStabilityRecorded || now - bootMs < 60 * 1000) {
    return;
  }
  Preferences preferences;
  if (preferences.begin("m5health", false)) {
    preferences.putBool("boot_pending", false);
    preferences.putUInt("early_boots", 0);
    preferences.end();
  }
  bootStabilityRecorded = true;
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
    case ProvisioningError::kInvalidOtaSecret:
      return "INVALID_OTA_SECRET";
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
                       uint64_t now, uint8_t brightnessBefore) {
  TelemetryRecord record;
  record.kind = TelemetryKind::kTransition;
  record.seq = nextTelemetrySeq++;
  record.uptimeMs = now;
  record.appliedConfigRevision = activeConfig.revision;
  record.transition.hasFromState = hasFromState;
  record.transition.fromState = fromState;
  record.transition.toState = toState;
  record.transition.reason = reason;
  record.transition.pir = pirHigh;
  record.transition.pirAgeMs = now >= lastPirMs ? now - lastPirMs : 0;
  record.transition.soundActive = soundActive;
  record.transition.soundAgeMs = now >= lastSoundMs ? now - lastSoundMs : 0;
  record.transition.micEnvelope = micEnvelope;
  record.transition.noiseFloor = noiseFloor;
  record.transition.soundThreshold = soundThreshold;
  record.transition.brightnessBefore = brightnessBefore;
  record.transition.brightnessAfter = currentBrightness;

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
  const uint64_t effectiveIntervalMs = adaptiveTelemetryIntervalMs(
      activeConfig.telemetryIntervalMs, healthSnapshot.spoolFiles);
  // Preserve high-resolution data around normal operation, but progressively
  // thin only ordinary samples during a long outage. Transitions and feedback
  // bypass this path and retain their immediate priority.
  if (now - lastTelemetrySampleMs < effectiveIntervalMs) {
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
  enqueueTransition(true, state, state, TransitionReason::kConfigChange, now,
                    currentBrightness);
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
  const uint8_t brightnessBefore = currentBrightness;
  state = next;
  stateSinceMs = now;
  displayDirty = true;

  if (!diagnosticsVisible) {
    restoreStateBrightness();
  }

  enqueueTransition(true, previous, state, reason, now, brightnessBefore);
  enqueueOperationalEvent(OperationalLogLevel::kInfo,
                          OperationalLogCode::kPresenceTransition,
                          static_cast<int32_t>(previous),
                          static_cast<int32_t>(state), now);
  lastTransitionReason = reason;
  lastTransitionMs = now;
  Serial.printf(
      "EVENT,state,%" PRIu64 ",%s,%s,pir,%d,pir_age,%" PRIu64
      ",sound,%d,sound_age,%" PRIu64 ",env,%.1f,noise,%.1f,threshold,%.1f,"
      "brightness,%u,%u,config,%" PRIu64 ",build,%s\n",
      now, stateName(state), transitionReasonWireName(reason), pirHigh ? 1 : 0,
      now >= lastPirMs ? now - lastPirMs : 0, soundActive ? 1 : 0,
      now >= lastSoundMs ? now - lastSoundMs : 0, micEnvelope, noiseFloor,
      soundThreshold, brightnessBefore, currentBrightness,
      activeConfig.revision, kM5goBuildId);
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
    microphoneHealth = SensorHealthStatus::kFault;
    return;
  }

  if (!otaLockFlashSensorGuard(500)) {
    recordOtaMicrophoneWindow(false, now);
    micRms = 0.0f;
    soundActive = false;
    microphoneHealth = SensorHealthStatus::kFault;
    return;
  }

  const uint32_t windowStartedUs = micros();
  uint32_t nextSampleUs = micros();
  size_t railSampleCount = 0;
  size_t repeatedSampleCount = 0;
  uint16_t rawMinimum = 4095;
  uint16_t rawMaximum = 0;
  uint16_t previousRaw = 0;
  for (size_t i = 0; i < kMicSamples; ++i) {
    while (static_cast<int32_t>(micros() - nextSampleUs) < 0) {
      delayMicroseconds(8);
    }
    uint16_t raw = analogRead(static_cast<uint8_t>(kMicPin));
    rawMinimum = std::min(rawMinimum, raw);
    rawMaximum = std::max(rawMaximum, raw);
    if (raw == 0 || raw == 4095) {
      ++railSampleCount;
    }
    if (i != 0 && raw == previousRaw) {
      ++repeatedSampleCount;
    }
    previousRaw = raw;
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
  const uint32_t windowElapsedUs = micros() - windowStartedUs;
  otaUnlockFlashSensorGuard();

  const MicrophoneWindowHealthInput healthInput = {
      true,
      kMicSamples,
      railSampleCount,
      repeatedSampleCount,
      rawMinimum,
      rawMaximum,
      windowElapsedUs,
      static_cast<uint32_t>(kMicSamples * kMicSamplePeriodUs),
  };
  const SensorHealthStatus windowHealth =
      classifyMicrophoneWindow(healthInput);
  const uint32_t expectedWindowUs =
      static_cast<uint32_t>(kMicSamples * kMicSamplePeriodUs);
  const bool microphoneTimingValid =
      windowElapsedUs <= expectedWindowUs + expectedWindowUs / 4U &&
      windowElapsedUs >= expectedWindowUs - expectedWindowUs / 4U;
  recordOtaMicrophoneWindow(microphoneTimingValid, now);
  microphoneHealth = microphoneHealthLatch.observe(windowHealth);

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

  if (state == PresenceState::kCalibrating ||
      now < micRecalibrationUntilMs) {
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
    // An invalid timing/electrical window cannot extend presence. A latched
    // hard microphone fault enters the documented PIR-only degraded mode.
    if (windowHealth != SensorHealthStatus::kHealthy ||
        microphoneHealth == SensorHealthStatus::kFault) {
      soundActive = false;
    }
    if (soundActive && state != PresenceState::kIdle &&
        now - lastPirMs < activeConfig.maxSoundBridgeMs) {
      lastSoundMs = now;
    }
  }
}

void publishMainControlResult(MainControlResultCode code) {
  if (!mainControlRequestActive) {
    return;
  }
  MainControlResult result = {};
  std::snprintf(result.commandId, sizeof(result.commandId), "%s",
                activeMainControlRequest.commandId);
  result.code = code;
  result.requestVersion = activeMainControlRequest.version;
  if (controlMailbox.publishResult(result)) {
    mainControlRequestActive = false;
    remoteOtaConfirmationPending = false;
    remoteOtaConfirmationDeadlineMs = 0;
    activeMainControlRequest = {};
  }
}

void processMainControl(uint64_t now) {
  if (remoteOtaConfirmationPending &&
      now >= remoteOtaConfirmationDeadlineMs) {
    publishMainControlResult(MainControlResultCode::kExpired);
    displayDirty = true;
  }
  if (mainControlRequestActive) {
    return;
  }

  MainControlRequest request = {};
  if (!controlMailbox.takeRequest(&request)) {
    return;
  }
  activeMainControlRequest = request;
  mainControlRequestActive = true;
  enqueueOperationalEvent(OperationalLogLevel::kInfo,
                          OperationalLogCode::kCommandChanged,
                          static_cast<int32_t>(request.action), 1, now);
  switch (request.action) {
    case RemoteCommandAction::kRecalibrateMicrophone:
      micEnvelopeInitialized = false;
      micRecalibrationUntilMs = now + kMicrophoneRecalibrationMs;
      microphoneHealthLatch = SensorHealthLatch();
      microphoneHealth = SensorHealthStatus::kUnknown;
      publishMainControlResult(MainControlResultCode::kSucceeded);
      return;
    case RemoteCommandAction::kOpenDevOta:
      if (!request.requiresLocalConfirmation) {
        publishMainControlResult(MainControlResultCode::kRejected);
        return;
      }
      remoteOtaConfirmationPending = true;
      remoteOtaConfirmationDeadlineMs =
          now + kRemoteOtaConfirmationTimeoutMs;
      if (!diagnosticsVisible) {
        enterDiagnostics(now);
      }
      displayDirty = true;
      return;
    case RemoteCommandAction::kDiagnosticSnapshot:
    case RemoteCommandAction::kSetLogLevel:
    case RemoteCommandAction::kRetryUpload:
    case RemoteCommandAction::kReboot:
      // These are worker-owned. Reject rather than accidentally performing a
      // duplicate side effect if a future worker publishes one here.
      publishMainControlResult(MainControlResultCode::kRejected);
      return;
  }
}

bool pollDiagnosticsOtaGesture(uint64_t now) {
  if (!diagnosticsVisible) {
    diagnosticsOtaGestureStartedMs = 0;
    diagnosticsOtaGestureTriggered = false;
    return false;
  }
  const bool bothPressed = digitalRead(GPIO_NUM_39) == LOW &&
                           digitalRead(GPIO_NUM_37) == LOW;
  if (!bothPressed) {
    diagnosticsOtaGestureStartedMs = 0;
    diagnosticsOtaGestureTriggered = false;
    return false;
  }
  lastDiagnosticsActivityMs = now;
  if (diagnosticsOtaGestureStartedMs == 0) {
    diagnosticsOtaGestureStartedMs = now;
    return false;
  }
  if (diagnosticsOtaGestureTriggered ||
      now - diagnosticsOtaGestureStartedMs < kDiagnosticsOtaGestureMs) {
    return false;
  }
  diagnosticsOtaGestureTriggered = true;
  const bool accepted =
      otaRuntimeMailbox.requestPhysicallyConfirmedDevelopmentOpen();
  enqueueOperationalEvent(
      accepted ? OperationalLogLevel::kInfo : OperationalLogLevel::kWarning,
      OperationalLogCode::kOtaChanged, accepted ? 1 : -1, 0, now);
  if (remoteOtaConfirmationPending) {
    publishMainControlResult(accepted ? MainControlResultCode::kSucceeded
                                      : MainControlResultCode::kFailed);
  }
  displayDirty = true;
  return accepted;
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

void enterDiagnostics(uint64_t now) {
  diagnosticsVisible = true;
  diagnosticsOtaGestureStartedMs = 0;
  diagnosticsOtaGestureTriggered = false;
  lastDiagnosticsActivityMs = now;
  lastDiagnosticsRenderMs = 0;
  ensureDisplayAwake();
  displayDirty = true;
  enqueueOperationalEvent(OperationalLogLevel::kInfo,
                          OperationalLogCode::kDebugSessionChanged, 1, 0,
                          now);
  Serial.printf("EVENT,diagnostics,opened,%" PRIu64 "\n", now);
}

void exitDiagnostics(uint64_t now, const char* reason) {
  if (!diagnosticsVisible) {
    return;
  }
  diagnosticsVisible = false;
  displayDirty = true;
  restoreStateBrightness();
  enqueueOperationalEvent(OperationalLogLevel::kInfo,
                          OperationalLogCode::kDebugSessionChanged, 0, 0,
                          now);
  Serial.printf("EVENT,diagnostics,closed,%" PRIu64 ",%s\n", now, reason);
}

bool readUserInput(uint64_t now) {
  if (diagnosticsVisible) {
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() ||
        M5.BtnC.wasPressed()) {
      lastDiagnosticsActivityMs = now;
    }
    if (M5.BtnA.wasPressed()) {
      Serial.printf("EVENT,button,A,diagnostics_suppressed,%" PRIu64 "\n",
                    now);
    }
    if (M5.BtnC.wasPressed()) {
      Serial.printf("EVENT,button,C,diagnostics_suppressed,%" PRIu64 "\n",
                    now);
    }
    pollDiagnosticsOtaGesture(now);
    if (M5.BtnB.wasClicked()) {
      if (remoteOtaConfirmationPending) {
        publishMainControlResult(MainControlResultCode::kRejected);
      }
      exitDiagnostics(now, "button");
    } else if (!healthSnapshot.otaActive &&
               !remoteOtaConfirmationPending &&
               now - lastDiagnosticsActivityMs >= kDiagnosticsTimeoutMs) {
      exitDiagnostics(now, "timeout");
    }
    // Diagnostic controls never create feedback or mutate presence. The
    // automatic state machine continues to run from live sensor evidence.
    return false;
  }

  if (M5.BtnB.wasHold()) {
    enterDiagnostics(now);
    return false;
  }

  // A/C labels remain press-edge actions. B waits for the click decision so a
  // 1.5-second hold cannot first masquerade as an ordinary wake.
  const bool chosePresent = M5.BtnA.wasPressed();
  const bool choseWake = M5.BtnB.wasClicked();
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
  canvas.drawString(stateName(state), 294, 6);
  uint16_t healthColor = TFT_DARKGREY;
  switch (healthSnapshot.level) {
    case DeviceHealthLevel::kHealthy:
      healthColor = TFT_GREEN;
      break;
    case DeviceHealthLevel::kDegraded:
      healthColor = TFT_YELLOW;
      break;
    case DeviceHealthLevel::kActionRequired:
      healthColor = TFT_RED;
      break;
    case DeviceHealthLevel::kUnknown:
      healthColor = TFT_DARKGREY;
      break;
  }
  canvas.fillCircle(306, 9, 4, healthColor);

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

void formatDiagnosticAge(uint64_t ageMs, char* output, size_t capacity) {
  if (output == nullptr || capacity == 0) {
    return;
  }
  if (ageMs == UINT64_MAX) {
    std::snprintf(output, capacity, "never");
  } else if (ageMs < 1000) {
    std::snprintf(output, capacity, "now");
  } else if (ageMs < 60 * 1000) {
    std::snprintf(output, capacity, "%llus",
                  static_cast<unsigned long long>(ageMs / 1000));
  } else {
    std::snprintf(output, capacity, "%llum",
                  static_cast<unsigned long long>(ageMs / 60000));
  }
}

template <typename Canvas>
void drawDiagnostics(Canvas& canvas, uint64_t now) {
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextDatum(top_left);
  canvas.setTextSize(2);
  canvas.setTextColor(TFT_CYAN, TFT_BLACK);
  canvas.drawString("DEVICE DIAGNOSTICS", 8, 5);
  canvas.setTextSize(1);
  canvas.setTextColor(
      healthSnapshot.level == DeviceHealthLevel::kHealthy
          ? TFT_GREEN
          : healthSnapshot.level == DeviceHealthLevel::kActionRequired
                ? TFT_RED
                : TFT_YELLOW,
      TFT_BLACK);
  canvas.drawString(deviceHealthLevelWireName(healthSnapshot.level), 238, 8);
  canvas.drawFastHLine(8, 27, 304, TFT_DARKGREY);

  char line[96] = {};
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  std::snprintf(line, sizeof(line), "%s  build %.18s",
                healthSnapshot.firmwareVersion, healthSnapshot.buildId);
  canvas.drawString(line, 8, 34);
  std::snprintf(line, sizeof(line), "IP %-15s RSSI %ld dBm",
                healthSnapshot.wifiConnected ? healthSnapshot.localIp
                                             : "disconnected",
                static_cast<long>(healthSnapshot.wifiRssiDbm));
  canvas.drawString(line, 8, 49);
  std::snprintf(line, sizeof(line), "UP %llum  RESET %s  BOOT %u",
                static_cast<unsigned long long>(now / 60000),
                healthSnapshot.resetReason, healthSnapshot.bootCount);
  canvas.drawString(line, 8, 64);
  std::snprintf(line, sizeof(line), "STATE %s  WHY %s (%llus)",
                presenceStateWireName(state),
                transitionReasonWireName(lastTransitionReason),
                static_cast<unsigned long long>(
                    now >= lastTransitionMs ? (now - lastTransitionMs) / 1000
                                            : 0));
  canvas.drawString(line, 8, 79);
  std::snprintf(line, sizeof(line), "CFG %llu  PIR %s  MIC %s%s",
                static_cast<unsigned long long>(activeConfig.revision),
                sensorHealthStatusWireName(healthSnapshot.pirStatus),
                sensorHealthStatusWireName(healthSnapshot.microphoneStatus),
                healthSnapshot.pirOnlyMode ? " (PIR ONLY)" : "");
  canvas.drawString(line, 8, 94);

  char ackAge[16] = {};
  char roomAge[16] = {};
  char weatherAge[16] = {};
  formatDiagnosticAge(healthSnapshot.lastTelemetryAckAgeMs, ackAge,
                      sizeof(ackAge));
  formatDiagnosticAge(healthSnapshot.lastRoomFetchAgeMs, roomAge,
                      sizeof(roomAge));
  formatDiagnosticAge(healthSnapshot.lastWeatherFetchAgeMs, weatherAge,
                      sizeof(weatherAge));
  std::snprintf(line, sizeof(line), "API %s/%s  ROOM %s  WX %s", ackAge,
                healthOperationResultWireName(
                    healthSnapshot.telemetryAckResult),
                roomAge, weatherAge);
  canvas.drawString(line, 8, 109);
  std::snprintf(line, sizeof(line), "RAM Q %u/%u FB %u/%u DROP %u/%u",
                healthSnapshot.telemetryQueueDepth,
                healthSnapshot.telemetryQueueCapacity,
                healthSnapshot.feedbackQueueDepth,
                healthSnapshot.feedbackQueueCapacity,
                healthSnapshot.telemetryDroppedSamples,
                healthSnapshot.telemetryDroppedCritical);
  canvas.drawString(line, 8, 124);
  std::snprintf(line, sizeof(line), "FLASH Q %u FB %u+%u DEAD %u  FS %s",
                healthSnapshot.spoolFiles,
                healthSnapshot.feedbackWaitFiles,
                healthSnapshot.feedbackReadyFiles, healthSnapshot.deadFiles,
                healthSnapshot.filesystemReady ? "ok" : "retry");
  canvas.drawString(line, 8, 139);
  const uint32_t fsFree =
      healthSnapshot.littlefsTotalBytes >= healthSnapshot.littlefsUsedBytes
          ? healthSnapshot.littlefsTotalBytes -
                healthSnapshot.littlefsUsedBytes
          : 0;
  std::snprintf(line, sizeof(line), "HEAP %uK min %uK  FS FREE %uK",
                healthSnapshot.freeHeapBytes / 1024,
                healthSnapshot.minimumFreeHeapBytes / 1024, fsFree / 1024);
  canvas.drawString(line, 8, 154);
  std::snprintf(line, sizeof(line), "WORKER %s  CLOCK %s  OTA %s",
                uploaderHealthStatusWireName(healthSnapshot.uploaderStatus),
                healthSnapshot.clockSynchronized ? "synced" : "waiting",
                healthSnapshot.otaState);
  canvas.drawString(line, 8, 169);
  if (remoteOtaConfirmationPending) {
    canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    canvas.drawString("OTA REQUEST: HOLD A+C FOR 3 SECONDS", 8, 187);
  } else if (otaRuntimeSnapshot.phase ==
             OtaRuntimePhase::kDevelopmentWindowOpen) {
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    std::snprintf(line, sizeof(line), "DEV OTA %s  %lus left",
                  otaRuntimeSnapshot.localIp,
                  static_cast<unsigned long>(
                      otaRuntimeSnapshot.remainingMs / 1000));
    canvas.drawString(line, 8, 187);
  } else if (otaRuntimeSnapshot.totalBytes != 0) {
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    const uint32_t percent = static_cast<uint32_t>(
        (static_cast<uint64_t>(otaRuntimeSnapshot.completedBytes) * 100U) /
        otaRuntimeSnapshot.totalBytes);
    std::snprintf(line, sizeof(line), "OTA %s %lu%% G%u M%u/%u",
                  otaRuntimePhaseName(otaRuntimeSnapshot.phase),
                  static_cast<unsigned long>(percent),
                  otaRuntimeSnapshot.maximumMainLoopGapMs,
                  otaRuntimeSnapshot.invalidMicrophoneWindows,
                  otaRuntimeSnapshot.totalMicrophoneWindows);
    canvas.drawString(line, 8, 187);
  } else if (healthSnapshot.safeMode) {
    canvas.setTextColor(TFT_RED, TFT_BLACK);
    canvas.drawString("SAFE MODE: repeated early resets", 8, 187);
  }

  canvas.drawRect(0, 207, 320, 33, TFT_DARKGREY);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString(remoteOtaConfirmationPending
                        ? "HOLD A+C 3s       B EXIT"
                        : "HOLD A+C: DEV OTA   B EXIT",
                    160, 223);
  canvas.setTextDatum(top_left);
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

void refreshDeviceHealth(uint64_t now) {
  static SensorHealthStatus reportedPirHealth = SensorHealthStatus::kUnknown;
  static SensorHealthStatus reportedMicrophoneHealth =
      SensorHealthStatus::kUnknown;
  if (pirHealth != reportedPirHealth) {
    Serial.printf("EVENT,sensor_health,pir,%" PRIu64 ",%s,%s\n", now,
                  sensorHealthStatusWireName(reportedPirHealth),
                  sensorHealthStatusWireName(pirHealth));
    enqueueOperationalEvent(
        pirHealth == SensorHealthStatus::kFault
            ? OperationalLogLevel::kWarning
            : OperationalLogLevel::kInfo,
        OperationalLogCode::kSensorChanged, 0,
        static_cast<int32_t>(pirHealth), now);
    reportedPirHealth = pirHealth;
  }
  if (microphoneHealth != reportedMicrophoneHealth) {
    Serial.printf("EVENT,sensor_health,microphone,%" PRIu64 ",%s,%s\n", now,
                  sensorHealthStatusWireName(reportedMicrophoneHealth),
                  sensorHealthStatusWireName(microphoneHealth));
    enqueueOperationalEvent(
        microphoneHealth == SensorHealthStatus::kFault
            ? OperationalLogLevel::kWarning
            : OperationalLogLevel::kInfo,
        OperationalLogCode::kSensorChanged, 1,
        static_cast<int32_t>(microphoneHealth), now);
    reportedMicrophoneHealth = microphoneHealth;
  }
  if (lastHealthRefreshMs == 0 ||
      now - lastHealthRefreshMs >= kHealthRefreshMs) {
    lastHealthRefreshMs = now;
    DeviceHealthMainUpdate update = {};
    std::snprintf(update.deviceId, sizeof(update.deviceId), "%s",
                  runtimeIdentity.deviceId);
    std::snprintf(update.bootId, sizeof(update.bootId), "%s",
                  runtimeIdentity.bootId);
    std::snprintf(update.firmwareVersion, sizeof(update.firmwareVersion), "%s",
                  kM5goFirmwareVersion);
    std::snprintf(update.buildId, sizeof(update.buildId), "%s", kM5goBuildId);
    std::snprintf(update.resetReason, sizeof(update.resetReason), "%s",
                  resetReasonName);
    update.uptimeMs = now;
    update.appliedConfigRevision = activeConfig.revision;
    update.storedConfigRevision = activeConfig.revision;
    update.mainHeartbeatAgeMs = 0;
    update.bootCount = bootCount;
    update.mainStackHighWaterBytes =
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
        sizeof(StackType_t);
    update.telemetryQueueDepth = telemetryQueue.size();
    update.telemetryQueueCapacity = telemetryQueue.capacity();
    update.telemetryDroppedSamples = telemetryQueue.droppedSamples();
    update.telemetryDroppedCritical = telemetryQueue.droppedCritical();
    update.feedbackQueueDepth = touchFeedbackQueue.size();
    update.feedbackQueueCapacity = touchFeedbackQueue.capacity();
    update.feedbackDroppedFull = touchFeedbackQueue.droppedFull();
    update.feedbackRejectedInvalid = touchFeedbackQueue.rejectedInvalid();
    update.freeHeapBytes = ESP.getFreeHeap();
    update.minimumFreeHeapBytes = ESP.getMinFreeHeap();
    update.largestFreeBlockBytes = ESP.getMaxAllocHeap();
    update.pirStatus = pirHealth;
    update.microphoneStatus = microphoneHealth;
    update.initialized = true;
    update.pirOnlyMode = microphoneHealth == SensorHealthStatus::kFault;
    update.safeMode = safeMode;
    healthMailbox.publishMain(update);
  }

  DeviceHealthSnapshot latest = {};
  if (!healthMailbox.copySnapshot(&latest) ||
      latest.version == healthSnapshot.version) {
    if (diagnosticsVisible &&
        now - lastDiagnosticsRenderMs >= kHealthRefreshMs) {
      lastDiagnosticsRenderMs = now;
      displayDirty = true;
    }
    return;
  }
  const bool levelChanged = latest.level != healthSnapshot.level;
  healthSnapshot = latest;
  if (levelChanged) {
    displayHeaderDirty = true;
    Serial.printf("EVENT,health,level,%" PRIu64 ",%s\n", now,
                  deviceHealthLevelWireName(healthSnapshot.level));
    enqueueOperationalEvent(
        healthSnapshot.level == DeviceHealthLevel::kActionRequired
            ? OperationalLogLevel::kError
            : healthSnapshot.level == DeviceHealthLevel::kDegraded
                  ? OperationalLogLevel::kWarning
                  : OperationalLogLevel::kInfo,
        OperationalLogCode::kHealthChanged,
        static_cast<int32_t>(healthSnapshot.level), 0, now);
  }
  if (diagnosticsVisible &&
      now - lastDiagnosticsRenderMs >= kHealthRefreshMs) {
    lastDiagnosticsRenderMs = now;
    displayDirty = true;
  }
}

void refreshOtaRuntime(uint64_t now) {
  OtaRuntimeSnapshot latest = {};
  if (!otaRuntimeMailbox.copySnapshot(&latest) ||
      latest.version == otaRuntimeSnapshot.version) {
    return;
  }
  const OtaRuntimePhase previous = otaRuntimeSnapshot.phase;
  otaRuntimeSnapshot = latest;
  if (previous != otaRuntimeSnapshot.phase) {
    enqueueOperationalEvent(
        otaRuntimeSnapshot.phase == OtaRuntimePhase::kFailed
            ? OperationalLogLevel::kError
            : OperationalLogLevel::kInfo,
        OperationalLogCode::kOtaChanged,
        static_cast<int32_t>(otaRuntimeSnapshot.phase),
        static_cast<int32_t>(otaRuntimeSnapshot.error), now);
  }
  if (diagnosticsVisible) {
    displayDirty = true;
  }
}

void pollOtaBootValidation(uint64_t now) {
  if (otaBootValidator.phase() == OtaBootValidationPhase::kNotStarted ||
      otaBootNoticePublished) {
    return;
  }
  OtaBootHealthGates gates = {};
  gates.mainLoopHealthy = true;
  gates.uploaderTaskHealthy =
      healthSnapshot.uploaderStatus != UploaderHealthStatus::kStarting &&
      healthSnapshot.uploaderStatus != UploaderHealthStatus::kTaskUnavailable;
  gates.settingsStorageHealthy =
      configStorageHealthy && settingsStorageHealthy;
  gates.filesystemHealthy = healthSnapshot.filesystemReady;
  gates.displayHealthy = displayInitializationHealthy;
  gates.otaInstallStateReady = otaRuntimeSnapshot.installStateKnown &&
                               otaRuntimeSnapshot.installStateHealthy;
  const bool hardFailure = !gates.settingsStorageHealthy ||
                           !gates.displayHealthy ||
                           (otaRuntimeSnapshot.installStateKnown &&
                            !otaRuntimeSnapshot.installStateHealthy) ||
                           healthSnapshot.uploaderStatus ==
                               UploaderHealthStatus::kTaskUnavailable;
  otaBootValidator.poll(now, gates, hardFailure);
  OtaBootValidationPhase phase = otaBootValidator.phase();
  if (phase == OtaBootValidationPhase::kAwaitingPersistence &&
      otaRuntimeSnapshot.installStateKnown &&
      otaRuntimeSnapshot.installStateHealthy) {
    if (otaRuntimeSnapshot.confirmationPrepared) {
      otaBootValidator.confirmAfterPersistence();
    } else {
      // Idempotent until consumed. The worker persists and verifies the
      // prepared bit before publishing confirmationPrepared back to main.
      otaRuntimeMailbox.publishBootValidationNotice(
          OtaBootValidationNotice::kPrepareConfirmation);
    }
    phase = otaBootValidator.phase();
  }
  if (phase == OtaBootValidationPhase::kConfirmed) {
    if (otaRuntimeMailbox.publishBootValidationNotice(
            OtaBootValidationNotice::kConfirmed)) {
      otaBootNoticePublished = true;
      enqueueOperationalEvent(OperationalLogLevel::kInfo,
                              OperationalLogCode::kOtaChanged, 1, 0, now);
    }
  } else if (phase == OtaBootValidationPhase::kRollbackRequested ||
             phase == OtaBootValidationPhase::kPlatformError) {
    if (otaRuntimeMailbox.publishBootValidationNotice(
            OtaBootValidationNotice::kFailed)) {
      otaBootNoticePublished = true;
      enqueueOperationalEvent(
          OperationalLogLevel::kError, OperationalLogCode::kOtaChanged, -1,
          static_cast<int32_t>(otaBootValidator.error()), now);
    }
  }
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
  } else if (diagnosticsVisible) {
    setBrightness(kOnBrightness);
  } else if (provisioningBrightnessOverride) {
    provisioningBrightnessOverride = false;
    restoreStateBrightness();
  }
  const bool heartbeatDue = now - lastFullDisplayMs >= kDisplayHeartbeatMs;
  const bool fullRedraw = displayDirty || heartbeatDue ||
                          (diagnosticsVisible && displayHeaderDirty);
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
    if (diagnosticsVisible) {
      drawDiagnostics(M5.Display, now);
    } else {
      drawDashboard(M5.Display, now);
    }
    if (provisioningChallengeActive) {
      drawProvisioningOverlay(M5.Display);
    }
    displayDirty = false;
    displayHeaderDirty = false;
    return;
  }

  if (diagnosticsVisible) {
    drawDiagnostics(displayFrame, now);
  } else {
    drawDashboard(displayFrame, now);
  }

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

void enqueueDetailedSensorLog(uint64_t now) {
  if (!healthSnapshot.debugActive || now - lastDetailedLogMs < 250) {
    return;
  }
  lastDetailedLogMs = now;
  const int32_t flags = (pirHigh ? 1 : 0) | (soundActive ? 2 : 0);
  enqueueOperationalEvent(
      OperationalLogLevel::kSensorDetail, OperationalLogCode::kSensorChanged,
      static_cast<int32_t>(std::lround(micEnvelope)), flags, now);
}

}  // namespace

void setup() {
  // NVS is initialized by the Arduino core before setup(). Rotate the boot
  // identity before board/display/network initialization so any prior flash
  // core dump is attributed to the firmware instance that actually crashed.
  runtimeIdentity = createRuntimeIdentity();
  crashContextRotation =
      captureAndRotateCrashContext(runtimeIdentity.bootId, kM5goBuildId);
  initializeCrashDumpAttribution();

  auto config = M5.config();
  config.serial_baudrate = 115200;
  config.clear_display = true;
  config.output_power = true;
  config.fallback_board = m5::board_t::board_M5Stack;
  config.internal_mic = false;
  config.internal_spk = false;
  M5.begin(config);
  M5.BtnB.setHoldThresh(kDiagnosticsHoldMs);
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
  displayInitializationHealthy =
      M5.Display.width() == 320 && M5.Display.height() == 240;

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
  micBeginOk = otaInitializeFlashSensorGuard() && beginAnalogMicrophone();

  initializeBootDiagnostics();

  PresenceConfig storedConfig = defaultPresenceConfig();
  const DeviceConfigStorageResult configStorageResult =
      loadStoredDeviceConfig(&storedConfig);
  if (configStorageResult == DeviceConfigStorageResult::kOk) {
    activeConfig = storedConfig;
  } else {
    activeConfig = defaultPresenceConfig();
  }
  configStorageHealthy =
      configStorageResult == DeviceConfigStorageResult::kOk ||
      configStorageResult == DeviceConfigStorageResult::kNotStored;
  configMailbox.acknowledgeAppliedRevision(activeConfig.revision);

  // NVS access is outside the sensor calibration window. The monotonic boot
  // baseline begins only after the configuration for this run is known.
  bootMs = monotonicMillis();
  stateSinceMs = bootMs;
  lastPirMs = bootMs;
  lastSoundMs = bootMs;
  lastTelemetrySampleMs = bootMs;
  enqueueOperationalEvent(
      safeMode ? OperationalLogLevel::kError : OperationalLogLevel::kInfo,
      OperationalLogCode::kBoot, static_cast<int32_t>(bootCount),
      static_cast<int32_t>(esp_reset_reason()), bootMs);
  otaBootValidatorStarted =
      otaBootValidator.begin(bootMs, otaEsp32BootValidationBackend());

  // The first telemetry record must describe the configuration that was
  // actually loaded for this boot, never an earlier compile-time placeholder.
  enqueueTransition(false, PresenceState::kCalibrating,
                    PresenceState::kCalibrating, TransitionReason::kBoot,
                    bootMs, currentBrightness);
  lastTransitionMs = bootMs;
  lastTransitionReason = TransitionReason::kBoot;
  pirHealth = pirHealthTracker.observe(false, bootMs);
  if (!micBeginOk) {
    microphoneHealth = SensorHealthStatus::kFault;
  }
  refreshDeviceHealth(bootMs);

  Serial.printf("M5GO Presence Lab v%s build %s\n", kM5goFirmwareVersion,
                kM5goBuildId);
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
  settingsStorageHealthy =
      settingsResult == DeviceSettingsStorageResult::kOk ||
      settingsResult == DeviceSettingsStorageResult::kNotConfigured;
  deviceSettingsConfigured = settingsResult == DeviceSettingsStorageResult::kOk;
  TelemetryUploaderSettings uploaderSettings;
  uploaderSettings.configured = deviceSettingsConfigured;
  uploaderSettings.initialConfig = activeConfig;
  uploaderSettings.startAfterUptimeMs = bootMs + kCalibrationMs + 1000;
  uploaderSettings.bootValidationStateKnown = otaBootValidatorStarted;
  uploaderSettings.bootImageInfo = otaBootValidator.runningImageInfo();
  uploaderSettings.bootValidationStateKnown =
      uploaderSettings.bootImageInfo.valid();
  uploaderSettings.bootValidationPending =
      uploaderSettings.bootImageInfo.state ==
      OtaRunningImageState::kPendingVerify;
  uploaderSettings.coreDumpAttributionAvailable =
      crashDumpAttributionAvailable;
  if (crashDumpAttributionAvailable) {
    uploaderSettings.coreDumpAttribution = activeCrashDumpAttribution;
  }
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
    std::memcpy(uploaderSettings.otaDevelopmentSecret,
                loadedSettings.otaSecret,
                sizeof(loadedSettings.otaSecret));
  }
  // After repeated early abnormal resets, keep the local sensor/display loop
  // alive but omit the network worker that may have caused the crash cycle.
  // One stable minute clears the latch for the next boot; USB remains the
  // explicit recovery path while the diagnostics page shows SAFE MODE.
  const bool uploaderStarted =
      !safeMode &&
      startTelemetryUploader(runtimeIdentity, uploaderSettings, telemetryQueue,
                             configMailbox, touchFeedbackQueue,
                             dashboardMailbox, healthMailbox, controlMailbox,
                             operationalLogRing, otaRuntimeMailbox);
  if (safeMode) {
    Serial.println("EVENT,safe_mode,network_worker_suppressed");
  }
  if (!uploaderStarted) {
    DeviceHealthWorkerUpdate failedWorker = {};
    failedWorker.uptimeMs = bootMs;
    failedWorker.uploaderHeartbeatAgeMs = 0;
    failedWorker.uploaderStatus = UploaderHealthStatus::kTaskUnavailable;
    healthMailbox.publishWorker(failedWorker);
  }
  securelyClear(&loadedSettings, sizeof(loadedSettings));
  securelyClear(&uploaderSettings, sizeof(uploaderSettings));
  Serial.printf("NETWORK,configured,%d,uploader_started,%d,settings_status,%u\n",
                deviceSettingsConfigured ? 1 : 0, uploaderStarted ? 1 : 0,
                static_cast<unsigned>(settingsResult));
  Serial.printf("CRASH_CONTEXT,rotation,%u,pin,%u,available,%d\n",
                static_cast<unsigned>(crashContextRotation.storage),
                static_cast<unsigned>(crashAttributionStorageResult),
                crashDumpAttributionAvailable ? 1 : 0);
  enableLoopWDT();
}

void loop() {
  const uint64_t now = monotonicMillis();
  applyPendingConfig(now);
  refreshDashboardSnapshot();
  refreshDashboardClock();
  M5.update();
  refreshOtaRuntime(now);
  updateOtaLoopSafety(now);
  processMainControl(now);
  logButtonPinChanges(now);

  pirHigh = digitalRead(kPirPin) == HIGH;
  pirHealth = pirHealthTracker.observe(pirHigh, now);
  sampleMicrophone(now);
  enqueueDetailedSensorLog(now);
  const bool userInputChangedState = readUserInput(now);
  if (!userInputChangedState) {
    updatePresenceState(now);
  }
  refreshDeviceHealth(now);
  pollOtaBootValidation(now);
  drawDisplay(now);
  printSerial(now);
  enqueueSample(now);
  pollProvisioningSerial(now);
  markBootStableIfNeeded(now);
  publishOtaSafetyMetrics();

  if (provisioningRestartMs != 0 && now >= provisioningRestartMs) {
    Serial.flush();
    ESP.restart();
  }

  delay(1);
}
