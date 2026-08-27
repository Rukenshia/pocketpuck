#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <SPI.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "amp_stats.h"
#include "amp_logo.h"
#include "display_config.h"

namespace {

Adafruit_ST7789 display(LCD_CS, LCD_DC, LCD_RST);

constexpr int16_t SCREEN_WIDTH = 320;
constexpr int16_t SCREEN_HEIGHT = 240;
constexpr size_t SCREEN_PIXEL_COUNT = SCREEN_WIDTH * SCREEN_HEIGHT;
constexpr uint32_t FRAME_INTERVAL_MS = 80;
constexpr uint8_t DEPTH_LAYER_COUNT = 8;
constexpr float FULL_ROTATION_RADIANS = 6.283185307F;
constexpr int16_t FACE_Y_OFFSET = -14;
constexpr uint8_t BACKLIGHT_MIN = 31;
constexpr uint8_t BACKLIGHT_STEP = 32;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t ENCODER_FEEDBACK_MS = 1500;
constexpr uint32_t LONG_PRESS_MS = 700;
constexpr uint32_t BROWSER_TIMEOUT_MS = 30000;

constexpr uint32_t LOGO_ROTATION_MS = 5000;
constexpr uint32_t LOGO_HOLD_AFTER_SETUP_MS = 5000;
constexpr uint32_t OVERVIEW_INTERVAL_MS = 30000;
constexpr uint32_t OVERVIEW_DURATION_MS = 12000;
constexpr uint32_t WAKE_DURATION_MS = 2500;
constexpr uint32_t IDLE_DURATION_MS = 9000;
constexpr uint32_t BEWILDERED_DURATION_MS = 5000;
constexpr uint32_t THINKING_DURATION_MS = 6000;
constexpr uint32_t QUIET_SURPRISE_DURATION_MS = 2500;
constexpr uint32_t WORKED_DURATION_MS = 4000;
constexpr uint32_t DEMO_DURATION_MS =
    WAKE_DURATION_MS + IDLE_DURATION_MS + BEWILDERED_DURATION_MS +
    THINKING_DURATION_MS + QUIET_SURPRISE_DURATION_MS + WORKED_DURATION_MS;

constexpr int16_t LOGO_FRAME_WIDTH = AMP_LOGO_WIDTH;
constexpr int16_t LOGO_FRAME_HEIGHT = AMP_LOGO_HEIGHT + 24;
constexpr int16_t LOGO_FRAME_ROW_BYTES = (LOGO_FRAME_WIDTH + 7) / 8;
constexpr size_t LOGO_FRAME_BYTES =
    LOGO_FRAME_ROW_BYTES * LOGO_FRAME_HEIGHT;

GFXcanvas16 canvas(SCREEN_WIDTH, SCREEN_HEIGHT);
uint8_t logoFrame[LOGO_FRAME_BYTES];
uint8_t logoHighlight[LOGO_FRAME_BYTES];
uint8_t sourceRow[AMP_LOGO_WIDTH];

uint16_t logoBackground;
uint16_t logoDepthColors[4];
uint16_t logoFaceColor;
uint16_t logoHighlightColor;
uint16_t faceShadowColor;
uint16_t eyeShadowColor;
uint16_t eyeColor;
uint16_t pupilColor;
uint16_t eyeHighlightColor;
uint16_t noseShadowColor;
uint16_t noseLightColor;
uint16_t mouthColor;
uint16_t accentColor;
uint16_t unreadColor;
uint16_t textureColor;

uint32_t demoStartedAt = 0;
uint32_t lastFrameAt = 0;
uint32_t initialSetupCompletedAt = 0;
uint8_t backlightBrightness = 255;
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR const int8_t encoderTransitions[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0,
};
volatile uint8_t encoderState = 0;
volatile int8_t encoderQuarterSteps = 0;
volatile int8_t pendingEncoderSteps = 0;
uint32_t lastEncoderStepAt = 0;
bool encoderFeedbackActive = false;
int8_t lastEncoderDirection = 0;
bool buttonReading = false;
bool buttonPressed = false;
bool buttonLongHandled = false;
uint32_t buttonChangedAt = 0;
uint32_t buttonPressedAt = 0;
uint32_t lastBrowserInteractionAt = 0;
bool showingAutomaticOverview = false;
bool automaticOverviewDismissed = false;

enum class UiPage : uint8_t { Face, ThreadList, ThreadDetail };

UiPage uiPage = UiPage::Face;
uint8_t selectedThreadIndex = 0;
AmpThreadSummary detailThread;
bool detailUnreadAvailable = false;
uint8_t detailThreadTotal = 0;

struct FacePose {
  float gazeX = 0.0F;
  float gazeY = 0.0F;
  float leftEyeOpen = 1.0F;
  float rightEyeOpen = 1.0F;
  float eyeScale = 1.0F;
  float pupilScale = 1.0F;
  float mouthCurveY = 150.0F;
  float mouthTilt = 0.0F;
};

void IRAM_ATTR handleEncoderChange() {
  const uint8_t newState =
      (digitalRead(ROTARY_CLK) << 1) | digitalRead(ROTARY_DT);

  portENTER_CRITICAL_ISR(&encoderMux);
  encoderQuarterSteps += encoderTransitions[(encoderState << 2) | newState];
  encoderState = newState;
  if (encoderQuarterSteps >= 4) {
    if (pendingEncoderSteps < 100) {
      ++pendingEncoderSteps;
    }
    encoderQuarterSteps = 0;
  } else if (encoderQuarterSteps <= -4) {
    if (pendingEncoderSteps > -100) {
      --pendingEncoderSteps;
    }
    encoderQuarterSteps = 0;
  }
  portEXIT_CRITICAL_ISR(&encoderMux);
}

void adjustBacklight(int8_t direction) {
  const int16_t adjusted = static_cast<int16_t>(backlightBrightness) +
                           direction * BACKLIGHT_STEP;
  backlightBrightness =
      static_cast<uint8_t>(std::max<int16_t>(BACKLIGHT_MIN,
                                             std::min<int16_t>(255, adjusted)));
  analogWrite(LCD_BL, backlightBrightness);
  Serial.printf("Backlight: %u%%\n",
                (backlightBrightness * 100U + 127U) / 255U);
}

void showFace() {
  if (showingAutomaticOverview) {
    automaticOverviewDismissed = true;
  }
  uiPage = UiPage::Face;
  showingAutomaticOverview = false;
  encoderFeedbackActive = false;
  Serial.println("Screen: face");
}

void showThreadList(uint32_t now) {
  const AmpStatsSnapshot stats = getAmpStats();
  if (stats.threadCount > 0 && selectedThreadIndex >= stats.threadCount) {
    selectedThreadIndex = stats.threadCount - 1;
  }
  uiPage = UiPage::ThreadList;
  lastBrowserInteractionAt = now;
  encoderFeedbackActive = false;
  Serial.println("Screen: thread list");
}

void openSelectedThread(uint32_t now) {
  const AmpStatsSnapshot stats = getAmpStats();
  if (!stats.available || selectedThreadIndex >= stats.threadCount) {
    return;
  }
  detailThread = stats.threads[selectedThreadIndex];
  detailUnreadAvailable = stats.unreadAvailable;
  detailThreadTotal = stats.threadCount;
  uiPage = UiPage::ThreadDetail;
  lastBrowserInteractionAt = now;
  Serial.printf("Screen: thread %u of %u\n", selectedThreadIndex + 1,
                stats.threadCount);
}

void navigateThreads(int8_t steps, uint32_t now, bool showDetail) {
  const AmpStatsSnapshot stats = getAmpStats();
  if (!stats.available || stats.threadCount == 0) {
    return;
  }
  const int16_t next = static_cast<int16_t>(selectedThreadIndex) + steps;
  selectedThreadIndex = static_cast<uint8_t>(std::max<int16_t>(
      0, std::min<int16_t>(stats.threadCount - 1, next)));
  lastBrowserInteractionAt = now;
  if (showDetail) {
    detailThread = stats.threads[selectedThreadIndex];
    detailUnreadAvailable = stats.unreadAvailable;
    detailThreadTotal = stats.threadCount;
  }
}

void handleShortPress(uint32_t now) {
  if (uiPage == UiPage::ThreadDetail) {
    showThreadList(now);
  } else if (uiPage == UiPage::ThreadList) {
    openSelectedThread(now);
  } else if (showingAutomaticOverview) {
    showThreadList(now);
    openSelectedThread(now);
  } else {
    showThreadList(now);
  }
}

void updateControls(uint32_t now) {
  portENTER_CRITICAL(&encoderMux);
  const int8_t encoderSteps = pendingEncoderSteps;
  pendingEncoderSteps = 0;
  portEXIT_CRITICAL(&encoderMux);
  if (encoderSteps != 0) {
    if (uiPage == UiPage::ThreadList || showingAutomaticOverview) {
      if (showingAutomaticOverview) {
        showThreadList(now);
      }
      navigateThreads(encoderSteps, now, false);
    } else if (uiPage == UiPage::ThreadDetail) {
      navigateThreads(encoderSteps, now, true);
    } else {
      lastEncoderDirection = encoderSteps > 0 ? 1 : -1;
      for (int8_t step = 0; step < std::abs(encoderSteps); ++step) {
        adjustBacklight(lastEncoderDirection);
      }
      lastEncoderStepAt = now;
      encoderFeedbackActive = true;
    }
  }

  const bool reading = digitalRead(ROTARY_SW) == LOW;
  if (reading != buttonReading) {
    buttonReading = reading;
    buttonChangedAt = now;
  }
  if (now - buttonChangedAt >= BUTTON_DEBOUNCE_MS &&
      buttonPressed != buttonReading) {
    buttonPressed = buttonReading;
    if (buttonPressed) {
      buttonPressedAt = now;
      buttonLongHandled = false;
    } else if (!buttonLongHandled) {
      handleShortPress(now);
    }
  }
  if (buttonPressed && !buttonLongHandled &&
      now - buttonPressedAt >= LONG_PRESS_MS) {
    buttonLongHandled = true;
    if (uiPage != UiPage::Face || showingAutomaticOverview) {
      showFace();
    }
  }
  if (uiPage != UiPage::Face &&
      now - lastBrowserInteractionAt >= BROWSER_TIMEOUT_MS) {
    showFace();
  }
}

float clamp01(float value) {
  if (value < 0.0F) {
    return 0.0F;
  }
  if (value > 1.0F) {
    return 1.0F;
  }
  return value;
}

float smoothStep(float value) {
  const float clamped = clamp01(value);
  return clamped * clamped * (3.0F - 2.0F * clamped);
}

float mix(float from, float to, float amount) {
  return from + (to - from) * amount;
}

float blinkAt(uint32_t elapsed, uint32_t center, uint32_t duration = 240) {
  const int32_t distance =
      std::abs(static_cast<int32_t>(elapsed) - static_cast<int32_t>(center));
  const float halfDuration = duration / 2.0F;
  if (distance >= halfDuration) {
    return 1.0F;
  }
  return smoothStep(distance / halfDuration);
}

void setBitmapPixel(uint8_t* bitmap, int16_t x, int16_t y) {
  bitmap[y * LOGO_FRAME_ROW_BYTES + x / 8] |= 0x80 >> (x % 8);
}

void loadSourceRow(int16_t y) {
  std::memset(sourceRow, 0, sizeof(sourceRow));

  uint16_t offset = pgm_read_word(&AMP_LOGO_ROW_OFFSETS[y]);
  const uint8_t spanCount = pgm_read_byte(&AMP_LOGO_ROW_SPANS[offset++]);
  for (uint8_t span = 0; span < spanCount; ++span) {
    const uint8_t start = pgm_read_byte(&AMP_LOGO_ROW_SPANS[offset++]);
    const uint8_t end = pgm_read_byte(&AMP_LOGO_ROW_SPANS[offset++]);
    std::memset(sourceRow + start, 1, end - start + 1);
  }
}

void transformLogo(float phase) {
  std::memset(logoFrame, 0, sizeof(logoFrame));
  std::memset(logoHighlight, 0, sizeof(logoHighlight));

  const float turn = std::sin(phase);
  const float facing = 0.72F + 0.28F * std::fabs(std::cos(phase));
  const int16_t renderedWidth = std::lround(AMP_LOGO_WIDTH * facing);
  const int16_t left = (LOGO_FRAME_WIDTH - renderedWidth) / 2;
  const int16_t top = (LOGO_FRAME_HEIGHT - AMP_LOGO_HEIGHT) / 2;
  const float shear = turn * 10.0F;
  const int16_t highlightCenter =
      left + std::lround((turn + 1.0F) * 0.5F * renderedWidth);

  for (int16_t sourceY = 0; sourceY < AMP_LOGO_HEIGHT; ++sourceY) {
    loadSourceRow(sourceY);
    for (int16_t outputX = 0; outputX < renderedWidth; ++outputX) {
      const int16_t sourceX = outputX * AMP_LOGO_WIDTH / renderedWidth;
      if (!sourceRow[sourceX]) {
        continue;
      }

      const int16_t x = left + outputX;
      const float horizontalPosition =
          static_cast<float>(outputX) / renderedWidth - 0.5F;
      const int16_t y = top + sourceY +
                        std::lround(shear * horizontalPosition);
      setBitmapPixel(logoFrame, x, y);
      if (std::abs(x - highlightCenter) <= 8) {
        setBitmapPixel(logoHighlight, x, y);
      }
    }
  }
}

void drawLogo(uint32_t elapsed) {
  const float phase =
      FULL_ROTATION_RADIANS * (elapsed % LOGO_ROTATION_MS) / LOGO_ROTATION_MS;
  transformLogo(phase);

  const int16_t originX = (SCREEN_WIDTH - LOGO_FRAME_WIDTH) / 2;
  const int16_t originY = (SCREEN_HEIGHT - LOGO_FRAME_HEIGHT) / 2;
  const int16_t depthX = std::lround(std::sin(phase) * 14.0F);
  const int16_t depthY = std::lround(std::cos(phase) * 6.0F);
  const uint8_t colorPhase = elapsed / 360;

  canvas.fillScreen(logoBackground);
  for (int8_t layer = DEPTH_LAYER_COUNT; layer > 0; --layer) {
    const int16_t layerX = originX + depthX * layer / DEPTH_LAYER_COUNT;
    const int16_t layerY = originY + depthY * layer / DEPTH_LAYER_COUNT;
    canvas.drawBitmap(layerX, layerY, logoFrame, LOGO_FRAME_WIDTH,
                      LOGO_FRAME_HEIGHT,
                      logoDepthColors[(layer + colorPhase) % 4]);
  }
  canvas.drawBitmap(originX, originY, logoFrame, LOGO_FRAME_WIDTH,
                    LOGO_FRAME_HEIGHT, logoFaceColor);
  canvas.drawBitmap(originX, originY, logoHighlight, LOGO_FRAME_WIDTH,
                    LOGO_FRAME_HEIGHT, logoHighlightColor);
}

void drawFaceBackground() {
  const int8_t dither[16] = {
      -4, 0, -3, 1,
       2, -2, 3, -1,
      -3, 1, -4, 0,
       3, -1, 2, -2,
  };
  uint16_t* pixels = canvas.getBuffer();

  for (int16_t y = 0; y < SCREEN_HEIGHT; ++y) {
    const int32_t offsetY = y - 108;
    for (int16_t x = 0; x < SCREEN_WIDTH; ++x) {
      const int32_t offsetX = x - SCREEN_WIDTH / 2;
      const int32_t distance =
          offsetX * offsetX * 140 / 25600 + offsetY * offsetY * 190 / 17424;
      const int32_t light = 255 - std::min<int32_t>(255, distance);
      const int8_t noise = dither[(y % 4) * 4 + x % 4];

      // Deliberately greener than the source art to compensate for the
      // ST7789 panel and RGB565 making its subtle olive palette look gray.
      const uint8_t red = 38 + light * 38 / 255 + noise;
      const uint8_t green = 58 + light * 44 / 255 + noise;
      const uint8_t blue = 28 + light * 24 / 255 + noise;
      pixels[y * SCREEN_WIDTH + x] = display.color565(red, green, blue);
    }
  }

  // Sparse, fixed flecks keep the flat LCD from looking too sterile.
  for (int16_t y = 9; y < SCREEN_HEIGHT; y += 17) {
    const int16_t x = (y * 73 + 41) % (SCREEN_WIDTH - 12) + 6;
    canvas.drawPixel(x, y, textureColor);
    canvas.drawPixel((x + 97) % SCREEN_WIDTH, y + 5, textureColor);
  }
}

void drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                   uint16_t color, uint8_t thickness) {
  const int16_t dx = std::abs(x1 - x0);
  const int16_t sx = x0 < x1 ? 1 : -1;
  const int16_t dy = -std::abs(y1 - y0);
  const int16_t sy = y0 < y1 ? 1 : -1;
  int16_t error = dx + dy;

  while (true) {
    canvas.fillCircle(x0, y0, thickness / 2, color);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    const int16_t doubledError = 2 * error;
    if (doubledError >= dy) {
      error += dy;
      x0 += sx;
    }
    if (doubledError <= dx) {
      error += dx;
      y0 += sy;
    }
  }
}

