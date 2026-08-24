#include <Arduino.h>
#include <M5Unified.h>
#include <algorithm>
#include <cmath>
#include <driver/i2s.h>

namespace {

// M5Stack Core2 PORT.B: white wire / digital input is GPIO36.
constexpr gpio_num_t kPirPin = GPIO_NUM_36;

constexpr uint32_t kCalibrationMs = 5000;
constexpr uint32_t kBenchTestMs = 5 * 60 * 1000;
constexpr uint32_t kMinimumOnMs = 10000;
constexpr uint32_t kPirHoldMs = 30000;
constexpr uint32_t kSoundHoldMs = 12000;
constexpr uint32_t kMaxSoundBridgeMs = 5 * 60 * 1000;
constexpr uint32_t kCooldownMs = 5000;
constexpr uint32_t kTouchWakeMs = 15000;
constexpr uint32_t kDisplayIntervalMs = 150;
constexpr uint32_t kSerialIntervalMs = 250;

// Match M5Stack's original SPM1423 capture layout: 44.1 kHz and a 512-byte
// read (256 signed 16-bit samples). The legacy ESP32 PDM peripheral is
// sensitive to its channel/DMA layout, so these values intentionally mirror
// the vendor example.
constexpr size_t kMicSamples = 256;
constexpr uint32_t kMicSampleRate = 44100;
constexpr uint8_t kOnBrightness = 255;
constexpr uint8_t kCooldownBrightness = 60;

enum class PresenceState : uint8_t {
  kCalibrating,
  kIdle,
  kPresent,
  kCooldown,
};

int16_t micSamples[kMicSamples] = {};
M5Canvas displayFrame(&M5.Display);

PresenceState state = PresenceState::kCalibrating;
uint32_t bootMs = 0;
uint32_t stateSinceMs = 0;
uint32_t lastPirMs = 0;
uint32_t lastSoundMs = 0;
uint32_t lastDisplayMs = 0;
uint32_t lastSerialMs = 0;

float micRms = 0.0f;
float micEnvelope = 0.0f;
int16_t micMin = 0;
int16_t micMax = 0;
float noiseFloor = 100.0f;
float soundFactor = 1.12f;
float soundThreshold = 300.0f;
bool pirHigh = false;
bool previousPirHigh = false;
bool soundActive = false;
bool tmosDetected = false;
bool baseImuDetected = false;
bool micBeginOk = false;
bool micEnvelopeInitialized = false;
bool displayFrameReady = false;
bool fallbackUiInitialized = false;
uint8_t currentBrightness = 255;

const char* stateName(PresenceState value) {
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

void forceCore2DisplayPower() {
  // Core2 v1.1 uses the AXP2101: ALDO4 powers the LCD/touch rail, ALDO2
  // releases their reset rail, and BLDO1 drives the backlight. M5GFX normally
  // configures all three, but make the diagnostic firmware explicit so a
  // PMIC/autodetection mismatch cannot leave a healthy LCD looking dead.
  if (M5.Power.getType() == m5::Power_Class::pmic_t::pmic_axp2101) {
    M5.Power.Axp2101.setALDO4(3300);
    M5.Power.Axp2101.setALDO2(3300);
    M5.Power.Axp2101.setBLDO1(3300);
  }

  M5.Display.wakeup();
  M5.Display.powerSaveOff();
  currentBrightness = 254;
  setBrightness(255);
}

void enterState(PresenceState next, uint32_t now) {
  if (next == state) {
    return;
  }

  state = next;
  stateSinceMs = now;

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

  Serial.printf("EVENT,state,%lu,%s\n", now, stateName(state));
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

bool beginRawMicrophone() {
  // Bypass M5Unified's asynchronous recorder for this hardware check. These
  // are the Core2/SPM1423 PDM pins from M5Stack's official schematic and
  // original I2S examples.
  i2s_driver_uninstall(I2S_NUM_0);

  i2s_config_t config = {};
  config.mode =
      static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  config.sample_rate = kMicSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ALL_RIGHT;
  config.communication_format =
      static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_STAND_I2S);
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 2;
  config.dma_buf_len = 128;
  config.use_apll = false;

  esp_err_t result = i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);
  if (result != ESP_OK) {
    Serial.printf("EVENT,i2s_install_error,%d\n", result);
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_PIN_NO_CHANGE;
  pins.ws_io_num = GPIO_NUM_0;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = GPIO_NUM_34;
  result = i2s_set_pin(I2S_NUM_0, &pins);
  if (result != ESP_OK) {
    Serial.printf("EVENT,i2s_pin_error,%d\n", result);
    return false;
  }

  result = i2s_set_clk(I2S_NUM_0, kMicSampleRate,
                       I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  if (result != ESP_OK) {
    Serial.printf("EVENT,i2s_clock_error,%d\n", result);
    return false;
  }
  return true;
}

void sampleMicrophone(uint32_t now) {
  if (!micBeginOk) {
    micRms = 0.0f;
    soundActive = false;
    return;
  }

  size_t bytesRead = 0;
  const esp_err_t result =
      i2s_read(I2S_NUM_0, micSamples, sizeof(micSamples), &bytesRead,
               pdMS_TO_TICKS(50));
  if (result != ESP_OK || bytesRead < sizeof(micSamples)) {
    return;
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
    // Fast enough to catch speech, but slow enough to reject the legacy
    // ESP32 PDM decoder's occasional one-block peaks. Release is deliberately
    // slower so gaps between syllables still count as one sound event.
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

    soundThreshold = fmaxf(noiseFloor * soundFactor, noiseFloor + 350.0f);
    const float releaseThreshold =
        fmaxf(noiseFloor * (1.0f + (soundFactor - 1.0f) * 0.55f),
              noiseFloor + 175.0f);
    soundActive = soundActive ? micEnvelope > releaseThreshold
                              : micEnvelope > soundThreshold;
    if (soundActive && state != PresenceState::kIdle &&
        now - lastPirMs < kMaxSoundBridgeMs) {
      lastSoundMs = now;
    }
  }
}

void readTouch(uint32_t now) {
  const auto touch = M5.Touch.getDetail();
  if (!touch.wasClicked()) {
    return;
  }

  // Bottom left/right tunes microphone sensitivity. A touch elsewhere wakes
  // the screen so diagnostics remain accessible after IDLE turns it off.
  if (touch.y > 190 && touch.x < 105) {
    soundFactor = fmaxf(1.05f, soundFactor - 0.02f);
    Serial.printf("EVENT,sensitivity,%lu,%.2f\n", now, soundFactor);
  } else if (touch.y > 190 && touch.x > 215) {
    soundFactor = fminf(2.0f, soundFactor + 0.02f);
    Serial.printf("EVENT,sensitivity,%lu,%.2f\n", now, soundFactor);
  }

  lastPirMs = now;
  lastSoundMs = now;
  enterState(PresenceState::kPresent, now);
}

void updatePresenceState(uint32_t now) {
  const bool pirRising = pirHigh && !previousPirHigh;
  if (pirHigh) {
    lastPirMs = now;
  }

  // Give the person installing the unit a visible five-minute window after
  // every reboot. Sensor values remain live, but the backlight is held on so
  // a stationary tester is not mistaken for an empty room mid-calibration.
  if (now - bootMs >= kCalibrationMs && now - bootMs < kBenchTestMs) {
    lastPirMs = now;
    lastSoundMs = now;
    enterState(PresenceState::kPresent, now);
    previousPirHigh = pirHigh;
    return;
  }

  switch (state) {
    case PresenceState::kCalibrating:
      if (now - bootMs >= kCalibrationMs) {
        if (pirHigh) {
          lastPirMs = now;
          lastSoundMs = now;
          enterState(PresenceState::kPresent, now);
        } else {
          enterState(PresenceState::kIdle, now);
        }
      }
      break;

    case PresenceState::kIdle:
      // Sound alone does not wake the screen: televisions and fans should not
      // create a false arrival. PIR is the authoritative wake signal.
      if (pirRising || pirHigh) {
        lastPirMs = now;
        lastSoundMs = now;
        enterState(PresenceState::kPresent, now);
      }
      break;

    case PresenceState::kPresent: {
      const bool minimumOnElapsed = now - stateSinceMs >= kMinimumOnMs;
      const bool pirQuiet = now - lastPirMs >= kPirHoldMs;
      const bool soundQuiet = now - lastSoundMs >= kSoundHoldMs;
      if (minimumOnElapsed && pirQuiet && soundQuiet) {
        enterState(PresenceState::kCooldown, now);
      }
      break;
    }

    case PresenceState::kCooldown:
      const bool soundCanBridge =
          soundActive && now - lastPirMs < kMaxSoundBridgeMs;
      if (pirHigh || soundCanBridge) {
        if (pirHigh) {
          lastPirMs = now;
        }
        if (soundCanBridge) {
          lastSoundMs = now;
        }
        enterState(PresenceState::kPresent, now);
      } else if (now - stateSinceMs >= kCooldownMs) {
        enterState(PresenceState::kIdle, now);
      }
      break;
  }

  previousPirHigh = pirHigh;
}

void drawBar(M5Canvas& canvas, int x, int y, int width, int height,
             float fraction, uint16_t color) {
  fraction = fmaxf(0.0f, fminf(1.0f, fraction));
  canvas.drawRect(x, y, width, height, TFT_DARKGREY);
  canvas.fillRect(x + 2, y + 2,
                  static_cast<int>((width - 4) * fraction), height - 4,
                  color);
  canvas.fillRect(x + 2 + static_cast<int>((width - 4) * fraction), y + 2,
                  (width - 4) - static_cast<int>((width - 4) * fraction),
                  height - 4, TFT_BLACK);
}

void drawDisplayFallback(uint32_t now) {
  if (!fallbackUiInitialized) {
    forceCore2DisplayPower();
    M5.Display.fillScreen(TFT_YELLOW);
    M5.Display.setTextColor(TFT_BLACK, TFT_YELLOW);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Core2 Presence Lab", 10, 10);
    M5.Display.setTextSize(1);
    M5.Display.drawString("Low-memory partial update mode", 10, 38);
    fallbackUiInitialized = true;
  }

  // Only erase the small dynamic region, never the whole display.
  M5.Display.fillRect(8, 58, 304, 126, TFT_YELLOW);
  M5.Display.setTextColor(TFT_BLACK, TFT_YELLOW);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 64);
  M5.Display.printf("PIR: %s", pirHigh ? "MOTION" : "quiet ");
  M5.Display.setCursor(10, 94);
  M5.Display.printf("Mic: %.0f", micEnvelope);
  M5.Display.setCursor(10, 124);
  M5.Display.printf("Sound: %s", soundActive ? "YES" : "no ");
  M5.Display.setCursor(10, 154);
  const uint32_t elapsed = now - bootMs;
  M5.Display.printf("Test: %lus ",
                    elapsed < kBenchTestMs
                        ? (kBenchTestMs - elapsed) / 1000
                        : 0);
}

void drawDisplay(uint32_t now) {
  if (currentBrightness == 0 || now - lastDisplayMs < kDisplayIntervalMs) {
    return;
  }
  lastDisplayMs = now;

  if (!displayFrameReady) {
    drawDisplayFallback(now);
    return;
  }

  const uint16_t background =
      now - bootMs < kBenchTestMs ? TFT_YELLOW : TFT_BLACK;
  displayFrame.fillScreen(background);
  displayFrame.setTextDatum(top_left);

  displayFrame.setTextSize(2);
  displayFrame.setTextColor(TFT_CYAN, TFT_BLACK);
  displayFrame.drawString("Core2 Presence Lab", 10, 8);

  displayFrame.setTextColor(stateColor(state), TFT_BLACK);
  if (now - bootMs < kBenchTestMs) {
    const uint32_t secondsLeft = (kBenchTestMs - (now - bootMs)) / 1000;
    char testLabel[32];
    snprintf(testLabel, sizeof(testLabel), "TEST MODE  %lu:%02lu",
             secondsLeft / 60, secondsLeft % 60);
    displayFrame.drawString(testLabel, 10, 36);
  } else {
    displayFrame.drawString(stateName(state), 10, 36);
  }

  displayFrame.setTextSize(2);
  displayFrame.setTextColor(TFT_WHITE, TFT_BLACK);
  displayFrame.setCursor(10, 68);
  displayFrame.printf("PIR G36: ");
  displayFrame.setTextColor(pirHigh ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  displayFrame.printf("%s", pirHigh ? "MOTION" : "quiet");

  displayFrame.setTextColor(TFT_WHITE, TFT_BLACK);
  displayFrame.setCursor(10, 96);
  displayFrame.printf("Mic level: %.0f", micEnvelope);
  drawBar(displayFrame, 10, 120, 300, 16,
          soundThreshold > 0.0f ? micEnvelope / (soundThreshold * 1.25f)
                                : 0.0f,
          soundActive ? TFT_MAGENTA : TFT_BLUE);

  displayFrame.setTextSize(1);
  displayFrame.setCursor(10, 142);
  displayFrame.setTextColor(soundActive ? TFT_MAGENTA : TFT_LIGHTGREY,
                            TFT_BLACK);
  displayFrame.printf("raw %.0f noise %.0f trigger %.0f", micRms, noiseFloor,
                      soundThreshold);

  displayFrame.setCursor(10, 164);
  displayFrame.setTextColor(tmosDetected ? TFT_ORANGE : TFT_DARKGREY,
                            TFT_BLACK);
  displayFrame.printf("TMOS:%s  base IMU:%s",
                      tmosDetected ? "FOUND" : "none",
                      baseImuDetected ? "FOUND" : "none");

  displayFrame.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  displayFrame.setCursor(10, 184);
  displayFrame.printf("Mic sensitivity factor: %.1fx", soundFactor);

  displayFrame.drawRect(0, 207, 105, 33, TFT_DARKGREY);
  displayFrame.drawRect(215, 207, 105, 33, TFT_DARKGREY);
  displayFrame.setTextDatum(middle_center);
  displayFrame.drawString("MORE SENSITIVE", 52, 223);
  displayFrame.drawString("LESS SENSITIVE", 267, 223);
  displayFrame.setTextDatum(top_left);

  M5.Display.startWrite();
  displayFrame.pushSprite(0, 0);
  M5.Display.endWrite();
}

void printSerial(uint32_t now) {
  if (now - lastSerialMs < kSerialIntervalMs) {
    return;
  }
  lastSerialMs = now;
  Serial.printf(
      "DATA,%lu,%d,%.1f,%.1f,%d,%d,%.1f,%.1f,%d,%s,%u\n", now,
      pirHigh ? 1 : 0, micRms, micEnvelope, micMin, micMax, noiseFloor,
      soundThreshold, soundActive ? 1 : 0, stateName(state), currentBrightness);
}

}  // namespace

void setup() {
  auto config = M5.config();
  config.serial_baudrate = 115200;
  config.clear_display = true;
  config.output_power = true;
  config.internal_mic = false;
  config.internal_spk = true;
  M5.begin(config);
  baseImuDetected = M5.Imu.isEnabled();

  Serial.begin(115200);
  delay(200);

  M5.Display.setRotation(1);
  M5.Display.setTextWrap(false);
  forceCore2DisplayPower();
  displayFrame.setPsram(true);
  displayFrame.setColorDepth(8);
  displayFrameReady =
      displayFrame.createSprite(M5.Display.width(), M5.Display.height());

  // Unmissable display/PMIC check before sensor initialization. The normal
  // diagnostics UI replaces this after three seconds.
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_RED, TFT_WHITE);
  M5.Display.setTextSize(3);
  M5.Display.drawString("DISPLAY POWER OK", M5.Display.width() / 2,
                        M5.Display.height() / 2);
  delay(3000);
  M5.Display.setTextDatum(top_left);

  pinMode(kPirPin, INPUT);
  // Do not create another TwoWire instance on I2C controller 1 here. Core2's
  // internal AXP2101 PMIC uses that controller on GPIO21/22; remapping it to
  // external Port A GPIO32/33 makes the LCD backlight control disappear.
  // The attached sensor is already verified as the digital GPIO36 PIR path.
  tmosDetected = false;

  // Core2 shares I2S0 between speaker and microphone. Stop the speaker before
  // installing the raw PDM receive driver.
  M5.Speaker.end();
  delay(50);
  forceCore2DisplayPower();
  micBeginOk = beginRawMicrophone();

  bootMs = millis();
  stateSinceMs = bootMs;
  lastPirMs = bootMs;
  lastSoundMs = bootMs;

  Serial.println("Core2 Presence Lab v0.1");
  Serial.printf(
      "DEVICE,pir_gpio,%d,tmos_0x5a,%d,mic_started,%d,mic_data,34,"
      "mic_clock,0,driver,raw_i2s_pdm,base_imu,%d,board,%d,pmic,%d,"
      "display,%dx%d,frame_buffer,%d,psram,%u\n",
                static_cast<int>(kPirPin), tmosDetected ? 1 : 0,
                micBeginOk ? 1 : 0, baseImuDetected ? 1 : 0,
                static_cast<int>(M5.getBoard()),
                static_cast<int>(M5.Power.getType()), M5.Display.width(),
                M5.Display.height(), displayFrameReady ? 1 : 0,
                ESP.getPsramSize());
  Serial.println(
      "CSV,type,ms,pir,mic_rms,mic_envelope,mic_min,mic_max,noise,"
      "threshold,sound,state,brightness");
}

void loop() {
  const uint32_t now = millis();
  M5.update();

  pirHigh = digitalRead(kPirPin) == HIGH;
  sampleMicrophone(now);
  readTouch(now);
  updatePresenceState(now);
  drawDisplay(now);
  printSerial(now);

  delay(1);
}
