#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "amp_stats.h"
#include "amp_logo.h"
#include "display_config.h"
#include "fonts/PlexMono9pt7b.h"

namespace {

Adafruit_ST7789 display(LCD_CS, LCD_DC, LCD_RST);

constexpr int16_t SCREEN_WIDTH = 320;
constexpr int16_t SCREEN_HEIGHT = 240;
constexpr size_t SCREEN_PIXEL_COUNT = SCREEN_WIDTH * SCREEN_HEIGHT;
constexpr uint32_t FRAME_INTERVAL_MS = 80;
constexpr float IRIS_SMOOTHING_TIME_MS = 180.0F;
constexpr uint8_t DEPTH_LAYER_COUNT = 8;
constexpr float FULL_ROTATION_RADIANS = 6.283185307F;
constexpr int16_t FACE_Y_OFFSET = 12;
constexpr uint8_t BACKLIGHT_MIN = 31;
constexpr uint8_t BACKLIGHT_STEP = 32;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t ENCODER_FEEDBACK_MS = 1500;
constexpr uint32_t LONG_PRESS_MS = 700;
constexpr uint32_t BROWSER_TIMEOUT_MS = 30000;
constexpr uint32_t NOTIFICATION_DURATION_MS = 4200;
constexpr uint32_t SHIPPED_NOTIFICATION_DURATION_MS =
    NOTIFICATION_DURATION_MS + 5000;

constexpr uint32_t LOGO_ROTATION_MS = 5000;
constexpr uint32_t STARTUP_LOGO_DURATION_MS = 5000;
constexpr uint32_t CONNECTING_FACE_MIN_DURATION_MS = 2000;
constexpr uint32_t WAKING_ANIMATION_DURATION_MS = 2000;

// Confirmed faces render live Amp state. Holding the dial button opens a
// picker that automatically runs the synchronized fixture lifecycle while the
// dial previews each design. The optional scripted toggle runs that lifecycle
// outside the picker too.
constexpr bool DESIGN_REEL_SCRIPTED = false;
constexpr uint8_t REEL_VERSION = 5;
constexpr uint8_t REEL_MODE_COUNT = 4;
constexpr uint8_t DEBUG_FACE_PHASE_COUNT = 5;
constexpr uint32_t REEL_OVERLAY_MS = 1800;
constexpr uint32_t IDLE_MESSAGE_DELAY_MS = 20 * 60 * 1000;
constexpr uint32_t IDLE_MESSAGE_INTERVAL_MS = 20 * 60 * 1000;
constexpr uint32_t IDLE_MESSAGE_DURATION_MS = 2 * 60 * 1000;
const char* const REEL_MODE_NAMES[REEL_MODE_COUNT] = {
    "MINIMAL", "KNOCK", "BEACON", "PANIC",
};
const char* const DEBUG_FACE_PHASE_NAMES[DEBUG_FACE_PHASE_COUNT] = {
    "IDLE", "WORKING", "MESSAGE", "ATTENTION", "ALL CLEAR",
};
const char* const IDLE_MESSAGES[] = {
    "Quiet here.",
    "Still here.",
    "Suspiciously quiet.",
    "Nothing has exploded.",
    "I await developments.",
    "A rare moment of order.",
    "Threads asleep. Allegedly.",
    "All systems plausible.",
};

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
uint32_t wifiConnectionStartedAt = 0;
uint32_t wakingAnimationStartedAt = 0;
bool ampStatsStarted = false;
bool connectingFaceDrawn = false;
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

enum class UiPage : uint8_t {
  Face,
  MainMenu,
  Settings,
  ThreadList,
  ThreadDetail,
};
enum class NotificationKind : uint8_t {
  None,
  Attention,
  Message,
  ThreadActive,
  Shipped,
};

UiPage uiPage = UiPage::Face;
uint8_t mainMenuIndex = 0;
uint8_t settingsMenuIndex = 0;
bool blinkingDisabled = false;
uint32_t settingsResetAt = 0;
bool debugFaceActive = false;
uint8_t debugFacePhase = 0;
uint32_t debugFacePhaseChangedAt = 0;
uint8_t selectedThreadIndex = 0;
AmpThreadSummary detailThread;
bool detailUnreadAvailable = false;
uint8_t detailThreadTotal = 0;
NotificationKind notificationKind = NotificationKind::None;
uint32_t notificationStartedAt = 0;
char notificationThreadTitle[AMP_THREAD_TITLE_LENGTH] = "";
AmpStatsSnapshot previousStats;
bool statsBaselineReady = false;
uint32_t reelAllClearStartedAt = 0;

uint8_t reelMode = 0;
uint8_t reelModeBeforeSelection = 0;
uint32_t reelModeChangedAt = 0;
bool reelModeSelecting = false;
uint32_t reelSelectionStartedAt = 0;
uint32_t idleMessageCycleStartedAt = 0;

struct FacePose {
  float gazeX = 0.0F;
  float gazeY = 0.0F;
  float leftEyeOpen = 1.0F;
  float rightEyeOpen = 1.0F;
  float eyeScale = 1.0F;
  float pupilScale = 1.0F;
  float mouthCurveY = 150.0F;
  float mouthTilt = 0.0F;
  float xOffset = 0.0F;
  float yOffset = 0.0F;
};

float irisTargetX = 0.0F;
float irisTargetY = 0.0F;
float irisPositionX = 0.0F;
float irisPositionY = 0.0F;
uint32_t irisUpdatedAt = 0;
bool irisSmoothingReady = false;

void pushCanvas();

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
  uiPage = UiPage::Face;
  debugFaceActive = false;
  reelModeSelecting = false;
  encoderFeedbackActive = false;
  Serial.println("Screen: face");
}

void showMainMenu(uint32_t now) {
  if (reelModeSelecting) {
    reelMode = reelModeBeforeSelection;
    reelModeSelecting = false;
  }
  uiPage = UiPage::MainMenu;
  debugFaceActive = false;
  mainMenuIndex = 0;
  lastBrowserInteractionAt = now;
  encoderFeedbackActive = false;
  Serial.println("Screen: main menu");
}

void showSettings(uint32_t now) {
  uiPage = UiPage::Settings;
  debugFaceActive = false;
  settingsMenuIndex = 0;
  lastBrowserInteractionAt = now;
  encoderFeedbackActive = false;
  Serial.println("Screen: settings");
}

void showFacePicker(uint32_t now) {
  uiPage = UiPage::Face;
  debugFaceActive = false;
  reelModeBeforeSelection = reelMode;
  reelModeSelecting = true;
  reelModeChangedAt = now;
  reelSelectionStartedAt = now;
  encoderFeedbackActive = false;
  Serial.println("Face picker: turn to preview, click to confirm");
}