void drawBezierMouth(const FacePose& pose) {
  const float leftY = 166.0F + FACE_Y_OFFSET - pose.mouthTilt * 0.5F;
  const float rightY = 166.0F + FACE_Y_OFFSET + pose.mouthTilt * 0.5F;
  int16_t previousX = 103;
  int16_t previousY = std::lround(leftY);

  for (uint8_t step = 1; step <= 28; ++step) {
    const float t = step / 28.0F;
    const float inverse = 1.0F - t;
    const int16_t x =
        std::lround(inverse * inverse * 103.0F + 2.0F * inverse * t * 160.0F +
                    t * t * 217.0F);
    const int16_t y = std::lround(
        inverse * inverse * leftY +
        2.0F * inverse * t * (pose.mouthCurveY + FACE_Y_OFFSET) +
        t * t * rightY);
    drawThickLine(previousX, previousY, x, y, mouthColor, 4);
    previousX = x;
    previousY = y;
  }
}

void drawEye(int16_t centerX, float openness, const FacePose& pose) {
  const int16_t centerY = 82 + FACE_Y_OFFSET;
  const int16_t radiusX = std::lround(29.0F * pose.eyeScale);
  const int16_t fullRadiusY = std::lround(29.0F * pose.eyeScale);
  const int16_t radiusY =
      std::max<int16_t>(2, std::lround(fullRadiusY * clamp01(openness)));

  if (openness < 0.09F) {
    drawThickLine(centerX - radiusX, centerY, centerX + radiusX, centerY,
                  eyeShadowColor, 4);
    return;
  }

  canvas.fillEllipse(centerX + 1, centerY + 3, radiusX + 3, radiusY + 3,
                     eyeShadowColor);
  canvas.fillEllipse(centerX, centerY, radiusX, radiusY, eyeColor);

  if (openness < 0.28F) {
    return;
  }

  const int16_t pupilRadius =
      std::max<int16_t>(4, std::lround(7.0F * pose.pupilScale));
  const int16_t availableX = std::max<int16_t>(0, radiusX - pupilRadius - 5);
  const int16_t availableY = std::max<int16_t>(0, radiusY - pupilRadius - 4);
  const int16_t pupilX = centerX + std::lround(pose.gazeX * availableX);
  const int16_t pupilY = centerY + std::lround(pose.gazeY * availableY);

  canvas.fillCircle(pupilX, pupilY, pupilRadius, pupilColor);
  canvas.fillCircle(pupilX - 2, pupilY - 2, 2, eyeHighlightColor);
}

void drawFace(const FacePose& pose) {
  drawFaceBackground();

  drawEye(99, pose.leftEyeOpen, pose);
  drawEye(221, pose.rightEyeOpen, pose);

  canvas.fillTriangle(159, 81 + FACE_Y_OFFSET, 144, 141 + FACE_Y_OFFSET,
                      161, 137 + FACE_Y_OFFSET, noseShadowColor);
  canvas.fillTriangle(159, 81 + FACE_Y_OFFSET, 161, 137 + FACE_Y_OFFSET,
                      171, 140 + FACE_Y_OFFSET, noseLightColor);
  canvas.drawLine(161, 137 + FACE_Y_OFFSET, 171, 140 + FACE_Y_OFFSET,
                  faceShadowColor);
  drawBezierMouth(pose);
}

FacePose neutralPose() {
  return FacePose{};
}

void drawWake(uint32_t elapsed) {
  FacePose pose = neutralPose();
  const float awake = smoothStep(elapsed / 1200.0F);
  pose.leftEyeOpen = awake;
  pose.rightEyeOpen = awake;
  pose.gazeY = mix(-0.3F, 0.0F, awake);
  pose.mouthCurveY = mix(157.0F, 149.0F, awake);
  drawFace(pose);
}

void drawIdle(uint32_t elapsed) {
  FacePose pose = neutralPose();
  pose.gazeX = std::sin(elapsed * 0.00078F) * 0.55F;
  pose.gazeY = std::cos(elapsed * 0.0011F) * 0.12F;
  pose.leftEyeOpen = blinkAt(elapsed, 2600, 280) * blinkAt(elapsed, 7900, 300);
  pose.rightEyeOpen = pose.leftEyeOpen;
  pose.mouthCurveY = 149.0F + std::sin(elapsed * 0.0008F);
  drawFace(pose);
}