void showDebugFace(uint32_t now) {
  uiPage = UiPage::Face;
  debugFaceActive = true;
  debugFacePhase = 0;
  debugFacePhaseChangedAt = now;
  lastBrowserInteractionAt = now;
  encoderFeedbackActive = false;
  Serial.printf("Debug face: %s; turn to change state, press to return\n",
                DEBUG_FACE_PHASE_NAMES[debugFacePhase]);
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

void saveSelectedFace() {
  Preferences preferences;
  preferences.begin("pocketpuck", false);
  preferences.putUChar("design", reelMode);
  preferences.putUChar("designVer", REEL_VERSION);
  preferences.end();
}

void saveBlinkingPreference() {
  Preferences preferences;
  preferences.begin("pocketpuck", false);
  preferences.putBool("noBlink", blinkingDisabled);
  preferences.end();
}

void resetSettings(uint32_t now) {
  Preferences preferences;
  preferences.begin("pocketpuck", false);
  preferences.clear();
  preferences.end();

  reelMode = 0;
  blinkingDisabled = false;
  debugFaceActive = false;
  debugFacePhase = 0;
  debugFacePhaseChangedAt = now;
  reelModeChangedAt = now;
  settingsResetAt = now;
  Serial.println("Settings reset to defaults");
}

void handleShortPress(uint32_t now) {
  if (uiPage == UiPage::Face && debugFaceActive) {
    showSettings(now);
  } else if (uiPage == UiPage::Face && reelModeSelecting) {
    reelModeSelecting = false;
    reelModeChangedAt = now;
    saveSelectedFace();
    Serial.printf("Design confirmed: %u/%u %s\n", reelMode + 1,
                  REEL_MODE_COUNT, REEL_MODE_NAMES[reelMode]);
  } else if (uiPage == UiPage::MainMenu) {
    if (mainMenuIndex == 0) {
      showFacePicker(now);
    } else {
      showSettings(now);
    }
  } else if (uiPage == UiPage::Settings) {
    lastBrowserInteractionAt = now;
    if (settingsMenuIndex == 0) {
      blinkingDisabled = !blinkingDisabled;
      saveBlinkingPreference();
      Serial.printf("Blinking: %s\n", blinkingDisabled ? "disabled" : "enabled");
    } else if (settingsMenuIndex == 1) {
      showDebugFace(now);
    } else {
      resetSettings(now);
    }
  } else if (uiPage == UiPage::ThreadDetail) {
    showThreadList(now);
  } else if (uiPage == UiPage::ThreadList) {
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
    if (uiPage == UiPage::MainMenu) {
      const int16_t next = static_cast<int16_t>(mainMenuIndex) + encoderSteps;
      mainMenuIndex = static_cast<uint8_t>(
          std::max<int16_t>(0, std::min<int16_t>(1, next)));
      lastBrowserInteractionAt = now;
    } else if (uiPage == UiPage::Settings) {
      const int16_t next =
          static_cast<int16_t>(settingsMenuIndex) + encoderSteps;
      settingsMenuIndex = static_cast<uint8_t>(
          std::max<int16_t>(0, std::min<int16_t>(2, next)));
      lastBrowserInteractionAt = now;
    } else if (uiPage == UiPage::ThreadList) {
      navigateThreads(encoderSteps, now, false);
    } else if (uiPage == UiPage::ThreadDetail) {
      navigateThreads(encoderSteps, now, true);
    } else if (reelModeSelecting) {
      const int16_t next =
          (static_cast<int16_t>(reelMode) + encoderSteps) % REEL_MODE_COUNT;
      reelMode = static_cast<uint8_t>((next + REEL_MODE_COUNT) %
                                      REEL_MODE_COUNT);
      reelModeChangedAt = now;
      Serial.printf("Design preview: %u/%u %s\n", reelMode + 1,
                    REEL_MODE_COUNT, REEL_MODE_NAMES[reelMode]);
    } else if (debugFaceActive) {
      const int16_t next =
          (static_cast<int16_t>(debugFacePhase) + encoderSteps) %
          DEBUG_FACE_PHASE_COUNT;
      debugFacePhase = static_cast<uint8_t>(
          (next + DEBUG_FACE_PHASE_COUNT) % DEBUG_FACE_PHASE_COUNT);
      debugFacePhaseChangedAt = now;
      Serial.printf("Debug face: %s\n",
                    DEBUG_FACE_PHASE_NAMES[debugFacePhase]);
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
    if (uiPage != UiPage::Face) {
      showFace();
    } else {
      showMainMenu(now);
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
  if (blinkingDisabled) {
    return 1.0F;
  }
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

void drawFaceBackground(int16_t centerX = SCREEN_WIDTH / 2) {
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
      const int32_t offsetX = x - centerX;
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
  const float faceX = pose.xOffset;
  const float faceY = FACE_Y_OFFSET + pose.yOffset;
  const float leftY = 166.0F + faceY - pose.mouthTilt * 0.5F;
  const float rightY = 166.0F + faceY + pose.mouthTilt * 0.5F;
  int16_t previousX = std::lround(103.0F + faceX);
  int16_t previousY = std::lround(leftY);

  for (uint8_t step = 1; step <= 28; ++step) {
    const float t = step / 28.0F;
    const float inverse = 1.0F - t;
    const int16_t x =
        std::lround(inverse * inverse * (103.0F + faceX) +
                    2.0F * inverse * t * (160.0F + faceX) +
                    t * t * (217.0F + faceX));
    const int16_t y = std::lround(
        inverse * inverse * leftY +
        2.0F * inverse * t * (pose.mouthCurveY + faceY) +
        t * t * rightY);
    drawThickLine(previousX, previousY, x, y, mouthColor, 4);
    previousX = x;
    previousY = y;
  }
}

void drawClippedEllipse(int16_t centerX, int16_t centerY, int16_t radiusX,
                        int16_t radiusY, int16_t clipTop,
                        int16_t clipBottom, uint16_t color) {
  const int16_t top = std::max<int16_t>(centerY - radiusY, clipTop);
  const int16_t bottom = std::min<int16_t>(centerY + radiusY, clipBottom);
  if (top == centerY - radiusY && bottom == centerY + radiusY) {
    canvas.fillEllipse(centerX, centerY, radiusX, radiusY, color);
    return;
  }
  for (int16_t y = top; y <= bottom; ++y) {
    const float normalizedY =
        static_cast<float>(y - centerY) / static_cast<float>(radiusY);
    const int16_t rowRadius = std::lround(
        radiusX * std::sqrt(std::max(0.0F, 1.0F - normalizedY * normalizedY)));
    canvas.drawFastHLine(centerX - rowRadius, y, rowRadius * 2 + 1, color);
  }
}

void drawClosedEye(int16_t centerX, int16_t centerY, int16_t radiusX) {
  int16_t previousX = centerX - radiusX;
  int16_t previousY = centerY;
  for (uint8_t step = 1; step <= 12; ++step) {
    const float offset = step / 6.0F - 1.0F;
    const int16_t x = centerX - radiusX + radiusX * 2 * step / 12;
    const int16_t y =
        centerY + std::lround((1.0F - offset * offset) * 5.0F);
    drawThickLine(previousX, previousY, x, y, eyeShadowColor, 4);
    previousX = x;
    previousY = y;
  }
}

void drawEye(int16_t centerX, float openness, const FacePose& pose) {
  const int16_t centerY = std::lround(82 + FACE_Y_OFFSET + pose.yOffset);
  const int16_t radiusX = std::lround(29.0F * pose.eyeScale);
  const int16_t radiusY = std::lround(29.0F * pose.eyeScale);
  const float open = clamp01(openness);

  if (open < 0.08F) {
    drawClosedEye(centerX, centerY, radiusX);
    return;
  }

  const int16_t closedY = centerY + 5;
  const int16_t clipTop =
      std::lround(mix(closedY, centerY - radiusY, open));
  const int16_t clipBottom =
      std::lround(mix(closedY, centerY + radiusY, open));
  drawClippedEllipse(centerX + 1, centerY + 3, radiusX + 3, radiusY + 3,
                     clipTop, clipBottom + 6, eyeShadowColor);
  drawClippedEllipse(centerX, centerY, radiusX, radiusY, clipTop, clipBottom,
                     eyeColor);

  if (open < 0.28F) {
    return;
  }

  const int16_t pupilRadius =
      std::max<int16_t>(4, std::lround(7.0F * pose.pupilScale));
  const int16_t availableX = std::max<int16_t>(0, radiusX - pupilRadius - 5);
  const int16_t availableY =
      std::max<int16_t>(0, (clipBottom - clipTop) / 2 - pupilRadius - 4);
  const int16_t pupilX = centerX + std::lround(pose.gazeX * availableX);
  const int16_t pupilY = centerY + std::lround(pose.gazeY * availableY);

  drawClippedEllipse(pupilX, pupilY, pupilRadius, pupilRadius, clipTop,
                     clipBottom, pupilColor);
  drawClippedEllipse(pupilX - 2, pupilY - 2, 2, 2, clipTop, clipBottom,
                     eyeHighlightColor);
}

FacePose smoothIris(const FacePose& targetPose) {
  FacePose pose = targetPose;
  const uint32_t now = millis();
  if (!irisSmoothingReady) {
    irisTargetX = targetPose.gazeX;
    irisTargetY = targetPose.gazeY;
    irisPositionX = targetPose.gazeX;
    irisPositionY = targetPose.gazeY;
    irisUpdatedAt = now;
    irisSmoothingReady = true;
  } else {
    const uint32_t elapsed =
        std::min<uint32_t>(now - irisUpdatedAt, FRAME_INTERVAL_MS);
    const float blend =
        1.0F - std::exp(-static_cast<float>(elapsed) /
                        IRIS_SMOOTHING_TIME_MS);
    irisTargetX = mix(irisTargetX, targetPose.gazeX, blend);
    irisTargetY = mix(irisTargetY, targetPose.gazeY, blend);
    irisPositionX = mix(irisPositionX, irisTargetX, blend);
    irisPositionY = mix(irisPositionY, irisTargetY, blend);
    irisUpdatedAt = now;
  }
  pose.gazeX = irisPositionX;
  pose.gazeY = irisPositionY;
  return pose;
}

void drawFace(const FacePose& targetPose) {
  const FacePose pose = smoothIris(targetPose);
  const int16_t faceX = std::lround(pose.xOffset);
  drawFaceBackground(SCREEN_WIDTH / 2 + faceX);

  drawEye(99 + faceX, pose.leftEyeOpen, pose);
  drawEye(221 + faceX, pose.rightEyeOpen, pose);

  const int16_t faceY = std::lround(FACE_Y_OFFSET + pose.yOffset);
  canvas.fillTriangle(159 + faceX, 81 + faceY, 144 + faceX, 141 + faceY,
                      161 + faceX, 137 + faceY, noseShadowColor);
  canvas.fillTriangle(159 + faceX, 81 + faceY, 161 + faceX, 137 + faceY,
                      171 + faceX, 140 + faceY, noseLightColor);
  canvas.drawLine(161 + faceX, 137 + faceY, 171 + faceX, 140 + faceY,
                  faceShadowColor);
  drawBezierMouth(pose);
}

FacePose neutralPose() {
  return FacePose{};
}

FacePose wakingPose(uint32_t elapsed) {
  FacePose pose = neutralPose();
  const auto eyeOpening = [elapsed](uint32_t delay) {
    if (elapsed <= delay) {
      return 0.0F;
    }
    return smoothStep((elapsed - delay) / 1300.0F);
  };
  pose.leftEyeOpen = eyeOpening(160);
  pose.rightEyeOpen = eyeOpening(280);
  pose.gazeY = mix(0.35F, 0.0F, smoothStep(elapsed / 1700.0F));
  return pose;
}

void setSelectedFont(uint8_t size) {
  if (size == 1) {
    canvas.setFont();
    canvas.setTextSize(size);
    return;
  }

  canvas.setFont(&IBMPlexMono_Medium9pt7b);
  canvas.setTextSize(std::max<uint8_t>(1, (size + 1) / 2));
}

void selectedTextDimensions(const char* text, uint8_t size, uint16_t& width,
                            uint16_t& height) {
  setSelectedFont(size);
  canvas.setTextWrap(false);
  int16_t x1;
  int16_t y1;
  canvas.getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  canvas.setTextWrap(true);
  canvas.setFont();
  canvas.setTextSize(1);
}

uint16_t selectedTextWidth(const char* text, uint8_t size) {
  uint16_t width;
  uint16_t height;
  selectedTextDimensions(text, size, width, height);
  return width;
}

uint8_t fittedSelectedTextSize(const char* text, uint8_t preferredSize,
                               uint16_t maxWidth) {
  for (uint8_t size = preferredSize; size > 1; --size) {
    if (selectedTextWidth(text, size) <= maxWidth) {
      return size;
    }
  }
  return 1;
}

void drawSelectedText(const char* text, int16_t x, int16_t y, uint8_t size,
                      uint16_t color) {
  setSelectedFont(size);
  canvas.setTextWrap(false);
  canvas.setTextColor(color);
  int16_t x1;
  int16_t y1;
  uint16_t width;
  uint16_t height;
  canvas.getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  canvas.setCursor(x - x1, y - y1);
  canvas.print(text);
  canvas.setTextWrap(true);
  canvas.setFont();
  canvas.setTextSize(1);
}

void drawCenteredText(const char* text, int16_t y, uint8_t size,
                      uint16_t color) {
  const uint16_t width = selectedTextWidth(text, size);
  drawSelectedText(text, (SCREEN_WIDTH - static_cast<int16_t>(width)) / 2, y,
                   size, color);
}

void drawMenuRow(const char* label, const char* value, int16_t y,
                 bool selected) {
  if (selected) {
    canvas.fillRoundRect(12, y, 296, 48, 6, faceShadowColor);
    canvas.drawFastVLine(12, y, 48, logoHighlightColor);
  }
  canvas.setFont();
  canvas.setTextSize(2);
  canvas.setTextColor(eyeColor);
  canvas.setCursor(28, y + 9);
  canvas.print(label);
  if (value) {
    canvas.setTextSize(1);
    canvas.setTextColor(selected ? logoHighlightColor : textureColor);
    canvas.setCursor(294 - std::strlen(value) * 6, y + 16);
    canvas.print(value);
  }
}

void drawMainMenu() {
  canvas.fillScreen(logoBackground);
  drawCenteredText("MENU", 12, 2, eyeColor);
  canvas.drawFastHLine(12, 36, 296, logoHighlightColor);
  drawMenuRow("SELECT FACE", REEL_MODE_NAMES[reelMode], 75,
              mainMenuIndex == 0);
  drawMenuRow("SETTINGS", nullptr, 131, mainMenuIndex == 1);
  drawCenteredText("TURN: SELECT   PRESS: OPEN", 218, 1, textureColor);
}

void drawSettings(uint32_t now) {
  canvas.fillScreen(logoBackground);
  drawCenteredText("SETTINGS", 12, 2, eyeColor);
  canvas.drawFastHLine(12, 36, 296, logoHighlightColor);
  drawMenuRow("DISABLE BLINKING", nullptr, 47, settingsMenuIndex == 0);
  canvas.drawRect(278, 62, 18, 18,
                  blinkingDisabled ? logoHighlightColor : eyeColor);
  if (blinkingDisabled) {
    drawThickLine(281, 70, 286, 76, logoHighlightColor, 3);
    drawThickLine(286, 76, 294, 65, logoHighlightColor, 3);
  }
  drawMenuRow("DEBUG FACE", "PRESS TO START", 103,
              settingsMenuIndex == 1);
  drawMenuRow("RESET SETTINGS", nullptr, 159, settingsMenuIndex == 2);
  if (settingsResetAt != 0 && now - settingsResetAt < REEL_OVERLAY_MS) {
    drawCenteredText("SETTINGS RESET", 185, 1, logoHighlightColor);
  }
  drawCenteredText("HOLD: BACK TO FACE", 218, 1, textureColor);
}

void drawSideMetric(int16_t centerX, int16_t y, uint16_t value,
                    bool available, const char* firstLabel,
                    const char* secondLabel, uint16_t activeColor) {
  char count[8];
  if (available) {
    std::snprintf(count, sizeof(count), "%u", value);
  } else {
    std::snprintf(count, sizeof(count), "--");
  }

  canvas.fillRoundRect(centerX - 27, y, 54, 60, 6, faceShadowColor);
  canvas.setFont();
  canvas.setTextColor(available && value ? activeColor : eyeColor);
  canvas.setTextSize(2);
  canvas.setCursor(centerX - std::strlen(count) * 6, y + 8);
  canvas.print(count);
  canvas.setTextSize(1);
  canvas.setCursor(centerX - std::strlen(firstLabel) * 3, y + 37);
  canvas.print(firstLabel);
  if (secondLabel) {
    canvas.setCursor(centerX - std::strlen(secondLabel) * 3, y + 47);
    canvas.print(secondLabel);
  }
}

void drawStatsPanel(const AmpStatsSnapshot& stats) {
  if (!stats.configured) {
    drawCenteredText("CONFIGURE WIFI", 207, 2, eyeColor);
    return;
  }
  if (!stats.wifiConnected) {
    drawCenteredText("CONNECTING", 207, 2, eyeColor);
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

  drawSideMetric(29, 77, stats.working, true, "WORKING", nullptr, eyeColor);
  drawSideMetric(29, 143, stats.needsAttention, stats.attentionAvailable,
                 "NEEDS", "YOU", accentColor);
  drawSideMetric(291, 77, stats.unread, stats.unreadAvailable, "NEW",
                 "MESSAGES", unreadColor);
  drawSideMetric(291, 143, stats.idle, true, "IDLE", nullptr, eyeColor);
}

const AmpThreadSummary* findPreviousThread(const AmpThreadSummary& thread) {
  for (uint8_t index = 0; index < previousStats.threadCount; ++index) {
    const AmpThreadSummary& previous = previousStats.threads[index];
    if ((thread.id[0] && std::strcmp(thread.id, previous.id) == 0) ||
        (!thread.id[0] && std::strcmp(thread.title, previous.title) == 0)) {
      return &previous;
    }
  }
  return nullptr;
}

bool stateIsWorking(const char* state) {
  return std::strcmp(state, "compacting") == 0 ||
         std::strcmp(state, "working") == 0 ||
         std::strcmp(state, "streaming") == 0 ||
         std::strcmp(state, "tool_use") == 0 ||
         std::strcmp(state, "running_tools") == 0;
}

void startNotification(NotificationKind kind, const char* title, uint32_t now) {
  notificationKind = kind;
  notificationStartedAt = now;
  std::snprintf(notificationThreadTitle, sizeof(notificationThreadTitle), "%s",
                title ? title : "");
  Serial.printf("Notification: %s%s%s\n",
                kind == NotificationKind::Attention
                    ? "needs attention"
                    : (kind == NotificationKind::Message
                           ? "new message"
                           : (kind == NotificationKind::Shipped
                                  ? "shipped"
                                  : "thread working")),
                notificationThreadTitle[0] ? " — " : "",
                notificationThreadTitle);
}

void updateNotifications(uint32_t now) {
  const AmpStatsSnapshot current = getAmpStats();
  if (!current.available) {
    statsBaselineReady = false;
    return;
  }
  if (!statsBaselineReady) {
    previousStats = current;
    statsBaselineReady = true;
    return;
  }

  const char* messageTitle = nullptr;
  const char* activeTitle = nullptr;
  const char* attentionTitle = nullptr;
  const char* shippedTitle = nullptr;
  for (uint8_t index = 0; index < current.threadCount; ++index) {
    const AmpThreadSummary& thread = current.threads[index];
    const AmpThreadSummary* previous = findPreviousThread(thread);
    const bool needsAttention =
        std::strcmp(thread.state, "awaiting_approval") == 0 ||
        std::strcmp(thread.state, "error") == 0;
    const bool previouslyNeededAttention =
        previous && (std::strcmp(previous->state, "awaiting_approval") == 0 ||
                     std::strcmp(previous->state, "error") == 0);
    if (!attentionTitle && needsAttention && !previouslyNeededAttention) {
      attentionTitle = thread.title;
    }
    if (!messageTitle && thread.unread && previous && !previous->unread) {
      messageTitle = thread.title;
    }
    if (!activeTitle && stateIsWorking(thread.state) && previous &&
        !stateIsWorking(previous->state)) {
      activeTitle = thread.title;
    }
    if (!shippedTitle && thread.shipped &&
        (!previous || !previous->shipped)) {
      shippedTitle = thread.title;
    }
  }

  if (current.attentionAvailable &&
      current.needsAttention > previousStats.needsAttention) {
    if (!attentionTitle) {
      for (uint8_t index = 0; index < current.threadCount; ++index) {
        const AmpThreadSummary& thread = current.threads[index];
        if (std::strcmp(thread.state, "awaiting_approval") == 0 ||
            std::strcmp(thread.state, "error") == 0) {
          attentionTitle = thread.title;
          break;
        }
      }
    }
    startNotification(NotificationKind::Attention, attentionTitle, now);
  } else if (current.shippedAvailable &&
             (shippedTitle || current.shipped > previousStats.shipped)) {
    startNotification(NotificationKind::Shipped, shippedTitle, now);
  } else if (current.unreadAvailable &&
             (messageTitle || current.unread > previousStats.unread)) {
    for (uint8_t index = 0; index < current.threadCount; ++index) {
      const AmpThreadSummary& thread = current.threads[index];
      const AmpThreadSummary* previous = findPreviousThread(thread);
      if (!messageTitle && thread.unread && !previous) {
        messageTitle = thread.title;
        break;
      }
    }
    startNotification(NotificationKind::Message, messageTitle, now);
  } else if (activeTitle || current.working > previousStats.working) {
    for (uint8_t index = 0; index < current.threadCount; ++index) {
      const AmpThreadSummary& thread = current.threads[index];
      const AmpThreadSummary* previous = findPreviousThread(thread);
      if (!activeTitle && stateIsWorking(thread.state) && !previous) {
        activeTitle = thread.title;
        break;
      }
    }
    startNotification(NotificationKind::ThreadActive, activeTitle, now);
  } else if (current.working == 0 && current.needsAttention == 0 &&
             current.unread == 0 &&
             (previousStats.working > 0 || previousStats.needsAttention > 0 ||
              previousStats.unread > 0)) {
    reelAllClearStartedAt = now;
    Serial.println("Notification: all clear");
  }
  previousStats = current;
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
    return "AWAITING APPROVAL";
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

void threadStatusLabel(const AmpThreadSummary& thread, char* label,
                       size_t labelSize) {
  const char* state = threadStateLabel(thread.state);
  if (thread.shipping && std::strcmp(thread.state, "idle") != 0) {
    std::snprintf(label, labelSize, "%s + SHIPPING", state);
  } else {
    std::snprintf(label, labelSize, "%s",
                  thread.shipping ? "SHIPPING" : state);
  }
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
                                    ? "CONNECTING"
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
    char stateLabel[32];
    threadStatusLabel(thread, stateLabel, sizeof(stateLabel));
    const bool active = std::strcmp(thread.state, "idle") != 0;
    canvas.setTextColor(stateNeedsAttention(thread.state)
                            ? accentColor
                            : (thread.shipping || active ? logoHighlightColor
                                                         : eyeColor));
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
  constexpr size_t maxCharacters = 22;
  const size_t length = std::strlen(title);
  if (length <= maxCharacters) {
    std::snprintf(first, maxCharacters + 1, "%s", title);
    second[0] = '\0';
    return;
  }
  size_t split = maxCharacters;
  while (split > 12 && title[split] != ' ') {
    --split;
  }
  if (split <= 12) {
    split = maxCharacters;
  }
  std::snprintf(first, maxCharacters + 1, "%.*s",
                static_cast<int>(split), title);
  const char* remainder = title + split;
  while (*remainder == ' ') {
    ++remainder;
  }
  copyEllipsized(second, maxCharacters + 1, remainder, maxCharacters);
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

  char stateLabel[32];
  threadStatusLabel(detailThread, stateLabel, sizeof(stateLabel));
  const uint16_t stateColor = stateNeedsAttention(detailThread.state)
                                  ? accentColor
                                  : (detailThread.shipping
                                         ? logoHighlightColor
                                         : std::strcmp(detailThread.state,
                                                       "idle") == 0
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


// Live state drives the selected design by default. The optional scripted
// comparison plays the same 25 s lifecycle in every mode: idle -> thread
// active -> new message -> needs attention -> all clear.

enum class ReelPhase : uint8_t {
  Idle,
  Working,
  Message,
  Attention,
  Resolved,
  Shipped,
};

constexpr uint32_t REEL_IDLE_MS = 6000;
constexpr uint32_t REEL_WORKING_MS = 5000;
constexpr uint32_t REEL_MESSAGE_MS = 5000;
constexpr uint32_t REEL_ATTENTION_MS = 5000;
constexpr uint32_t REEL_RESOLVED_MS = 4000;
constexpr uint32_t REEL_CYCLE_MS = REEL_IDLE_MS + REEL_WORKING_MS +
                                   REEL_MESSAGE_MS + REEL_ATTENTION_MS +
                                   REEL_RESOLVED_MS;

struct ReelScene {
  ReelPhase phase = ReelPhase::Idle;
  uint32_t phaseElapsed = 0;
  float intro = 0.0F;  // eased 0..1 over the first 600 ms of the phase
  float beat = 0.0F;   // shared pulse for urgent accents
  AmpStatsSnapshot stats;
  char eventTitle[AMP_THREAD_TITLE_LENGTH] = "";
  char eventProject[AMP_THREAD_PROJECT_LENGTH] = "";
};

uint16_t reelGreenColor() { return display.color565(104, 180, 111); }

void setReelThread(AmpThreadSummary& thread, const char* title,
                   const char* project, const char* state, bool unread) {
  std::snprintf(thread.title, sizeof(thread.title), "%s", title);
  std::snprintf(thread.project, sizeof(thread.project), "%s", project);
  std::snprintf(thread.state, sizeof(thread.state), "%s", state);
  thread.unread = unread;
}

ReelScene reelScene(uint32_t elapsed) {
  ReelScene scene;
  uint32_t t = elapsed % REEL_CYCLE_MS;
  if (t < REEL_IDLE_MS) {
    scene.phase = ReelPhase::Idle;
  } else if ((t -= REEL_IDLE_MS) < REEL_WORKING_MS) {
    scene.phase = ReelPhase::Working;
  } else if ((t -= REEL_WORKING_MS) < REEL_MESSAGE_MS) {
    scene.phase = ReelPhase::Message;
  } else if ((t -= REEL_MESSAGE_MS) < REEL_ATTENTION_MS) {
    scene.phase = ReelPhase::Attention;
  } else {
    t -= REEL_ATTENTION_MS;
    scene.phase = ReelPhase::Resolved;
  }
  scene.phaseElapsed = t;
  scene.intro = smoothStep(t / 600.0F);
  scene.beat = std::pow(std::max(0.0F, std::sin(t * 0.0105F)), 5.0F);

  AmpStatsSnapshot& stats = scene.stats;
  stats.configured = true;
  stats.wifiConnected = true;
  stats.available = true;
  stats.initialAttemptComplete = true;
  stats.attentionAvailable = true;
  stats.unreadAvailable = true;
  stats.total = 4;
  stats.threadCount = 3;
  setReelThread(stats.threads[0], "Redesign notification pipeline",
                "pocketpuck", "idle", false);
  setReelThread(stats.threads[1], "Review auth integration test", "amp",
                "idle", false);
  setReelThread(stats.threads[2], "Main screen design reel", "pocketpuck",
                "idle", false);
  switch (scene.phase) {
    case ReelPhase::Idle:
      stats.idle = 4;
      break;
    case ReelPhase::Working:
      stats.working = 2;
      stats.idle = 2;
      std::snprintf(stats.threads[0].state, sizeof(stats.threads[0].state),
                    "%s", "running_tools");
      std::snprintf(stats.threads[1].state, sizeof(stats.threads[1].state),
                    "%s", "working");
      std::snprintf(scene.eventTitle, sizeof(scene.eventTitle), "%s",
                    "Redesign notification pipeline");
      std::snprintf(scene.eventProject, sizeof(scene.eventProject), "%s",
                    "pocketpuck");
      break;
    case ReelPhase::Message:
      stats.working = 2;
      stats.idle = 2;
      stats.unread = 1;
      std::snprintf(stats.threads[0].state, sizeof(stats.threads[0].state),
                    "%s", "running_tools");
      std::snprintf(stats.threads[1].state, sizeof(stats.threads[1].state),
                    "%s", "working");
      stats.threads[0].unread = true;
      std::snprintf(scene.eventTitle, sizeof(scene.eventTitle), "%s",
                    "Redesign notification pipeline");
      std::snprintf(scene.eventProject, sizeof(scene.eventProject), "%s",
                    "pocketpuck");
      break;
    case ReelPhase::Attention:
      stats.working = 1;
      stats.needsAttention = 2;
      stats.unread = 1;
      stats.idle = 1;
      std::snprintf(stats.threads[0].state, sizeof(stats.threads[0].state),
                    "%s", "running_tools");
      std::snprintf(stats.threads[1].state, sizeof(stats.threads[1].state),
                    "%s", "awaiting_approval");
      std::snprintf(stats.threads[2].state, sizeof(stats.threads[2].state),
                    "%s", "error");
      stats.threads[0].unread = true;
      std::snprintf(scene.eventTitle, sizeof(scene.eventTitle), "%s",
                    "Review auth integration test");
      std::snprintf(scene.eventProject, sizeof(scene.eventProject), "%s",
                    "amp");
      break;
    case ReelPhase::Resolved:
      stats.idle = 4;
      break;
  }
  return scene;
}

uint32_t debugFaceElapsed(uint32_t now) {
  uint32_t phaseStart = 0;
  uint32_t phaseDuration = REEL_IDLE_MS;
  switch (debugFacePhase) {
    case 0:
      break;
    case 1:
      phaseStart = REEL_IDLE_MS;
      phaseDuration = REEL_WORKING_MS;
      break;
    case 2:
      phaseStart = REEL_IDLE_MS + REEL_WORKING_MS;
      phaseDuration = REEL_MESSAGE_MS;
      break;
    case 3:
      phaseStart = REEL_IDLE_MS + REEL_WORKING_MS + REEL_MESSAGE_MS;
      phaseDuration = REEL_ATTENTION_MS;
      break;
    default:
      phaseStart = REEL_IDLE_MS + REEL_WORKING_MS + REEL_MESSAGE_MS +
                   REEL_ATTENTION_MS;
      phaseDuration = REEL_RESOLVED_MS;
      break;
  }
  return phaseStart +
         std::min(now - debugFacePhaseChangedAt, phaseDuration - 1);
}

const AmpThreadSummary* reelThreadForPhase(const AmpStatsSnapshot& stats,
                                           ReelPhase phase) {
  if (phase == ReelPhase::Working && stats.shipping > 0) {
    for (uint8_t index = 0; index < stats.threadCount; ++index) {
      if (stats.threads[index].shipping) {
        return &stats.threads[index];
      }
    }
  }
  for (uint8_t index = 0; index < stats.threadCount; ++index) {
    const AmpThreadSummary& thread = stats.threads[index];
    if ((phase == ReelPhase::Attention && stateNeedsAttention(thread.state)) ||
        (phase == ReelPhase::Message && thread.unread) ||
        (phase == ReelPhase::Working && stateIsWorking(thread.state))) {
      return &thread;
    }
  }
  return nullptr;
}

ReelScene liveReelScene(uint32_t now) {
  ReelScene scene;
  scene.stats = getAmpStats();

  const uint32_t notificationElapsed = now - notificationStartedAt;
  if (notificationKind == NotificationKind::Shipped &&
      notificationElapsed < SHIPPED_NOTIFICATION_DURATION_MS) {
    scene.phase = ReelPhase::Shipped;
    scene.phaseElapsed = notificationElapsed;
    std::snprintf(scene.eventTitle, sizeof(scene.eventTitle), "%s",
                  notificationThreadTitle);
  } else if (scene.stats.available && scene.stats.working == 0 &&
      scene.stats.needsAttention == 0 && scene.stats.unread == 0 &&
      reelAllClearStartedAt != 0 &&
      now - reelAllClearStartedAt < REEL_RESOLVED_MS) {
    scene.phase = ReelPhase::Resolved;
    scene.phaseElapsed = now - reelAllClearStartedAt;
  } else if (notificationKind != NotificationKind::None &&
             notificationElapsed < NOTIFICATION_DURATION_MS) {
    if (notificationKind == NotificationKind::Attention) {
      scene.phase = ReelPhase::Attention;
    } else if (notificationKind == NotificationKind::Message) {
      scene.phase = ReelPhase::Message;
    } else {
      scene.phase = ReelPhase::Working;
    }
    scene.phaseElapsed = notificationElapsed;
    std::snprintf(scene.eventTitle, sizeof(scene.eventTitle), "%s",
                  notificationThreadTitle);
  } else if (scene.stats.available && scene.stats.needsAttention > 0) {
    scene.phase = ReelPhase::Attention;
    scene.phaseElapsed = NOTIFICATION_DURATION_MS + 200 + now;
  } else if (scene.stats.available && scene.stats.shipping > 0) {
    scene.phase = ReelPhase::Working;
    scene.phaseElapsed = NOTIFICATION_DURATION_MS + 200 + now;
  } else if (scene.stats.available && scene.stats.unread > 0) {
    scene.phase = ReelPhase::Message;
    scene.phaseElapsed = NOTIFICATION_DURATION_MS + 200 + now;
  } else if (scene.stats.available && scene.stats.working > 0) {
    scene.phase = ReelPhase::Working;
    scene.phaseElapsed = NOTIFICATION_DURATION_MS + 200 + now;
  } else {
    scene.phase = ReelPhase::Idle;
    scene.phaseElapsed = now;
  }

  const AmpThreadSummary* thread = nullptr;
  if (scene.eventTitle[0]) {
    for (uint8_t index = 0; index < scene.stats.threadCount; ++index) {
      if (std::strcmp(scene.stats.threads[index].title, scene.eventTitle) == 0) {
        thread = &scene.stats.threads[index];
        break;
      }
    }
  }
  if (!thread) {
    thread = reelThreadForPhase(scene.stats, scene.phase);
  }
  if (thread) {
    if (!scene.eventTitle[0]) {
      std::snprintf(scene.eventTitle, sizeof(scene.eventTitle), "%s",
                    thread->title);
    }
    if (thread->project[0]) {
      std::snprintf(scene.eventProject, sizeof(scene.eventProject), "%s",
                    thread->project);
    }
  } else if (!scene.eventTitle[0] && scene.phase != ReelPhase::Idle &&
             scene.phase != ReelPhase::Resolved) {
    std::snprintf(scene.eventTitle, sizeof(scene.eventTitle), "%s",
                  "Amp thread");
  }
  scene.intro = smoothStep(scene.phaseElapsed / 600.0F);
  scene.beat = std::pow(
      std::max(0.0F, std::sin(scene.phaseElapsed * 0.0105F)), 5.0F);
  return scene;
}

struct ReelStatus {
  char text[32];
  uint16_t color;
};

ReelStatus reelStatus(const ReelScene& scene) {
  ReelStatus status;
  switch (scene.phase) {
    case ReelPhase::Working:
      if (scene.stats.shipping > 0 && scene.stats.working > 0) {
        std::snprintf(status.text, sizeof(status.text),
                      "%u WORKING + %u SHIPPING", scene.stats.working,
                      scene.stats.shipping);
      } else if (scene.stats.shipping > 0) {
        std::snprintf(status.text, sizeof(status.text), "%u SHIPPING",
                      scene.stats.shipping);
      } else {
        std::snprintf(status.text, sizeof(status.text), "%u WORKING",
                      scene.stats.working);
      }
      status.color = reelGreenColor();
      break;
    case ReelPhase::Message:
      std::snprintf(status.text, sizeof(status.text), "%u NEW MESSAGE%s",
                    scene.stats.unread,
                    scene.stats.unread == 1 ? "" : "S");
      status.color = unreadColor;
      break;
    case ReelPhase::Attention:
      std::snprintf(status.text, sizeof(status.text), "%u NEED%s ATTENTION",
                    scene.stats.needsAttention,
                    scene.stats.needsAttention == 1 ? "S" : "");
      status.color = accentColor;
      break;
    case ReelPhase::Resolved:
      std::snprintf(status.text, sizeof(status.text), "%s", "ALL CLEAR");
      status.color = reelGreenColor();
      break;
    case ReelPhase::Shipped:
      std::snprintf(status.text, sizeof(status.text), "%s", "SHIPPED");
      status.color = reelGreenColor();
      break;
    default:
      std::snprintf(status.text, sizeof(status.text), "%s", "ALL QUIET");
      status.color = eyeColor;
      break;
  }
  return status;
}

float bottomRowGlance(uint32_t elapsed) {
  constexpr uint32_t cycleMs = 7000;
  constexpr uint32_t glanceStartMs = 1800;
  constexpr uint32_t glanceInMs = 450;
  constexpr uint32_t glanceHoldMs = 650;
  constexpr uint32_t glanceOutMs = 550;
  const uint32_t phase = elapsed % cycleMs;
  if (phase < glanceStartMs) {
    return 0.0F;
  }
  if (phase < glanceStartMs + glanceInMs) {
    return smoothStep(static_cast<float>(phase - glanceStartMs) / glanceInMs);
  }
  if (phase < glanceStartMs + glanceInMs + glanceHoldMs) {
    return 1.0F;
  }
  if (phase < glanceStartMs + glanceInMs + glanceHoldMs + glanceOutMs) {
    const uint32_t glanceOutElapsed =
        phase - glanceStartMs - glanceInMs - glanceHoldMs;
    return 1.0F -
           smoothStep(static_cast<float>(glanceOutElapsed) / glanceOutMs);
  }
  return 0.0F;
}

FacePose reelPose(const ReelScene& scene) {
  const uint32_t t = scene.phaseElapsed;
  FacePose pose = neutralPose();
  switch (scene.phase) {
    case ReelPhase::Idle: {
      constexpr uint32_t distractedCycleMs = 31000;
      constexpr uint32_t distractedStartMs = 19000;
      constexpr uint32_t distractedInMs = 1200;
      constexpr uint32_t distractedHoldMs = 2600;
      constexpr uint32_t distractedOutMs = 1500;
      const uint32_t distractedElapsed = t % distractedCycleMs;
      float distracted = 0.0F;
      if (distractedElapsed >= distractedStartMs &&
          distractedElapsed < distractedStartMs + distractedInMs) {
        distracted = smoothStep(
            static_cast<float>(distractedElapsed - distractedStartMs) /
            distractedInMs);
      } else if (distractedElapsed < distractedStartMs + distractedInMs +
                                             distractedHoldMs &&
                 distractedElapsed >= distractedStartMs + distractedInMs) {
        distracted = 1.0F;
      } else if (distractedElapsed < distractedStartMs + distractedInMs +
                                             distractedHoldMs +
                                             distractedOutMs &&
                 distractedElapsed >= distractedStartMs + distractedInMs +
                                          distractedHoldMs) {
        distracted = 1.0F - smoothStep(static_cast<float>(
                                             distractedElapsed -
                                             distractedStartMs -
                                             distractedInMs -
                                             distractedHoldMs) /
                                         distractedOutMs);
      }
      pose.gazeX = std::sin(t * 0.00024F) * 0.55F;
      pose.gazeY = std::sin(t * 0.00018F + 1.2F) * 0.12F;
      pose.gazeX = mix(pose.gazeX, 0.0F, distracted);
      pose.gazeY = mix(pose.gazeY, -0.06F, distracted);
      pose.eyeScale = mix(1.0F, 1.16F, distracted);
      pose.pupilScale = mix(1.0F, 0.58F, distracted);
      pose.leftEyeOpen = blinkAt(t % 6000, 2600, 280);
      pose.leftEyeOpen = mix(pose.leftEyeOpen, 1.0F, distracted);
      pose.rightEyeOpen = pose.leftEyeOpen;
      break;
    }
    case ReelPhase::Working:
      pose.gazeX = mix(0.0F, 0.25F, scene.intro) +
                   std::sin(t * 0.0008F) * 0.28F * scene.intro;
      pose.gazeY = mix(0.0F, -0.28F, scene.intro) +
                   std::cos(t * 0.0007F) * 0.08F * scene.intro;
      pose.leftEyeOpen = blinkAt(t % 5000, 4200, 300);
      pose.rightEyeOpen = pose.leftEyeOpen;
      pose.mouthCurveY = 148.0F;
      break;
    case ReelPhase::Message:
      pose.eyeScale = mix(1.0F, 1.08F, scene.intro);
      pose.pupilScale = mix(1.0F, 0.9F, scene.intro);
      pose.gazeX = std::sin(t * 0.0008F) * 0.16F * scene.intro;
      pose.gazeY = -0.25F * scene.intro;
      pose.mouthCurveY = mix(149.0F, 155.0F, scene.intro);
      break;
    case ReelPhase::Attention:
      pose.eyeScale = mix(1.0F, 1.14F, scene.intro);
      pose.pupilScale = mix(1.0F, 0.8F, scene.intro);
      pose.gazeX = std::sin(t * 0.004F) * 0.12F * scene.intro;
      pose.gazeY = -0.2F * scene.intro;
      pose.mouthCurveY = mix(149.0F, 157.0F, scene.intro);
      break;
    case ReelPhase::Resolved:
      pose.leftEyeOpen = mix(1.0F, 0.86F, scene.intro);
      pose.rightEyeOpen = pose.leftEyeOpen;
      pose.mouthCurveY = 143.0F;
      break;
    case ReelPhase::Shipped:
      pose.leftEyeOpen = mix(1.0F, 0.9F, scene.intro);
      pose.rightEyeOpen = pose.leftEyeOpen;
      pose.mouthCurveY = mix(149.0F, 141.0F, scene.intro);
      break;
  }
  if (scene.phase == ReelPhase::Working || scene.phase == ReelPhase::Message) {
    const float glance = bottomRowGlance(t) * scene.intro;
    pose.gazeX = mix(pose.gazeX, 0.0F, glance * 0.65F);
    pose.gazeY = mix(pose.gazeY, 0.72F, glance);
  }
  if (scene.stats.configured && scene.stats.wifiConnected &&
      !scene.stats.available) {
    pose.xOffset += std::sin(t * 0.03F) * 3.0F;
  }
  return pose;
}

uint16_t reelPhaseColor(const ReelScene& scene) {
  return reelStatus(scene).color;
}

const char* reelProject(const ReelScene& scene) {
  return scene.eventProject[0] ? scene.eventProject : "AMP";
}

const AmpThreadSummary* reelAttentionThread(const ReelScene& scene) {
  if (scene.eventTitle[0]) {
    for (uint8_t index = 0; index < scene.stats.threadCount; ++index) {
      const AmpThreadSummary& thread = scene.stats.threads[index];
      if (stateNeedsAttention(thread.state) &&
          std::strcmp(thread.title, scene.eventTitle) == 0) {
        return &thread;
      }
    }
  }
  return reelThreadForPhase(scene.stats, ReelPhase::Attention);
}

const char* reelDetail(const ReelScene& scene) {
  switch (scene.phase) {
    case ReelPhase::Working:
      return scene.stats.shipping > 0 && scene.stats.working > 0
                 ? "Working and shipping"
             : scene.stats.shipping > 0 ? "Ship in progress"
                                        : "Thread is working";
    case ReelPhase::Message:
      return "New message in thread";
    case ReelPhase::Attention: {
      const AmpThreadSummary* thread = reelAttentionThread(scene);
      if (thread && std::strcmp(thread->state, "awaiting_approval") == 0) {
        return "Waiting for approval";
      }
      if (thread && std::strcmp(thread->state, "error") == 0) {
        return "Thread encountered an error";
      }
      return "Action required";
    }
    case ReelPhase::Resolved:
      return "Nothing needs attention";
    case ReelPhase::Shipped:
      return "Ship completed";
    default:
      return "No active notifications";
  }
}

void drawDesignMinimal(const ReelScene& scene) {
  FacePose pose = reelPose(scene);
  const bool persistentAttention =
      scene.phase == ReelPhase::Attention &&
      scene.phaseElapsed >= NOTIFICATION_DURATION_MS;
  if (persistentAttention) {
    pose.xOffset =
        std::sin(scene.phaseElapsed * 0.09F) * scene.beat * 5.0F;
    pose.eyeScale += 0.28F;
    pose.pupilScale -= 0.22F;
    pose.mouthCurveY += 10.0F;
  }
  drawFace(pose);
  if (scene.phase == ReelPhase::Idle) {
    const uint32_t idleElapsed = millis() - idleMessageCycleStartedAt;
    if (idleMessageCycleStartedAt != 0 &&
        idleElapsed >= IDLE_MESSAGE_DELAY_MS) {
      const uint32_t messageElapsed = idleElapsed - IDLE_MESSAGE_DELAY_MS;
      if (messageElapsed % IDLE_MESSAGE_INTERVAL_MS <
          IDLE_MESSAGE_DURATION_MS) {
        constexpr size_t messageCount =
            sizeof(IDLE_MESSAGES) / sizeof(IDLE_MESSAGES[0]);
        const size_t messageIndex =
            (messageElapsed / IDLE_MESSAGE_INTERVAL_MS) % messageCount;
        drawCenteredText(IDLE_MESSAGES[messageIndex], 210, 2, 0xFFFF);
      }
    }
    return;
  }
  if (scene.phase == ReelPhase::Resolved) {
    return;
  }
  const ReelStatus status = reelStatus(scene);
  uint16_t textWidth;
  uint16_t textHeight;
  selectedTextDimensions(status.text, 2, textWidth, textHeight);
  const int16_t groupWidth = textWidth + 18;
  const int16_t x = (SCREEN_WIDTH - groupWidth) / 2;
  constexpr int16_t statusY = 210;
  canvas.fillCircle(x + 5, statusY + textHeight / 2, 5, status.color);
  drawSelectedText(status.text, x + 18, statusY, 2, status.color);
  if (scene.phase == ReelPhase::Attention) {
    const int16_t edge = 3 + std::lround(scene.beat * 5.0F);
    canvas.fillRect(0, 0, SCREEN_WIDTH, edge, accentColor);
    canvas.fillRect(0, SCREEN_HEIGHT - edge, SCREEN_WIDTH, edge, accentColor);
    canvas.fillRect(0, 0, edge, SCREEN_HEIGHT, accentColor);
    canvas.fillRect(SCREEN_WIDTH - edge, 0, edge, SCREEN_HEIGHT, accentColor);
  }
}

float reelEventVisibility(const ReelScene& scene) {
  if (scene.phase == ReelPhase::Idle || scene.phase == ReelPhase::Resolved) {
    return 0.0F;
  }
  float visibility = smoothStep(scene.phaseElapsed / 480.0F);
  if (scene.phaseElapsed > 3400) {
    visibility *=
        1.0F - smoothStep((scene.phaseElapsed - 3400) / 700.0F);
  }
  return visibility;
}

float reelEventExit(const ReelScene& scene) {
  return scene.phaseElapsed > 3400
             ? smoothStep((scene.phaseElapsed - 3400) / 700.0F)
             : 0.0F;
}

uint32_t reelHash(uint32_t value);
float reelCardSettle(uint32_t elapsed);

const char* reelEventHeadline(const ReelScene& scene) {
  switch (scene.phase) {
    case ReelPhase::Working:
      return scene.stats.shipping > 0 && scene.stats.working > 0
                 ? "WORKING + SHIPPING"
             : scene.stats.shipping > 0 ? "SHIPPING"
                                        : "WORKING";
    case ReelPhase::Message:
      return "NEW MESSAGE";
    case ReelPhase::Attention:
      return "NEEDS ATTENTION";
    case ReelPhase::Resolved:
      return "ALL CLEAR";
    case ReelPhase::Shipped:
      return "SHIPPED";
    default:
      return "ALL QUIET";
  }
}

uint8_t reelHeadlineSize(const ReelScene& scene, uint8_t preferred = 4) {
  return fittedSelectedTextSize(reelEventHeadline(scene), preferred, 300);
}

void drawReelEventTitle(const ReelScene& scene, int16_t y, uint16_t color) {
  if (!scene.eventTitle[0]) {
    drawCenteredText(reelDetail(scene), y, 1, color);
    return;
  }
  char firstLine[25];
  char secondLine[25];
  splitTitle(scene.eventTitle, firstLine, secondLine);
  drawCenteredText(firstLine, y, 2, color);
  if (secondLine[0]) {
    drawCenteredText(secondLine, y + 22, 2, color);
  }
}

void drawShippedTitle(const char* title, int16_t y, uint16_t color,
                      uint16_t bodyColor) {
  drawCenteredText("SHIPPED!", y, 5, color);
  char clipped[39];
  copyEllipsized(clipped, sizeof(clipped),
                 title && title[0] ? title : "Amp thread", 36);
  drawCenteredText(clipped, y + 58, 1, bodyColor);
}

void drawShippedLiftoff(uint32_t elapsed, const char* title) {
  const uint16_t sky = display.color565(7, 17, 31);
  const uint16_t flame = display.color565(255, 116, 43);
  const uint16_t hot = display.color565(255, 211, 92);
  const uint16_t smoke = display.color565(101, 119, 125);
  const uint16_t hull = display.color565(231, 231, 207);
  canvas.fillScreen(sky);

  for (uint8_t star = 0; star < 24; ++star) {
    const uint32_t h = reelHash(star + 700);
    const int16_t x = h % SCREEN_WIDTH;
    const int16_t y = (h >> 9) % 176;
    const uint8_t radius = ((elapsed / 180 + star) % 4 == 0) ? 2 : 1;
    canvas.fillCircle(x, y, radius, eyeColor);
  }

  const float launch = smoothStep((static_cast<float>(elapsed) - 350.0F) /
                                  2550.0F);
  const int16_t rocketY = std::lround(mix(154.0F, -82.0F, launch));
  const int16_t wobble = std::lround(std::sin(elapsed * 0.018F) * 2.0F);
  const int16_t rocketX = 160 + wobble;
  if (launch < 0.98F) {
    const int16_t flameLength = 24 + ((elapsed / 70) % 3) * 8;
    canvas.fillTriangle(rocketX - 12, rocketY + 61, rocketX + 12,
                        rocketY + 61, rocketX, rocketY + 61 + flameLength,
                        flame);
    canvas.fillTriangle(rocketX - 6, rocketY + 59, rocketX + 6,
                        rocketY + 59, rocketX, rocketY + 70 + flameLength / 2,
                        hot);
    canvas.fillRoundRect(rocketX - 18, rocketY + 12, 36, 50, 15, hull);
    canvas.fillTriangle(rocketX - 18, rocketY + 24, rocketX,
                        rocketY - 13, rocketX + 18, rocketY + 24, hull);
    canvas.fillTriangle(rocketX - 18, rocketY + 45, rocketX - 29,
                        rocketY + 65, rocketX - 16, rocketY + 61, flame);
    canvas.fillTriangle(rocketX + 18, rocketY + 45, rocketX + 29,
                        rocketY + 65, rocketX + 16, rocketY + 61, flame);
    canvas.fillCircle(rocketX, rocketY + 28, 8, unreadColor);
    canvas.drawCircle(rocketX, rocketY + 28, 9, sky);
  }

  for (uint8_t puff = 0; puff < 13; ++puff) {
    const uint32_t h = reelHash(puff + 760);
    const float age = clamp01((static_cast<float>(elapsed) - 300.0F -
                               puff * 45.0F) /
                              1050.0F);
    const int16_t x = 160 + static_cast<int16_t>(h % 67) - 33 +
                      std::lround(std::sin(puff * 2.1F) * age * 18.0F);
    const int16_t y = 219 - std::lround(age * 23.0F) + (h >> 8) % 13;
    canvas.fillCircle(x, y, 5 + std::lround(age * 9.0F),
                      puff % 2 ? smoke : faceShadowColor);
  }

  if (elapsed > 2450) {
    canvas.fillRoundRect(9, 12, 302, 99, 12, logoBackground);
    canvas.drawRoundRect(9, 12, 302, 99, 12, hot);
    drawShippedTitle(title, 18, hot, eyeColor);
  }
  canvas.fillRect(0, 228, SCREEN_WIDTH, 12, flame);
  canvas.fillRect(0, 232, SCREEN_WIDTH, 8, hot);
}

// KNOCK — a solid notification barges in from the side and physically bumps
// Puck away. Attention knocks repeatedly; quieter events settle once.
void drawDesignKnock(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  if (scene.phase == ReelPhase::Working || show < 0.01F) {
    drawDesignMinimal(scene);
    return;
  }
  const float exit = reelEventExit(scene);
  const float impact = reelCardSettle(scene.phaseElapsed);
  const int16_t repeat = scene.phase == ReelPhase::Attention
                             ? std::lround(scene.beat * 8.0F)
                             : 0;
  FacePose pose = reelPose(scene);
  pose.xOffset += (scene.phase == ReelPhase::Attention ? 74.0F : 48.0F) * show +
                  repeat;
  pose.gazeX = -0.9F * show;
  pose.eyeScale += 0.1F * show;
  drawFace(pose);
  const uint16_t color = reelPhaseColor(scene);
  const int16_t x =
      std::lround(mix(-294.0F, -9.0F, clamp01(impact) * show)) - repeat;
  canvas.fillRoundRect(x, 20, 230, 199, 13, color);
  canvas.fillRect(x + 18, 20, 5, 199, logoBackground);
  if (show < 0.7F && exit == 0.0F) {
    return;
  }
  canvas.setFont();
  canvas.setTextSize(1);
  canvas.setTextColor(logoBackground);
  canvas.setCursor(x + 35, 38);
  canvas.print(reelProject(scene));
  drawSelectedText(reelEventHeadline(scene), x + 35, 63, 2,
                   logoBackground);
  char firstLine[25];
  char secondLine[25];
  splitTitle(scene.eventTitle, firstLine, secondLine);
  canvas.setCursor(x + 35, 103);
  canvas.print(firstLine);
  if (secondLine[0]) {
    canvas.setCursor(x + 35, 119);
    canvas.print(secondLine);
  }
  if (scene.phase == ReelPhase::Attention) {
    canvas.setCursor(x + 35, 153);
    canvas.print(reelDetail(scene));
    canvas.setCursor(x + 35, 188);
    canvas.print("OPEN AMP TO RESPOND");
  }
}

// BEACON — a graphic pulse radiates behind a central message capsule. Working
// threads use a compact signal display of their own so the face stays clear and
// no status text has to sit against the bottom edge of the screen.
void drawDesignBeacon(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  const uint16_t color = reelPhaseColor(scene);
  if (scene.phase == ReelPhase::Working) {
    if (show < 0.01F) {
      drawDesignMinimal(scene);
      return;
    }
    drawFace(reelPose(scene));

    // Three expanding, dotted ellipses read as a radio ping without drawing a
    // hard ring through Puck's eyes.
    for (uint8_t ring = 0; ring < 3; ++ring) {
      const float pulse = std::fmod(scene.phaseElapsed / 1500.0F +
                                       ring / 3.0F,
                                   1.0F);
      const float radiusX = mix(70.0F, 151.0F, pulse);
      const float radiusY = mix(45.0F, 107.0F, pulse);
      const uint16_t ringColor = pulse < 0.58F ? color : textureColor;
      for (uint8_t marker = 0; marker < 20; ++marker) {
        const float angle = FULL_ROTATION_RADIANS * marker / 20.0F;
        const int16_t x = std::lround(160.0F + std::cos(angle) * radiusX);
        const int16_t y = std::lround(116.0F + std::sin(angle) * radiusY);
        if (y > 48) {
          const uint16_t background =
              canvas.getBuffer()[y * SCREEN_WIDTH + x];
          const float inverse = 1.0F - show;
          const uint16_t fadedColor =
              (std::lround(((ringColor >> 11) & 0x1F) * show +
                           ((background >> 11) & 0x1F) * inverse)
               << 11) |
              (std::lround(((ringColor >> 5) & 0x3F) * show +
                           ((background >> 5) & 0x3F) * inverse)
               << 5) |
              std::lround((ringColor & 0x1F) * show +
                          (background & 0x1F) * inverse);
          canvas.fillCircle(x, y, marker % 5 == 0 ? 2 : 1, fadedColor);
        }
      }
    }

    const float exit = reelEventExit(scene);
    const int16_t panelWidth = std::lround(mix(96.0F, 300.0F, show));
    constexpr int16_t panelHeight = 46;
    const int16_t panelY = 7 - std::lround(exit * 62.0F);
    const int16_t panelX = (SCREEN_WIDTH - panelWidth) / 2;
    canvas.fillRoundRect(panelX, panelY, panelWidth, panelHeight, 8,
                         logoBackground);
    canvas.drawRoundRect(panelX, panelY, panelWidth, panelHeight, 8, color);
    if (show > 0.68F || exit > 0.0F) {
      drawCenteredText("THREAD ACTIVE", panelY + 6, 2, color);
      char title[44];
      copyEllipsized(title, sizeof(title), scene.eventTitle, 41);
      drawCenteredText(title, panelY + 32, 1, eyeColor);
    }
    return;
  }
  if (show < 0.01F) {
    drawDesignMinimal(scene);
    return;
  }
  drawFace(reelPose(scene));
  const float exit = reelEventExit(scene);
  const uint8_t rayCount = scene.phase == ReelPhase::Attention ? 16 : 8;
  for (uint8_t ray = 0; ray < rayCount; ++ray) {
    const float angle = FULL_ROTATION_RADIANS * ray / rayCount +
                        scene.phaseElapsed * 0.00035F;
    const float inner = 42.0F + scene.beat * 9.0F;
    const float outer = mix(45.0F, 195.0F, show);
    canvas.fillTriangle(160 + std::cos(angle - 0.05F) * inner,
                        116 + std::sin(angle - 0.05F) * inner,
                        160 + std::cos(angle) * outer,
                        116 + std::sin(angle) * outer,
                        160 + std::cos(angle + 0.05F) * inner,
                        116 + std::sin(angle + 0.05F) * inner, color);
  }
  const int16_t targetWidth = scene.phase == ReelPhase::Attention ? 292 : 268;
  const int16_t width = std::lround(targetWidth * scene.intro);
  const int16_t panelY = 28 - std::lround(exit * 220.0F);
  canvas.fillRoundRect((SCREEN_WIDTH - width) / 2, panelY, width, 172, 16,
                       logoBackground);
  if (show < 0.72F && exit == 0.0F) {
    return;
  }
  drawCenteredText(reelEventHeadline(scene), panelY + 15,
                   reelHeadlineSize(scene), color);
  drawReelEventTitle(scene, panelY + 57, eyeColor);
  if (scene.phase == ReelPhase::Attention) {
    drawCenteredText(reelDetail(scene), panelY + 115, 1, color);
    drawCenteredText("OPEN AMP TO RESPOND", panelY + 146, 1, eyeColor);
  }
  FacePose cameo = reelPose(scene);
  cameo.eyeScale = 0.5F;
  cameo.pupilScale = 0.65F;
  cameo.yOffset = 121.0F + panelY - 28;
  cameo.gazeY = -0.65F;
  drawEye(138, cameo.leftEyeOpen, cameo);
  drawEye(182, cameo.rightEyeOpen, cameo);
}

// PANIC — the intentionally excessive option. Puck's reaction is the hero;
// giant words pin the face between them while attention makes it shiver.
void drawDesignPanic(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  if (scene.phase == ReelPhase::Working || show < 0.01F) {
    drawDesignMinimal(scene);
    return;
  }
  const float exit = reelEventExit(scene);
  FacePose pose = reelPose(scene);
  const float shake = scene.phase == ReelPhase::Attention
                          ? std::sin(scene.phaseElapsed * 0.09F) * scene.beat * 5.0F
                          : 0.0F;
  pose.xOffset = shake;
  pose.eyeScale += 0.28F * show;
  pose.pupilScale -= 0.22F * show;
  pose.mouthCurveY += 10.0F * show;
  drawFace(pose);
  const uint16_t color = reelPhaseColor(scene);
  const int16_t top = std::lround(mix(-58.0F, 0.0F, show));
  canvas.fillRect(0, top, SCREEN_WIDTH, 58, color);
  const char* attentionTop = "ACTION";
  const char* attentionBottom = "REQUIRED";
  if (scene.phase == ReelPhase::Attention) {
    const AmpThreadSummary* thread = reelAttentionThread(scene);
    if (thread && std::strcmp(thread->state, "awaiting_approval") == 0) {
      attentionTop = "APPROVAL";
      attentionBottom = "NEEDED";
    } else if (thread && std::strcmp(thread->state, "error") == 0) {
      attentionTop = "ERROR";
      attentionBottom = "DETECTED";
    }
    drawCenteredText(attentionTop, top + 11, 4, logoBackground);
  } else {
    drawCenteredText(reelEventHeadline(scene), top + 11,
                     reelHeadlineSize(scene), logoBackground);
  }
  if (scene.phase == ReelPhase::Message) {
    const int16_t panelY = 172 + std::lround(exit * 80.0F);
    canvas.fillRoundRect(14, panelY, 292, 68, 8, logoBackground);
    canvas.drawRoundRect(14, panelY, 292, 68, 8, unreadColor);
    drawReelEventTitle(scene, panelY + 6, unreadColor);
    return;
  }
  const int16_t bottom = std::lround(mix(240.0F, 184.0F, show));
  canvas.fillRect(0, bottom, SCREEN_WIDTH, 56, color);
  if (scene.phase == ReelPhase::Attention) {
    drawCenteredText(attentionBottom, bottom + 10, 4, logoBackground);
  } else {
    const uint8_t detailSize = std::strlen(reelDetail(scene)) <= 24 ? 2 : 1;
    drawCenteredText(reelDetail(scene), bottom + (detailSize == 2 ? 13 : 18),
                     detailSize, logoBackground);
  }
  if ((show > 0.72F || exit > 0.0F) && scene.eventTitle[0]) {
    char title[37];
    copyEllipsized(title, sizeof(title), scene.eventTitle, 34);
    const int16_t titleY = 148 + std::lround(exit * 100.0F);
    canvas.fillRoundRect(30, titleY, 260, 24, 6, logoBackground);
    canvas.drawRoundRect(30, titleY, 260, 24, 6, color);
    drawCenteredText(title, titleY + 8, 1, color);
  }
}

float reelCardSettle(uint32_t elapsed) {
  const float t = elapsed;
  return 1.0F - std::exp(-t * 0.0065F) * std::cos(t * 0.013F);
}

uint32_t reelHash(uint32_t value) {
  value ^= value >> 16;
  value *= 2654435761U;
  value ^= value >> 13;
  return value;
}

void drawReelOverlay(uint32_t now) {
  if (debugFaceActive) {
    char label[32];
    std::snprintf(label, sizeof(label), "DEBUG FACE: %s",
                  DEBUG_FACE_PHASE_NAMES[debugFacePhase]);
    const int16_t width = std::strlen(label) * 6 + 12;
    canvas.fillRoundRect(4, 3, width, 18, 5, logoBackground);
    canvas.drawRoundRect(4, 3, width, 18, 5, textureColor);
    canvas.setFont();
    canvas.setTextSize(1);
    canvas.setTextColor(eyeColor);
    canvas.setCursor(10, 8);
    canvas.print(label);
    return;
  }
  if (now - reelModeChangedAt >= REEL_OVERLAY_MS) {
    return;
  }
  char label[24];
  std::snprintf(label, sizeof(label), "%u/%u %s",
                reelMode + 1, REEL_MODE_COUNT, REEL_MODE_NAMES[reelMode]);
  if (reelModeSelecting) {
    const int16_t width = std::strlen(label) * 6 + 12;
    canvas.fillRoundRect(4, 3, width, 18, 5, logoBackground);
    canvas.drawRoundRect(4, 3, width, 18, 5, textureColor);
    canvas.setFont();
    canvas.setTextSize(1);
    canvas.setTextColor(eyeColor);
    canvas.setCursor(10, 8);
    canvas.print(label);
    return;
  }
  const int16_t width = std::strlen(label) * 12 + 16;
  const int16_t x = (SCREEN_WIDTH - width) / 2;
  canvas.fillRoundRect(x, 44, width, 24, 6, logoBackground);
  canvas.drawRoundRect(x, 44, width, 24, 6, eyeColor);
  drawCenteredText(label, 48, 2, eyeColor);
}

void drawDesignReelFrame(uint32_t elapsed, uint32_t now) {
  const uint32_t reelElapsed =
      reelModeSelecting ? now - reelSelectionStartedAt : elapsed;
  const bool scripted = DESIGN_REEL_SCRIPTED || reelModeSelecting;
  const ReelScene scene = debugFaceActive
                              ? reelScene(debugFaceElapsed(now))
                              : (scripted ? reelScene(reelElapsed)
                                          : liveReelScene(now));
  const bool quietlyIdle = !debugFaceActive && !scripted &&
                           scene.phase == ReelPhase::Idle &&
                           scene.stats.available && scene.stats.total > 0;
  if (quietlyIdle) {
    if (idleMessageCycleStartedAt == 0) {
      idleMessageCycleStartedAt = now;
    }
  } else {
    idleMessageCycleStartedAt = 0;
  }
  if (scene.phase == ReelPhase::Shipped) {
    drawShippedLiftoff(scene.phaseElapsed, scene.eventTitle);
    pushCanvas();
    return;
  }
  if (!scripted &&
      (!scene.stats.available || scene.stats.total == 0)) {
    drawFace(reelPose(scene));
    drawStatsPanel(scene.stats);
    drawReelOverlay(now);
    pushCanvas();
    return;
  }
  switch (reelMode) {
    case 0:
      drawDesignMinimal(scene);
      break;
    case 1:
      drawDesignKnock(scene);
      break;
    case 2:
      drawDesignBeacon(scene);
      break;
    case 3:
      drawDesignPanic(scene);
      break;
    default:
      drawDesignMinimal(scene);
      break;
  }
  drawReelOverlay(now);
  pushCanvas();
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

  Preferences preferences;
  preferences.begin("pocketpuck", false);
  const uint8_t savedReelVersion = preferences.getUChar("designVer", 0);
  const uint8_t savedReelMode = preferences.getUChar("design", 0);
  blinkingDisabled = preferences.getBool("noBlink", false);
  reelMode = savedReelVersion == REEL_VERSION &&
                     savedReelMode < REEL_MODE_COUNT
                 ? savedReelMode
                 : 0;
  preferences.end();
  Serial.printf("Saved design: %u/%u %s\n", reelMode + 1, REEL_MODE_COUNT,
                REEL_MODE_NAMES[reelMode]);
  Serial.printf("Blinking: %s\n", blinkingDisabled ? "disabled" : "enabled");

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
  drawLogo(0);
  pushCanvas();
  lastFrameAt = millis();
}

void loop() {
  const uint32_t now = millis();
  updateControls(now);
  if (!ampStatsStarted && now - demoStartedAt >= STARTUP_LOGO_DURATION_MS) {
    beginAmpStats();
    ampStatsStarted = true;
    wifiConnectionStartedAt = now;
  }
  if (ampStatsStarted) {
    updateAmpStats(now);
  }
  updateNotifications(now);
  if (now - lastFrameAt < FRAME_INTERVAL_MS) {
    return;
  }

  lastFrameAt = now;
  const AmpStatsSnapshot stats = getAmpStats();
  if (!ampStatsStarted) {
    drawLogo(now - demoStartedAt);
    pushCanvas();
    return;
  }
  if (initialSetupCompletedAt == 0) {
    const uint32_t connectingElapsed = now - wifiConnectionStartedAt;
    if ((!stats.configured ||
         (!stats.wifiConnected && stats.initialAttemptComplete)) &&
        connectingElapsed >= CONNECTING_FACE_MIN_DURATION_MS) {
      initialSetupCompletedAt = now;
    } else {
      if (wakingAnimationStartedAt == 0 && stats.wifiConnected &&
          connectingElapsed >= CONNECTING_FACE_MIN_DURATION_MS) {
        wakingAnimationStartedAt = now;
      }
      if (wakingAnimationStartedAt == 0) {
        if (!connectingFaceDrawn) {
          AmpStatsSnapshot connectingStats = stats;
          connectingStats.configured = true;
          connectingStats.wifiConnected = false;
          FacePose sleepingPose = neutralPose();
          sleepingPose.leftEyeOpen = 0.0F;
          sleepingPose.rightEyeOpen = 0.0F;
          drawFace(sleepingPose);
          drawStatsPanel(connectingStats);
          pushCanvas();
          connectingFaceDrawn = true;
        }
        return;
      }

      const uint32_t wakingElapsed = now - wakingAnimationStartedAt;
      if (wakingElapsed < WAKING_ANIMATION_DURATION_MS) {
        drawFace(wakingPose(wakingElapsed));
        pushCanvas();
        return;
      }
      initialSetupCompletedAt = now;
    }
  }

  const uint32_t mainElapsed = now - initialSetupCompletedAt;
  if (uiPage == UiPage::MainMenu) {
    drawMainMenu();
    pushCanvas();
  } else if (uiPage == UiPage::Settings) {
    drawSettings(now);
    pushCanvas();
  } else if (uiPage == UiPage::ThreadDetail) {
    drawThreadDetail();
    pushCanvas();
  } else if (uiPage == UiPage::ThreadList) {
    drawThreadOverview();
    pushCanvas();
  } else {
    drawDesignReelFrame(mainElapsed, now);
  }
}