void drawBewildered(uint32_t elapsed) {
  FacePose pose = neutralPose();
  const float settle = smoothStep(elapsed / 900.0F);
  pose.gazeX = mix(0.0F, -0.58F, settle) +
               std::sin(elapsed * 0.0007F) * 0.08F;
  pose.gazeY = 0.12F * settle;
  pose.leftEyeOpen = mix(1.0F, 0.84F, settle);
  pose.rightEyeOpen = mix(1.0F, 0.72F, settle);
  pose.mouthCurveY = mix(149.0F, 147.0F, settle);
  pose.mouthTilt = -2.0F * settle;
  drawFace(pose);
}

void drawThinking(uint32_t elapsed) {
  FacePose pose = neutralPose();
  const float settle = smoothStep(elapsed / 1000.0F);
  pose.gazeX = mix(0.0F, 0.62F, settle) +
               std::sin(elapsed * 0.0008F) * 0.12F;
  pose.gazeY = mix(0.0F, -0.42F, settle) +
               std::cos(elapsed * 0.0007F) * 0.06F;
  pose.leftEyeOpen = blinkAt(elapsed, 4200, 300);
  pose.rightEyeOpen = pose.leftEyeOpen;
  pose.mouthCurveY = 148.0F + std::sin(elapsed * 0.001F);
  drawFace(pose);
}

void drawQuietSurprise(uint32_t elapsed) {
  FacePose pose = neutralPose();
  const float surprise = smoothStep(elapsed / 700.0F);
  pose.eyeScale = mix(1.0F, 1.08F, surprise);
  pose.pupilScale = mix(1.0F, 0.92F, surprise);
  pose.rightEyeOpen = mix(1.0F, 0.96F, surprise);
  pose.mouthCurveY = mix(148.0F, 154.0F, surprise);
  drawFace(pose);
}

void drawWorked(uint32_t elapsed) {
  FacePose pose = neutralPose();
  const float settle = smoothStep(elapsed / 900.0F);
  pose.eyeScale = mix(1.08F, 1.0F, settle);
  pose.pupilScale = mix(0.92F, 1.0F, settle);
  pose.gazeX = 0.08F;
  pose.mouthCurveY = mix(154.0F, 149.0F, settle);
  drawFace(pose);
}

void drawCenteredText(const char* text, int16_t y, uint8_t size,
                      uint16_t color) {
  const int16_t width = std::strlen(text) * 6 * size;
  canvas.setFont();
  canvas.setTextSize(size);
  canvas.setTextColor(color);
  canvas.setCursor((SCREEN_WIDTH - width) / 2, y);
  canvas.print(text);
}

void drawStatsPanel() {
  const AmpStatsSnapshot stats = getAmpStats();
  if (!stats.configured) {
    drawCenteredText("CONFIGURE WIFI", 207, 2, eyeColor);
    return;
  }
  if (!stats.wifiConnected) {
    drawCenteredText("CONNECTING WIFI", 207, 2, eyeColor);
    return;
  }
  if (!stats.available) {
    drawCenteredText(stats.reconnecting ? "AMP RECONNECTING" : "BRIDGE UNAVAILABLE",
                     207, 2, eyeColor);
    return;
  }

  if (stats.total == 0) {
    drawCenteredText("NO THREADS", 207, 2, eyeColor);
    return;
  }

  canvas.fillRect(2, 170, 316, 68, faceShadowColor);
  const auto drawMetric = [](int16_t centerX, uint16_t value, bool available,
                             const char* firstLabel, const char* secondLabel,
                             uint16_t activeColor) {
    char count[8];
    if (available) {
      std::snprintf(count, sizeof(count), "%u", value);
    } else {
      std::snprintf(count, sizeof(count), "--");
    }
    canvas.setFont();
    canvas.setTextSize(3);
    canvas.setTextColor(available && value ? activeColor : eyeColor);
    canvas.setCursor(centerX - std::strlen(count) * 9, 176);
    canvas.print(count);
    canvas.setTextSize(1);
    if (secondLabel) {
      canvas.setCursor(centerX - std::strlen(firstLabel) * 3, 218);
      canvas.print(firstLabel);
      canvas.setCursor(centerX - std::strlen(secondLabel) * 3, 228);
      canvas.print(secondLabel);
    } else {
      canvas.setCursor(centerX - std::strlen(firstLabel) * 3, 225);
      canvas.print(firstLabel);
    }
  };

  drawMetric(40, stats.working, true, "WORKING", nullptr, eyeColor);
  drawMetric(120, stats.needsAttention, stats.attentionAvailable, "NEEDS",
             "ATTENTION", accentColor);
  drawMetric(200, stats.unread, stats.unreadAvailable, "NEW", "MESSAGES",
             unreadColor);
  drawMetric(280, stats.idle, true, "IDLE", nullptr, eyeColor);
  canvas.drawFastVLine(80, 176, 54, textureColor);
  canvas.drawFastVLine(160, 176, 54, textureColor);
  canvas.drawFastVLine(240, 176, 54, textureColor);
}

void copyEllipsized(char* output, size_t outputSize, const char* input,
                    size_t maxCharacters) {
  const size_t length = std::strlen(input);
  if (length <= maxCharacters) {
    std::snprintf(output, outputSize, "%s", input);
    return;
  }
  const size_t prefixLength = maxCharacters - 3;
  std::snprintf(output, outputSize, "%.*s...", static_cast<int>(prefixLength),
                input);
}

const char* threadStateLabel(const char* state) {
  if (std::strcmp(state, "compacting") == 0) {
    return "COMPACTING";
  }
  if (std::strcmp(state, "working") == 0) {
    return "THINKING";
  }
  if (std::strcmp(state, "streaming") == 0) {
    return "STREAMING";
  }
  if (std::strcmp(state, "tool_use") == 0) {
    return "USING TOOL";
  }
  if (std::strcmp(state, "running_tools") == 0) {
    return "RUNNING TOOLS";
  }
  if (std::strcmp(state, "awaiting_approval") == 0) {
    return "APPROVAL";
  }
  if (std::strcmp(state, "error") == 0) {
    return "ERROR";
  }
  if (std::strcmp(state, "idle") == 0) {
    return "IDLE";
  }
  return "UNKNOWN";
}

bool stateNeedsAttention(const char* state) {
  return std::strcmp(state, "awaiting_approval") == 0 ||
         std::strcmp(state, "error") == 0;
}

void drawThreadOverview() {
  const AmpStatsSnapshot stats = getAmpStats();
  canvas.fillScreen(logoBackground);
  drawCenteredText("THREAD OVERVIEW", 8, 2, eyeColor);
  canvas.drawFastHLine(12, 30, 296, logoHighlightColor);

  if (!stats.available) {
    const char* status = !stats.configured
                             ? "CONFIGURE WIFI"
                             : (!stats.wifiConnected
                                    ? "CONNECTING WIFI"
                                    : (stats.reconnecting ? "AMP RECONNECTING"
                                                          : "BRIDGE UNAVAILABLE"));
    drawCenteredText(status, 108, 2, eyeColor);
    return;
  }
  if (stats.total == 0) {
    drawCenteredText("NO THREADS", 108, 2, eyeColor);
    return;
  }

  if (uiPage == UiPage::ThreadList && stats.threadCount > 0) {
    char position[8];
    std::snprintf(position, sizeof(position), "%u/%u", selectedThreadIndex + 1,
                  stats.threadCount);
    canvas.setTextSize(1);
    canvas.setTextColor(textureColor);
    canvas.setCursor(302 - std::strlen(position) * 6, 12);
    canvas.print(position);
  }

  for (uint8_t index = 0; index < stats.threadCount; ++index) {
    const AmpThreadSummary& thread = stats.threads[index];
    const int16_t y = 39 + index * 45;
    char title[28];
    char project[34];
    copyEllipsized(title, sizeof(title), thread.title, 23);
    copyEllipsized(project, sizeof(project), thread.project, 20);
    if (uiPage == UiPage::ThreadList && index == selectedThreadIndex) {
      canvas.fillRect(4, y - 4, 312, 42, faceShadowColor);
      canvas.drawFastVLine(4, y - 4, 42, logoHighlightColor);
    }
    if (thread.unread) {
      canvas.fillCircle(9, y + 7, 4, unreadColor);
    }
    canvas.setTextSize(2);
    canvas.setTextColor(eyeColor);
    canvas.setCursor(20, y);
    canvas.print(title);
    canvas.setTextSize(1);
    canvas.setTextColor(textureColor);
    canvas.setCursor(12, y + 23);
    canvas.print(project[0] ? project : "no project");
    const char* stateLabel = threadStateLabel(thread.state);
    const bool active = std::strcmp(thread.state, "idle") != 0;
    canvas.setTextColor(stateNeedsAttention(thread.state)
                            ? accentColor
                            : (active ? logoHighlightColor : eyeColor));
    canvas.setCursor(302 - std::strlen(stateLabel) * 6, y + 23);
    canvas.print(stateLabel);
  }

  if (stats.total > stats.threadCount) {
    char more[18];
    std::snprintf(more, sizeof(more), "+%u MORE",
                  stats.total - stats.threadCount);
    canvas.setTextSize(1);
    canvas.setTextColor(textureColor);
    canvas.setCursor(308 - std::strlen(more) * 6, 224);
    canvas.print(more);
  }
}

void splitTitle(const char* title, char* first, char* second) {
  constexpr size_t maxCharacters = 24;
  std::snprintf(first, maxCharacters + 1, "%.*s",
                static_cast<int>(maxCharacters), title);
  const char* remainder = title + std::min(maxCharacters, std::strlen(title));
  while (*remainder == ' ') {
    ++remainder;
  }
  std::snprintf(second, maxCharacters + 1, "%.*s",
                static_cast<int>(maxCharacters), remainder);
}

void drawThreadDetail() {
  canvas.fillScreen(logoBackground);

  char heading[24];
  std::snprintf(heading, sizeof(heading), "THREAD %u OF %u",
                selectedThreadIndex + 1, detailThreadTotal);
  drawCenteredText(heading, 8, 2, eyeColor);
  canvas.drawFastHLine(12, 30, 296, logoHighlightColor);

  char firstLine[25];
  char secondLine[25];
  splitTitle(detailThread.title, firstLine, secondLine);
  drawCenteredText(firstLine, 43, 2, eyeColor);
  if (secondLine[0]) {
    drawCenteredText(secondLine, 67, 2, eyeColor);
  }

  char project[32];
  copyEllipsized(project, sizeof(project),
                  detailThread.project[0] ? detailThread.project : "no project",
                  28);
  drawCenteredText(project, 103, 1, textureColor);

  const char* stateLabel = threadStateLabel(detailThread.state);
  const uint16_t stateColor = stateNeedsAttention(detailThread.state)
                                  ? accentColor
                                  : (std::strcmp(detailThread.state, "idle") == 0
                                         ? eyeColor
                                         : logoHighlightColor);
  drawCenteredText(stateLabel, 128, 2, stateColor);
  drawCenteredText(detailUnreadAvailable
                       ? (detailThread.unread ? "NEW MESSAGE" : "NO NEW MESSAGES")
                       : "MESSAGES UNKNOWN",
                   159, 1,
                   detailUnreadAvailable && detailThread.unread ? unreadColor
                                                                : textureColor);
  drawCenteredText(detailThread.executorConnected ? "EXECUTOR CONNECTED"
                                                   : "NO EXECUTOR ATTACHED",
                   178, 1, textureColor);

  canvas.drawFastHLine(12, 204, 296, logoHighlightColor);
  drawCenteredText("TURN: THREAD   PRESS: BACK", 218, 1, eyeColor);
}

void drawEncoderFeedback() {
  if (!encoderFeedbackActive ||
      millis() - lastEncoderStepAt >= ENCODER_FEEDBACK_MS) {
    encoderFeedbackActive = false;
    return;
  }

  constexpr int16_t panelX = 48;
  constexpr int16_t panelY = 174;
  constexpr int16_t panelWidth = 224;
  constexpr int16_t panelHeight = 60;
  constexpr int16_t barWidth = 192;
  const int16_t filledWidth =
      (backlightBrightness - BACKLIGHT_MIN) * barWidth /
      (255 - BACKLIGHT_MIN);
  char label[24];
  std::snprintf(label, sizeof(label), "%s  %u%%",
                lastEncoderDirection > 0 ? "BRIGHTER" : "DIMMER",
                (backlightBrightness * 100U + 127U) / 255U);

  canvas.fillRect(panelX, panelY, panelWidth, panelHeight, logoBackground);
  canvas.drawRect(panelX, panelY, panelWidth, panelHeight, eyeColor);
  drawCenteredText(label, panelY + 9, 2, eyeColor);
  canvas.drawRect(panelX + 15, panelY + 40, barWidth + 2, 10, eyeColor);
  if (filledWidth > 0) {
    canvas.fillRect(panelX + 16, panelY + 41, filledWidth, 8,
                    logoHighlightColor);
  }
}

void pushCanvas() {
  drawEncoderFeedback();
  display.startWrite();
  display.setAddrWindow(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  display.writePixels(canvas.getBuffer(), SCREEN_PIXEL_COUNT);
  display.endWrite();
}

void drawDemoFrame(uint32_t elapsed) {
  uint32_t sceneTime = elapsed % DEMO_DURATION_MS;
  if (sceneTime < WAKE_DURATION_MS) {
    drawWake(sceneTime);
  } else if ((sceneTime -= WAKE_DURATION_MS) < IDLE_DURATION_MS) {
    drawIdle(sceneTime);
  } else if ((sceneTime -= IDLE_DURATION_MS) < BEWILDERED_DURATION_MS) {
    drawBewildered(sceneTime);
  } else if ((sceneTime -= BEWILDERED_DURATION_MS) < THINKING_DURATION_MS) {
    drawThinking(sceneTime);
  } else if ((sceneTime -= THINKING_DURATION_MS) <
             QUIET_SURPRISE_DURATION_MS) {
    drawQuietSurprise(sceneTime);
  } else {
    sceneTime -= QUIET_SURPRISE_DURATION_MS;
    drawWorked(sceneTime);
  }
  drawStatsPanel();
  pushCanvas();
}

void printWiring() {
  Serial.println();
  Serial.println("PocketPuck personality demo");
  Serial.println("Waveshare LCD -> Nano ESP32");
  Serial.println("VCC -> 3V3, GND -> GND");
  Serial.println("DIN -> D11, CLK -> D12, CS -> D10");
  Serial.println("DC  -> D7,  RST -> D8,  BL -> D9");
  Serial.println("Rotary CLK -> D2, DT -> D3, SW -> D4");
  Serial.println("Rotary + -> D5, GND -> GND");
}

void initializeColors() {
  logoBackground = display.color565(10, 17, 18);
  logoDepthColors[0] = display.color565(14, 20, 21);
  logoDepthColors[1] = display.color565(17, 24, 25);
  logoDepthColors[2] = display.color565(21, 29, 31);
  logoDepthColors[3] = display.color565(24, 33, 35);
  logoFaceColor = display.color565(28, 35, 37);
  logoHighlightColor = display.color565(48, 59, 61);

  faceShadowColor = display.color565(35, 55, 27);
  eyeShadowColor = display.color565(38, 58, 29);
  eyeColor = display.color565(214, 212, 179);
  pupilColor = display.color565(35, 37, 25);
  eyeHighlightColor = display.color565(244, 238, 210);
  noseShadowColor = display.color565(38, 59, 31);
  noseLightColor = display.color565(94, 119, 72);
  mouthColor = display.color565(31, 45, 24);
  accentColor = display.color565(184, 92, 45);
  unreadColor = display.color565(66, 154, 224);
  textureColor = display.color565(105, 128, 77);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  printWiring();

  // D13 is both the default SPI clock and the Nano's yellow LED. Keep it low
  // and move the hardware SPI clock to unused D12 so display writes do not
  // flash the LED. SPIClass::begin() is idempotent, so display.init() keeps
  // this pin mapping when Adafruit_SPITFT calls begin() again.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  SPI.begin(LCD_SCK, -1, LCD_COPI, LCD_CS);

  pinMode(LCD_BL, OUTPUT);
  analogWrite(LCD_BL, backlightBrightness);

  pinMode(ROTARY_VCC, OUTPUT);
  digitalWrite(ROTARY_VCC, HIGH);
  pinMode(ROTARY_CLK, INPUT_PULLUP);
  pinMode(ROTARY_DT, INPUT_PULLUP);
  pinMode(ROTARY_SW, INPUT_PULLUP);
  encoderState =
      (digitalRead(ROTARY_CLK) << 1) | digitalRead(ROTARY_DT);
  attachInterrupt(digitalPinToInterrupt(ROTARY_CLK), handleEncoderChange,
                  CHANGE);
  attachInterrupt(digitalPinToInterrupt(ROTARY_DT), handleEncoderChange,
                  CHANGE);
  buttonReading = digitalRead(ROTARY_SW) == LOW;
  buttonPressed = buttonReading;

  display.init(LCD_NATIVE_WIDTH, LCD_NATIVE_HEIGHT);
  display.setRotation(LCD_ROTATION);
  display.setSPISpeed(32000000);
  initializeColors();

  if (!canvas.getBuffer()) {
    Serial.println("Unable to allocate the display framebuffer");
    while (true) {
      delay(1000);
    }
  }

  demoStartedAt = millis();
  beginAmpStats();
  drawDemoFrame(0);
  lastFrameAt = millis();
}

void loop() {
  const uint32_t now = millis();
  updateControls(now);
  updateAmpStats(now);
  if (now - lastFrameAt < FRAME_INTERVAL_MS) {
    return;
  }

  lastFrameAt = now;
  const AmpStatsSnapshot stats = getAmpStats();
  if (initialSetupCompletedAt == 0 && stats.initialAttemptComplete) {
    initialSetupCompletedAt = now;
  }
  if (initialSetupCompletedAt == 0 ||
      now - initialSetupCompletedAt < LOGO_HOLD_AFTER_SETUP_MS) {
    drawLogo(now - demoStartedAt);
    pushCanvas();
    return;
  }

  const uint32_t mainElapsed = now - initialSetupCompletedAt -
                               LOGO_HOLD_AFTER_SETUP_MS;
  const bool automaticOverview =
      mainElapsed >= OVERVIEW_INTERVAL_MS &&
      mainElapsed % OVERVIEW_INTERVAL_MS < OVERVIEW_DURATION_MS;
  if (!automaticOverview) {
    automaticOverviewDismissed = false;
  }
  showingAutomaticOverview =
      uiPage == UiPage::Face && automaticOverview &&
      !automaticOverviewDismissed;
  if (uiPage == UiPage::ThreadDetail) {
    drawThreadDetail();
    pushCanvas();
  } else if (uiPage == UiPage::ThreadList || showingAutomaticOverview) {
    drawThreadOverview();
    pushCanvas();
  } else {
    drawDemoFrame(mainElapsed);
  }
}
