#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <Preferences.h>
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
constexpr int16_t FACE_Y_OFFSET = 12;
constexpr uint8_t BACKLIGHT_MIN = 31;
constexpr uint8_t BACKLIGHT_STEP = 32;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t ENCODER_FEEDBACK_MS = 1500;
constexpr uint32_t LONG_PRESS_MS = 700;
constexpr uint32_t BROWSER_TIMEOUT_MS = 30000;
constexpr uint32_t NOTIFICATION_DURATION_MS = 4200;

constexpr uint32_t LOGO_ROTATION_MS = 5000;
constexpr uint32_t LOGO_HOLD_AFTER_SETUP_MS = 5000;
constexpr uint32_t WAKE_DURATION_MS = 2500;
constexpr uint32_t IDLE_DURATION_MS = 9000;
constexpr uint32_t BEWILDERED_DURATION_MS = 5000;
constexpr uint32_t THINKING_DURATION_MS = 6000;
constexpr uint32_t QUIET_SURPRISE_DURATION_MS = 2500;
constexpr uint32_t WORKED_DURATION_MS = 4000;
constexpr uint32_t DEMO_DURATION_MS =
    WAKE_DURATION_MS + IDLE_DURATION_MS + BEWILDERED_DURATION_MS +
    THINKING_DURATION_MS + QUIET_SURPRISE_DURATION_MS + WORKED_DURATION_MS;

// =================== DESIGN REEL (temporary experiment) ====================
// Confirmed designs render live Amp state. Holding the dial button opens a
// picker that automatically runs the synchronized fixture lifecycle while the
// dial previews each design. The optional scripted toggle runs that lifecycle
// outside the picker too. To revert completely, set DESIGN_REEL_ENABLED to
// false or delete the blocks between the "DESIGN REEL" markers.
constexpr bool DESIGN_REEL_ENABLED = true;
constexpr bool DESIGN_REEL_SCRIPTED = false;
constexpr uint8_t REEL_VERSION = 5;
constexpr uint8_t REEL_MODE_COUNT = 4;
constexpr uint8_t FONT_COUNT = 4;
constexpr uint32_t REEL_OVERLAY_MS = 1800;
const char* const REEL_MODE_NAMES[REEL_MODE_COUNT] = {
    "MINIMAL", "KNOCK", "BEACON", "PANIC",
};
const char* const FONT_NAMES[FONT_COUNT] = {
    "CLASSIC", "MONO", "SANS", "SERIF",
};
// ================== END DESIGN REEL constants =============================

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

// DESIGN REEL state (delete with the reel).
uint8_t reelMode = 0;
uint8_t reelModeBeforeSelection = 0;
uint32_t reelModeChangedAt = 0;
bool reelModeSelecting = false;
uint32_t reelSelectionStartedAt = 0;
uint8_t selectedFont = 0;
uint8_t fontBeforeSelection = 0;
bool fontSelecting = false;

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
  reelModeSelecting = false;
  fontSelecting = false;
  encoderFeedbackActive = false;
  Serial.println("Screen: face");
}

void showMainMenu(uint32_t now) {
  if (reelModeSelecting) {
    reelMode = reelModeBeforeSelection;
    reelModeSelecting = false;
  }
  if (fontSelecting) {
    selectedFont = fontBeforeSelection;
    fontSelecting = false;
  }
  uiPage = UiPage::MainMenu;
  mainMenuIndex = 0;
  lastBrowserInteractionAt = now;
  encoderFeedbackActive = false;
  Serial.println("Screen: main menu");
}

void showSettings(uint32_t now) {
  uiPage = UiPage::Settings;
  settingsMenuIndex = 0;
  lastBrowserInteractionAt = now;
  encoderFeedbackActive = false;
  Serial.println("Screen: settings");
}

void showFacePicker(uint32_t now) {
  uiPage = UiPage::Face;
  reelModeBeforeSelection = reelMode;
  reelModeSelecting = true;
  reelModeChangedAt = now;
  reelSelectionStartedAt = now;
  encoderFeedbackActive = false;
  Serial.println("Face picker: turn to preview, click to confirm");
}

void showFontPicker(uint32_t now) {
  uiPage = UiPage::Face;
  fontBeforeSelection = selectedFont;
  fontSelecting = true;
  reelModeChangedAt = now;
  reelSelectionStartedAt = now;
  encoderFeedbackActive = false;
  Serial.println("Font picker: turn to preview, click to confirm");
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

void saveSelectedFont() {
  Preferences preferences;
  preferences.begin("pocketpuck", false);
  preferences.putUChar("font", selectedFont);
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
  selectedFont = 0;
  blinkingDisabled = false;
  reelModeChangedAt = now;
  settingsResetAt = now;
  Serial.println("Settings reset to defaults");
}

void handleShortPress(uint32_t now) {
  if (uiPage == UiPage::Face && reelModeSelecting) {
    reelModeSelecting = false;
    reelModeChangedAt = now;
    saveSelectedFace();
    Serial.printf("Design confirmed: %u/%u %s\n", reelMode + 1,
                  REEL_MODE_COUNT, REEL_MODE_NAMES[reelMode]);
  } else if (uiPage == UiPage::Face && fontSelecting) {
    fontSelecting = false;
    reelModeChangedAt = now;
    saveSelectedFont();
    Serial.printf("Font confirmed: %u/%u %s\n", selectedFont + 1,
                  FONT_COUNT, FONT_NAMES[selectedFont]);
  } else if (uiPage == UiPage::MainMenu) {
    if (mainMenuIndex == 0) {
      showFacePicker(now);
    } else if (mainMenuIndex == 1) {
      showFontPicker(now);
    } else {
      showSettings(now);
    }
  } else if (uiPage == UiPage::Settings) {
    lastBrowserInteractionAt = now;
    if (settingsMenuIndex == 0) {
      blinkingDisabled = !blinkingDisabled;
      saveBlinkingPreference();
      Serial.printf("Blinking: %s\n", blinkingDisabled ? "disabled" : "enabled");
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
          std::max<int16_t>(0, std::min<int16_t>(2, next)));
      lastBrowserInteractionAt = now;
    } else if (uiPage == UiPage::Settings) {
      const int16_t next =
          static_cast<int16_t>(settingsMenuIndex) + encoderSteps;
      settingsMenuIndex = static_cast<uint8_t>(
          std::max<int16_t>(0, std::min<int16_t>(1, next)));
      lastBrowserInteractionAt = now;
    } else if (uiPage == UiPage::ThreadList) {
      navigateThreads(encoderSteps, now, false);
    } else if (uiPage == UiPage::ThreadDetail) {
      navigateThreads(encoderSteps, now, true);
    } else if (DESIGN_REEL_ENABLED && reelModeSelecting) {
      // DESIGN REEL: only cycle concepts inside the deliberate mode picker.
      const int16_t next =
          (static_cast<int16_t>(reelMode) + encoderSteps) % REEL_MODE_COUNT;
      reelMode = static_cast<uint8_t>((next + REEL_MODE_COUNT) %
                                      REEL_MODE_COUNT);
      reelModeChangedAt = now;
      Serial.printf("Design preview: %u/%u %s\n", reelMode + 1,
                    REEL_MODE_COUNT, REEL_MODE_NAMES[reelMode]);
    } else if (fontSelecting) {
      const int16_t next =
          (static_cast<int16_t>(selectedFont) + encoderSteps) % FONT_COUNT;
      selectedFont = static_cast<uint8_t>((next + FONT_COUNT) % FONT_COUNT);
      reelModeChangedAt = now;
      Serial.printf("Font preview: %u/%u %s\n", selectedFont + 1,
                    FONT_COUNT, FONT_NAMES[selectedFont]);
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
    } else if (DESIGN_REEL_ENABLED) {
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

void drawEye(int16_t centerX, float openness, const FacePose& pose) {
  const int16_t centerY = std::lround(82 + FACE_Y_OFFSET + pose.yOffset);
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

void setSelectedFont(uint8_t size) {
  if (selectedFont == 0 || size == 1) {
    canvas.setFont();
    canvas.setTextSize(size);
    return;
  }

  const GFXfont* fonts[] = {nullptr, &FreeMono9pt7b, &FreeSans9pt7b,
                            &FreeSerif9pt7b};
  canvas.setFont(fonts[selectedFont]);
  canvas.setTextSize(std::max<uint8_t>(1, (size + 1) / 2));
}

void drawCenteredText(const char* text, int16_t y, uint8_t size,
                      uint16_t color) {
  setSelectedFont(size);
  canvas.setTextColor(color);
  int16_t x1;
  int16_t y1;
  uint16_t width;
  uint16_t height;
  canvas.getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  canvas.setCursor((SCREEN_WIDTH - static_cast<int16_t>(width)) / 2 - x1,
                   y - y1);
  canvas.print(text);
  canvas.setFont();
  canvas.setTextSize(1);
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
  drawMenuRow("SELECT FACE", REEL_MODE_NAMES[reelMode], 47,
              mainMenuIndex == 0);
  drawMenuRow("SELECT FONT", FONT_NAMES[selectedFont], 103,
              mainMenuIndex == 1);
  drawMenuRow("SETTINGS", nullptr, 159, mainMenuIndex == 2);
  drawCenteredText("TURN: SELECT   PRESS: OPEN", 218, 1, textureColor);
}

void drawSettings(uint32_t now) {
  canvas.fillScreen(logoBackground);
  drawCenteredText("SETTINGS", 12, 2, eyeColor);
  canvas.drawFastHLine(12, 36, 296, logoHighlightColor);
  drawMenuRow("DISABLE BLINKING", nullptr, 59, settingsMenuIndex == 0);
  canvas.drawRect(278, 74, 18, 18,
                  blinkingDisabled ? logoHighlightColor : eyeColor);
  if (blinkingDisabled) {
    drawThickLine(281, 82, 286, 88, logoHighlightColor, 3);
    drawThickLine(286, 88, 294, 77, logoHighlightColor, 3);
  }
  drawMenuRow("RESET SETTINGS", nullptr, 119, settingsMenuIndex == 1);
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

  drawSideMetric(29, 77, stats.working, true, "WORKING", nullptr, eyeColor);
  drawSideMetric(29, 143, stats.needsAttention, stats.attentionAvailable,
                 "NEEDS", "YOU", accentColor);
  drawSideMetric(291, 77, stats.unread, stats.unreadAvailable, "NEW",
                 "MESSAGES", unreadColor);
  drawSideMetric(291, 143, stats.idle, true, "IDLE", nullptr, eyeColor);
}

void drawAttentionPulse(uint32_t elapsed) {
  const float beat = std::pow(std::max(0.0F, std::sin(elapsed * 0.0105F)), 7.0F);
  const uint16_t amber = display.color565(241, 94, 38);
  const uint16_t hot = display.color565(255, 194, 67);
  const uint16_t ink = display.color565(39, 13, 8);
  canvas.fillScreen(beat > 0.45F ? hot : amber);

  const int16_t radius = 43 + std::lround(beat * 8.0F);
  canvas.fillCircle(160, 70, radius, ink);
  const int16_t markOffset = std::lround(beat * 3.0F);
  canvas.fillRoundRect(154, 38 - markOffset, 13, 31, 6, hot);
  canvas.fillCircle(160, 82 - markOffset, 7, hot);
  drawCenteredText("NEEDS ATTENTION", 124, 3, ink);
  canvas.fillRect(40, 170, 240, 2, ink);
  drawCenteredText(elapsed < 2100 ? "APPROVAL REQUESTED" : "THREAD IS WAITING",
                   185, 2, ink);

  for (int16_t x = -20; x < SCREEN_WIDTH + 20; x += 32) {
    canvas.fillTriangle(x, 212, x + 15, 212, x + 31, 220, ink);
  }
}

FacePose notificationPose(uint32_t elapsed, float visibility) {
  const float surprise = smoothStep(elapsed / 500.0F) * visibility;
  FacePose pose = neutralPose();
  pose.eyeScale = mix(1.0F, 1.12F, surprise);
  pose.pupilScale = mix(1.0F, 0.78F, surprise);
  pose.gazeY = -0.3F * surprise;
  pose.mouthCurveY = mix(149.0F, 157.0F, surprise);
  pose.yOffset = visibility * 12.0F;
  return pose;
}

void drawStatusNotification(uint32_t elapsed, NotificationKind kind,
                            const char* title, const AmpStatsSnapshot& stats) {
  float visibility = smoothStep(elapsed / 600.0F);
  if (elapsed > 3500) {
    visibility *= 1.0F - smoothStep((elapsed - 3500) / 600.0F);
  }
  drawFace(notificationPose(elapsed, visibility));
  drawStatsPanel(stats);

  const uint16_t statusColor = kind == NotificationKind::Message
                                   ? unreadColor
                                   : display.color565(104, 180, 111);
  const uint16_t paper = display.color565(239, 236, 203);
  const int16_t stripHeight = std::lround(visibility * 48.0F);

  canvas.fillRect(0, 0, SCREEN_WIDTH, stripHeight, faceShadowColor);
  canvas.fillRect(0, stripHeight, SCREEN_WIDTH, 4, statusColor);
  if (stripHeight > 35) {
    char subtitle[45];
    std::snprintf(subtitle, sizeof(subtitle), "%.43s",
                  title && title[0] ? title : "Amp thread");
    canvas.fillCircle(20, 23, 8, statusColor);
    canvas.setTextColor(paper);
    canvas.setTextSize(2);
    canvas.setCursor(39, 9);
    canvas.print(kind == NotificationKind::Message
                     ? "NEW MESSAGE"
                     : (kind == NotificationKind::Shipped ? "SHIPPED"
                                                           : "THREAD WORKING"));
    canvas.setTextSize(1);
    canvas.setCursor(40, 31);
    canvas.print(subtitle);
  }
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
  constexpr size_t maxCharacters = 24;
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


// =================== DESIGN REEL drawing (temporary) ========================
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
      notificationElapsed < NOTIFICATION_DURATION_MS) {
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
    scene.phaseElapsed = now % REEL_IDLE_MS;
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
  char text[28];
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
      std::snprintf(status.text, sizeof(status.text), "NEW MESSAGE%s: %u",
                    scene.stats.unread == 1 ? "" : "S", scene.stats.unread);
      status.color = unreadColor;
      break;
    case ReelPhase::Attention:
      std::snprintf(status.text, sizeof(status.text), "NEEDS ATTENTION: %u",
                    scene.stats.needsAttention);
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

FacePose reelPose(const ReelScene& scene) {
  const uint32_t t = scene.phaseElapsed;
  FacePose pose = neutralPose();
  switch (scene.phase) {
    case ReelPhase::Idle:
      pose.gazeX = std::sin(t * 0.00078F) * 0.55F;
      pose.gazeY = std::cos(t * 0.0011F) * 0.12F;
      pose.leftEyeOpen = blinkAt(t % 6000, 2600, 280);
      pose.rightEyeOpen = pose.leftEyeOpen;
      break;
    case ReelPhase::Working:
      pose.gazeX = mix(0.0F, 0.62F, scene.intro) + std::sin(t * 0.0008F) * 0.1F;
      pose.gazeY = mix(0.0F, -0.42F, scene.intro);
      pose.leftEyeOpen = blinkAt(t % 5000, 4200, 300);
      pose.rightEyeOpen = pose.leftEyeOpen;
      pose.mouthCurveY = 148.0F;
      break;
    case ReelPhase::Message:
      pose.eyeScale = mix(1.0F, 1.08F, scene.intro);
      pose.pupilScale = mix(1.0F, 0.9F, scene.intro);
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
  return pose;
}

float reelTakeoverVisibility(const ReelScene& scene) {
  if (scene.phase != ReelPhase::Message &&
      scene.phase != ReelPhase::Attention) {
    return 0.0F;
  }
  float visibility = smoothStep(scene.phaseElapsed / 620.0F);
  if (scene.phaseElapsed > 3500) {
    visibility *=
        1.0F - smoothStep((scene.phaseElapsed - 3500) / 750.0F);
  }
  return visibility;
}

uint16_t reelPhaseColor(const ReelScene& scene) {
  return reelStatus(scene).color;
}

const char* reelProject(const ReelScene& scene) {
  return scene.eventProject[0] ? scene.eventProject : "AMP";
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
      const AmpThreadSummary* thread =
          reelThreadForPhase(scene.stats, ReelPhase::Attention);
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

void drawWatchShelf(const ReelScene& scene) {
  constexpr int16_t x = 8;
  constexpr int16_t width = 304;
  const ReelStatus status = reelStatus(scene);
  const uint16_t color = status.color;
  canvas.fillRoundRect(x, 194, width, 42, 7, faceShadowColor);
  canvas.fillRoundRect(x, 194, 6, 42, 3, color);
  canvas.setFont();
  canvas.setTextSize(1);
  canvas.setTextColor(color);
  canvas.setCursor(x + 14, 201);
  canvas.print(status.text);
  canvas.setTextColor(eyeColor);
  canvas.setCursor(x + 14, 218);
  if (scene.eventTitle[0]) {
    char title[48];
    copyEllipsized(title, sizeof(title), scene.eventTitle, 46);
    canvas.print(title);
  } else {
    canvas.print(reelDetail(scene));
  }
  if (scene.phase == ReelPhase::Attention) {
    canvas.drawRoundRect(x, 194, width, 42, 7, color);
  }
}

void drawTakeoverCopy(const ReelScene& scene, float visibility) {
  const int16_t bottom = std::lround(178.0F * visibility);
  const uint16_t color = reelPhaseColor(scene);
  canvas.fillRect(0, 0, SCREEN_WIDTH, bottom, logoBackground);
  canvas.fillRect(0, 0, 7, bottom, color);
  if (bottom > 1) {
    canvas.fillRect(0, bottom - 2, SCREEN_WIDTH, 2, color);
  }
  if (visibility < 0.82F) {
    return;
  }

  canvas.setFont();
  canvas.setTextSize(2);
  canvas.setTextColor(color);
  canvas.setCursor(20, 12);
  canvas.print(scene.phase == ReelPhase::Message ? "NEW MESSAGE" : "NEEDS YOU");
  canvas.setTextSize(1);
  canvas.setTextColor(textureColor);
  char project[18];
  copyEllipsized(project, sizeof(project), reelProject(scene), 16);
  canvas.setCursor(300 - std::strlen(project) * 6, 18);
  canvas.print(project);

  char firstLine[25];
  char secondLine[25];
  splitTitle(scene.eventTitle, firstLine, secondLine);
  drawCenteredText(firstLine, 48, 2, eyeColor);
  if (secondLine[0]) {
    drawCenteredText(secondLine, 70, 2, eyeColor);
  }
  drawCenteredText(reelDetail(scene), 108, 1, eyeColor);
  drawCenteredText(scene.phase == ReelPhase::Attention
                       ? "PRESS TO REVIEW"
                       : "PRESS TO OPEN THREAD",
                   145, 1, color);
}

void drawDesignCurrent(const ReelScene& scene) {
  if (scene.phase == ReelPhase::Attention) {
    drawAttentionPulse(scene.phaseElapsed % NOTIFICATION_DURATION_MS);
    return;
  }
  if (scene.phase == ReelPhase::Working &&
      scene.phaseElapsed < NOTIFICATION_DURATION_MS) {
    drawStatusNotification(scene.phaseElapsed, NotificationKind::ThreadActive,
                           scene.eventTitle, scene.stats);
    return;
  }
  if (scene.phase == ReelPhase::Message &&
      scene.phaseElapsed < NOTIFICATION_DURATION_MS) {
    drawStatusNotification(scene.phaseElapsed, NotificationKind::Message,
                           scene.eventTitle, scene.stats);
    return;
  }
  drawFace(reelPose(scene));
  drawStatsPanel(scene.stats);
}

void drawDesignMarquee(const ReelScene& scene) {
  FacePose pose = reelPose(scene);
  pose.yOffset = -22.0F;
  drawFace(pose);

  constexpr int16_t bandY = 182;
  const ReelStatus status = reelStatus(scene);
  const bool invert = scene.phase == ReelPhase::Attention ||
                      ((scene.phase == ReelPhase::Working ||
                        scene.phase == ReelPhase::Message) &&
                       scene.phaseElapsed < 1400);
  canvas.fillRect(0, bandY, SCREEN_WIDTH, SCREEN_HEIGHT - bandY,
                  invert ? status.color : logoBackground);
  canvas.fillRect(0, bandY, SCREEN_WIDTH, 3, status.color);
  drawCenteredText(status.text, bandY + 13, 3,
                   invert ? logoBackground : status.color);
  if (scene.eventTitle[0]) {
    char title[38];
    copyEllipsized(title, sizeof(title), scene.eventTitle, 35);
    drawCenteredText(title, bandY + 44, 1,
                     invert ? logoBackground : eyeColor);
  }
}

void drawDesignHeadline(const ReelScene& scene) {
  drawFaceBackground();
  uint16_t count = scene.stats.idle;
  const char* label = "THREADS IDLE";
  const ReelStatus status = reelStatus(scene);
  if (scene.phase == ReelPhase::Working) {
    count = scene.stats.working;
    label = "THREADS WORKING";
  } else if (scene.phase == ReelPhase::Message) {
    count = scene.stats.unread;
    label = "NEW MESSAGE";
  } else if (scene.phase == ReelPhase::Attention) {
    count = scene.stats.needsAttention;
    label = "THREADS NEED YOU";
  }

  if (scene.phase == ReelPhase::Resolved) {
    drawCenteredText("ALL CLEAR", 40, 5, status.color);
  } else {
    char number[8];
    std::snprintf(number, sizeof(number), "%u", count);
    drawCenteredText(number, 12, 7, status.color);
    drawCenteredText(label, 76, 2, status.color);
  }
  if (scene.eventTitle[0]) {
    drawCenteredText("LATEST", 112, 1, textureColor);
    char firstLine[25];
    char secondLine[25];
    splitTitle(scene.eventTitle, firstLine, secondLine);
    drawCenteredText(firstLine, 126, 2, eyeColor);
    if (secondLine[0]) {
      drawCenteredText(secondLine, 148, 2, eyeColor);
    }
  }

  FacePose eyes = reelPose(scene);
  eyes.yOffset = 152.0F;
  eyes.gazeY = -0.75F;
  drawEye(124, eyes.leftEyeOpen, eyes);
  drawEye(196, eyes.rightEyeOpen, eyes);
}

void drawReelBadge(int16_t x, int16_t y, uint16_t value, const char* label,
                   uint16_t color, bool selected) {
  const uint16_t background = selected ? color : faceShadowColor;
  const uint16_t foreground = selected ? logoBackground
                                       : (value ? color : logoHighlightColor);
  canvas.fillRoundRect(x, y, 76, 44, 7, background);
  canvas.drawRoundRect(x, y, 76, 44, 7,
                       selected ? color : logoHighlightColor);
  char count[8];
  std::snprintf(count, sizeof(count), "%u", value);
  canvas.setFont();
  canvas.setTextSize(2);
  canvas.setTextColor(foreground);
  canvas.setCursor(x + 7, y + 5);
  canvas.print(count);
  canvas.setTextSize(1);
  canvas.setCursor(x + 7, y + 29);
  canvas.print(label);
}

void drawDesignBadges(const ReelScene& scene) {
  drawFace(reelPose(scene));
  drawReelBadge(6, 4, scene.stats.working, "WORKING", reelGreenColor(),
                scene.phase == ReelPhase::Working);
  drawReelBadge(238, 4, scene.stats.unread, "NEW MSGS", unreadColor,
                scene.phase == ReelPhase::Message);
  drawReelBadge(6, 192, scene.stats.needsAttention, "NEED YOU", accentColor,
                scene.phase == ReelPhase::Attention);
  drawReelBadge(238, 192, scene.stats.idle, "IDLE", eyeColor, false);
}

void drawDesignTicker(const ReelScene& scene, uint32_t elapsed) {
  FacePose pose = reelPose(scene);
  pose.yOffset = -16.0F;
  drawFace(pose);

  constexpr int16_t tickerY = 198;
  const ReelStatus status = reelStatus(scene);
  const bool flash = scene.phase != ReelPhase::Idle &&
                     scene.phase != ReelPhase::Resolved &&
                     scene.phaseElapsed < 900;
  canvas.fillRect(0, tickerY, SCREEN_WIDTH, SCREEN_HEIGHT - tickerY,
                  flash ? status.color : logoBackground);
  canvas.fillRect(0, tickerY, SCREEN_WIDTH, 3, status.color);
  char ticker[200] = "";
  for (uint8_t index = 0; index < scene.stats.threadCount; ++index) {
    const AmpThreadSummary& thread = scene.stats.threads[index];
    char item[65];
    std::snprintf(item, sizeof(item), "%s: %.42s%s",
                  threadStateLabel(thread.state), thread.title,
                  index + 1 < scene.stats.threadCount ? "  *  " : "  *  ");
    std::strncat(ticker, item, sizeof(ticker) - std::strlen(ticker) - 1);
  }
  if (!ticker[0]) {
    std::snprintf(ticker, sizeof(ticker), "%s  *  ", status.text);
  }
  const int16_t width = std::max<int16_t>(12, std::strlen(ticker) * 12);
  const int16_t offset = (elapsed / 12) % width;
  canvas.setTextWrap(false);
  canvas.setFont();
  canvas.setTextSize(2);
  canvas.setTextColor(flash ? logoBackground : eyeColor);
  canvas.setCursor(-offset, tickerY + 13);
  canvas.print(ticker);
  canvas.setCursor(width - offset, tickerY + 13);
  canvas.print(ticker);
  canvas.setTextWrap(true);
}

void drawDesignRing(const ReelScene& scene, uint32_t elapsed) {
  drawFace(reelPose(scene));
  uint8_t slot = 0;
  for (uint8_t index = 0; index < 12; ++index) {
    uint16_t color = faceShadowColor;
    if (slot < scene.stats.working) {
      color = reelGreenColor();
    } else if (slot < scene.stats.working + scene.stats.unread) {
      color = unreadColor;
    } else if (slot < scene.stats.working + scene.stats.unread +
                          scene.stats.needsAttention) {
      color = accentColor;
    } else if (slot < scene.stats.working + scene.stats.unread +
                          scene.stats.needsAttention + scene.stats.idle) {
      color = logoHighlightColor;
    }
    const float angle = elapsed * 0.00015F +
                        FULL_ROTATION_RADIANS * index / 12.0F;
    const int16_t x = 160 + std::lround(std::cos(angle) * 108.0F);
    const int16_t y = 120 + std::lround(std::sin(angle) * 108.0F);
    const int16_t radius =
        color == accentColor ? 6 + std::lround(scene.beat * 3.0F) : 6;
    canvas.fillCircle(x, y, radius, color);
    ++slot;
  }
}

void drawDesignSplit(const ReelScene& scene) {
  FacePose pose = reelPose(scene);
  pose.yOffset = -40.0F;
  drawFace(pose);
  constexpr int16_t panelY = 150;
  const ReelStatus status = reelStatus(scene);
  canvas.fillRect(0, panelY, SCREEN_WIDTH, SCREEN_HEIGHT - panelY,
                  scene.phase == ReelPhase::Attention ? accentColor
                                                      : logoBackground);
  canvas.fillRect(0, panelY, SCREEN_WIDTH, 2,
                  scene.phase == ReelPhase::Attention ? logoBackground
                                                      : status.color);
  if (scene.phase == ReelPhase::Message) {
    float visibility = smoothStep(scene.phaseElapsed / 500.0F);
    if (scene.phaseElapsed > REEL_MESSAGE_MS - 700) {
      visibility *= 1.0F - smoothStep(
          (scene.phaseElapsed - (REEL_MESSAGE_MS - 700)) / 700.0F);
    }
    const int16_t y = std::lround(mix(240.0F, 158.0F, visibility));
    const uint16_t paper = display.color565(239, 236, 203);
    canvas.fillRoundRect(16, y, 288, 84, 8, paper);
    canvas.fillRect(16, y, 14, 84, unreadColor);
    canvas.setFont();
    canvas.setTextColor(pupilColor);
    canvas.setTextSize(2);
    canvas.setCursor(42, y + 10);
    canvas.print("NEW MESSAGE");
    char title[45];
    copyEllipsized(title, sizeof(title), scene.eventTitle, 42);
    canvas.setTextSize(1);
    canvas.setCursor(42, y + 38);
    canvas.print(title);
    canvas.setCursor(42, y + 61);
    canvas.print("PRESS TO OPEN THREADS");
    return;
  }
  uint16_t count = scene.stats.idle;
  if (scene.phase == ReelPhase::Working) count = scene.stats.working;
  if (scene.phase == ReelPhase::Attention) count = scene.stats.needsAttention;
  char number[8];
  std::snprintf(number, sizeof(number), "%u", count);
  canvas.setFont();
  canvas.setTextSize(6);
  canvas.setTextColor(scene.phase == ReelPhase::Attention ? logoBackground
                                                          : status.color);
  canvas.setCursor(24, 172);
  canvas.print(number);
  canvas.setTextSize(2);
  canvas.setCursor(120, 176);
  canvas.print(status.text);
  if (scene.eventTitle[0]) {
    char title[33];
    copyEllipsized(title, sizeof(title), scene.eventTitle, 30);
    canvas.setTextSize(1);
    canvas.setCursor(120, 202);
    canvas.print(title);
  }
}

void drawDesignPoster(const ReelScene& scene) {
  canvas.fillScreen(logoBackground);
  struct PosterRow {
    uint16_t value;
    const char* label;
    uint16_t color;
  };
  const PosterRow rows[4] = {
      {scene.stats.working, "WORKING", reelGreenColor()},
      {scene.stats.unread, "NEW MSGS", unreadColor},
      {scene.stats.needsAttention, "NEED YOU", accentColor},
      {scene.stats.idle, "IDLE", eyeColor},
  };
  int8_t selected = -1;
  if (scene.phase == ReelPhase::Working) selected = 0;
  if (scene.phase == ReelPhase::Message) selected = 1;
  if (scene.phase == ReelPhase::Attention) selected = 2;
  for (uint8_t row = 0; row < 4; ++row) {
    const int16_t y = 14 + row * 50;
    if (selected == row) {
      canvas.fillRect(0, y - 4, std::lround(SCREEN_WIDTH * scene.intro), 42,
                      rows[row].color);
    }
    char line[24];
    std::snprintf(line, sizeof(line), "%u %s", rows[row].value,
                  rows[row].label);
    canvas.setFont();
    canvas.setTextSize(3);
    canvas.setTextColor(selected == row ? logoBackground
                                        : (rows[row].value ? rows[row].color
                                                           : logoHighlightColor));
    canvas.setCursor(18, y);
    canvas.print(line);
  }
  FacePose eyes = reelPose(scene);
  eyes.eyeScale = 0.38F;
  eyes.pupilScale = 0.5F;
  eyes.yOffset = -67.0F;
  eyes.gazeY = selected < 0 ? 0.0F : mix(-0.8F, 0.8F, selected / 3.0F);
  drawEye(266, 1.0F, eyes);
  drawEye(298, 1.0F, eyes);
  if (scene.eventTitle[0]) {
    char title[47];
    copyEllipsized(title, sizeof(title), scene.eventTitle, 45);
    canvas.setTextSize(1);
    canvas.setTextColor(eyeColor);
    canvas.setCursor(18, 222);
    canvas.print(title);
  }
}

void drawDesignTerminal(const ReelScene& scene, uint32_t elapsed) {
  canvas.fillScreen(logoBackground);
  const uint16_t green = reelGreenColor();
  canvas.setFont();
  canvas.setTextSize(2);
  canvas.setTextColor(green);
  canvas.setCursor(12, 8);
  canvas.print("PUCK://MAIN");
  FacePose eyes = reelPose(scene);
  eyes.eyeScale = 0.35F;
  eyes.pupilScale = 0.5F;
  eyes.yOffset = -68.0F;
  drawEye(272, 1.0F, eyes);
  drawEye(302, 1.0F, eyes);
  canvas.drawFastHLine(10, 34, 300, logoHighlightColor);

  const auto printLine = [&](int16_t y, const char* prefix, const char* text,
                             uint16_t color) {
    char clipped[26];
    copyEllipsized(clipped, sizeof(clipped), text, 23);
    canvas.setFont();
    canvas.setTextSize(2);
    canvas.setTextColor(color);
    canvas.setCursor(12, y);
    canvas.print(prefix);
    canvas.print(clipped);
  };
  int16_t y = 44;
  char tracked[24];
  std::snprintf(tracked, sizeof(tracked), "%u threads tracked",
                scene.stats.total);
  printLine(y, "> ", tracked, eyeColor);
  y += 24;
  if (scene.phase >= ReelPhase::Working) {
    printLine(y, "> run  ", scene.stats.threads[0].title, green);
    y += 24;
  }
  if (scene.phase >= ReelPhase::Message) {
    printLine(y, "> mail ", scene.stats.threads[0].title, unreadColor);
    y += 24;
  }
  if (scene.phase >= ReelPhase::Attention) {
    printLine(y, "> ATTN ", "approval needed", accentColor);
    y += 24;
  }
  if (scene.phase == ReelPhase::Resolved) {
    printLine(y, "> done ", "all clear", green);
    y += 24;
  }
  if ((elapsed / 400) % 2 == 0) {
    canvas.fillRect(12, y + 2, 12, 18, green);
  }
}

void drawDesignGauge(const ReelScene& scene, uint32_t elapsed) {
  canvas.fillScreen(logoBackground);
  const ReelStatus status = reelStatus(scene);
  drawCenteredText(status.text, 14, 3, status.color);
  if (scene.eventTitle[0]) {
    char title[43];
    copyEllipsized(title, sizeof(title), scene.eventTitle, 40);
    drawCenteredText(title, 50, 1, eyeColor);
  }
  FacePose eyes = reelPose(scene);
  eyes.eyeScale = 0.55F;
  eyes.pupilScale = 0.7F;
  eyes.yOffset = -15.0F;
  drawEye(132, 1.0F, eyes);
  drawEye(188, 1.0F, eyes);

  float urgency = 0.08F + std::sin(elapsed * 0.001F) * 0.02F;
  if (scene.phase == ReelPhase::Working) {
    urgency = 0.38F + std::sin(elapsed * 0.002F) * 0.05F;
  } else if (scene.phase == ReelPhase::Message) {
    urgency = 0.6F;
  } else if (scene.phase == ReelPhase::Attention) {
    urgency = 0.9F + std::sin(elapsed * 0.03F) * 0.04F;
  } else if (scene.phase == ReelPhase::Resolved) {
    urgency = 0.12F;
  }
  constexpr float HALF_TURN = 3.141592654F;
  for (uint8_t dot = 0; dot < 13; ++dot) {
    const float angle = HALF_TURN - HALF_TURN * dot / 12.0F;
    const int16_t x = 160 + std::lround(std::cos(angle) * 84.0F);
    const int16_t y = 218 - std::lround(std::sin(angle) * 84.0F);
    const uint16_t color = dot < 5 ? reelGreenColor()
                           : dot < 9 ? unreadColor
                                     : accentColor;
    canvas.fillCircle(x, y, 5, color);
  }
  const float needleAngle = HALF_TURN * (1.0F - clamp01(urgency));
  const int16_t needleX = 160 + std::lround(std::cos(needleAngle) * 62.0F);
  const int16_t needleY = 218 - std::lround(std::sin(needleAngle) * 62.0F);
  drawThickLine(160, 218, needleX, needleY, eyeColor, 5);
  canvas.fillCircle(160, 218, 9, eyeColor);
  canvas.fillCircle(160, 218, 4, logoBackground);
}

void drawDesignMinimal(const ReelScene& scene) {
  drawFace(reelPose(scene));
  const ReelStatus status = reelStatus(scene);
  const int16_t groupWidth = std::strlen(status.text) * 12 + 18;
  const int16_t x = (SCREEN_WIDTH - groupWidth) / 2;
  canvas.fillCircle(x + 5, 228, 5, status.color);
  canvas.setFont();
  canvas.setTextSize(2);
  canvas.setTextColor(status.color);
  canvas.setCursor(x + 18, 220);
  canvas.print(status.text);
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
  const size_t length = std::strlen(reelEventHeadline(scene));
  const uint8_t fitted = static_cast<uint8_t>(300 / std::max<size_t>(1, length * 6));
  return std::max<uint8_t>(1, std::min(preferred, fitted));
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

void drawWatchingEyes(const ReelScene& scene, int16_t yOffset = 152) {
  FacePose eyes = reelPose(scene);
  eyes.yOffset = yOffset;
  eyes.gazeY = -0.82F;
  eyes.eyeScale = scene.phase == ReelPhase::Attention ? 1.12F : 1.0F;
  drawEye(124, eyes.leftEyeOpen, eyes);
  drawEye(196, eyes.rightEyeOpen, eyes);
}

// BILLBOARD — no chrome and no polite shelf. A notification becomes a bold
// full-screen poster, while Puck's eyes remain at the foot of the display.
void drawDesignBillboard(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  if (show < 0.01F) {
    drawDesignMinimal(scene);
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  const int16_t width = std::lround(SCREEN_WIDTH * show);
  drawDesignMinimal(scene);
  canvas.fillRect(0, 0, width, SCREEN_HEIGHT, color);
  if (show < 0.78F) {
    return;
  }
  const uint16_t ink = logoBackground;
  drawCenteredText(reelEventHeadline(scene), 18, 4, ink);
  canvas.fillRect(28, 66, 264, 3, ink);
  drawReelEventTitle(scene, 82, ink);
  drawCenteredText(scene.phase == ReelPhase::Attention
                       ? "PRESS TO HANDLE IT"
                       : reelDetail(scene),
                   142, 1, ink);
  drawWatchingEyes(scene, 151);
}

// IRIS — the status dot on Minimal swells until it owns the screen. The
// transition feels like Puck has suddenly focused on one important thing.
void drawDesignIris(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  const int16_t radius = std::lround(8.0F + 215.0F * show);
  canvas.fillCircle(160, 118, radius, color);
  canvas.drawCircle(160, 118, std::max<int16_t>(1, radius - 7),
                    logoBackground);
  if (show < 0.76F) {
    return;
  }
  drawCenteredText(reelEventHeadline(scene), 34, 3, logoBackground);
  drawReelEventTitle(scene, 82, logoBackground);
  drawCenteredText(scene.phase == ReelPhase::Attention ? "PUCK NEEDS A HAND"
                                                        : reelDetail(scene),
                   139, 1, logoBackground);
  FacePose eyes = reelPose(scene);
  eyes.eyeScale = 0.66F;
  eyes.pupilScale = 0.76F;
  eyes.yOffset = 73.0F;
  eyes.gazeY = -0.65F;
  drawEye(133, eyes.leftEyeOpen, eyes);
  drawEye(187, eyes.rightEyeOpen, eyes);
}

// PAPER — a physical dispatch drops over the face, overshoots, and settles.
// It deliberately looks unlike the screen underneath it.
void drawDesignPaper(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const float settle = 1.0F - std::exp(-scene.phaseElapsed * 0.006F) *
                                  std::cos(scene.phaseElapsed * 0.016F);
  const int16_t y = std::lround(mix(-250.0F, 7.0F, settle) +
                                  (1.0F - show) * 250.0F);
  const uint16_t paper = display.color565(236, 231, 197);
  const uint16_t ink = display.color565(31, 36, 26);
  const uint16_t color = reelPhaseColor(scene);
  canvas.fillRoundRect(15, y + 5, 294, 224, 9, faceShadowColor);
  canvas.fillRoundRect(10, y, 294, 224, 9, paper);
  canvas.fillRect(10, y, 294, 12, color);
  canvas.setFont();
  canvas.setTextColor(ink);
  canvas.setTextSize(1);
  canvas.setCursor(25, y + 24);
  canvas.print("AMP DISPATCH / ");
  canvas.print(reelProject(scene));
  canvas.drawFastHLine(24, y + 41, 266, ink);
  drawCenteredText(reelEventHeadline(scene), y + 54, 4, ink);
  drawReelEventTitle(scene, y + 105, ink);
  canvas.drawFastHLine(24, y + 158, 266, ink);
  drawCenteredText(scene.phase == ReelPhase::Attention
                       ? "ACTION REQUIRED - PRESS"
                       : reelDetail(scene),
                   y + 174, 1, ink);
  canvas.fillCircle(276, y + 194, 15, color);
  canvas.drawCircle(276, y + 194, 10, paper);
}

// COMIC — Puck reacts with one huge speech balloon rather than UI panels.
void drawDesignComicFull(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  FacePose pose = reelPose(scene);
  pose.yOffset += 54.0F * show;
  pose.eyeScale += 0.12F * show;
  drawFace(pose);
  if (show < 0.01F) {
    drawDesignMinimal(scene);
    return;
  }
  const uint16_t paper = scene.phase == ReelPhase::Attention
                             ? accentColor
                             : display.color565(238, 234, 202);
  const uint16_t ink = display.color565(31, 35, 25);
  const int16_t y = std::lround(mix(-150.0F, 5.0F, show));
  canvas.fillRoundRect(8, y, 304, 145, 18, paper);
  canvas.fillTriangle(136, y + 142, 184, y + 142, 160, y + 174, paper);
  drawCenteredText(scene.phase == ReelPhase::Working
                       ? "I'M ON IT."
                       : scene.phase == ReelPhase::Message
                             ? "OH! MAIL."
                             : scene.phase == ReelPhase::Attention
                                   ? "UM. JAN?"
                                   : "WE DID IT.",
                   y + 16, 4, ink);
  drawReelEventTitle(scene, y + 67, ink);
  drawCenteredText(scene.phase == ReelPhase::Attention
                       ? "THIS ONE ACTUALLY NEEDS YOU"
                       : reelDetail(scene),
                   y + 117, 1, ink);
}

void drawHazardStripes(uint16_t color, int16_t offset) {
  for (int16_t x = -50; x < SCREEN_WIDTH + 50; x += 38) {
    canvas.fillTriangle(x + offset, 0, x + 18 + offset, 0,
                        x - 18 + offset, 22, color);
    canvas.fillTriangle(x - 18 + offset, 218, x + 18 + offset, 240,
                        x + offset, 240, color);
  }
}

// SIREN — every event has a different visual grammar: scanning work lanes,
// an expanding blue mail slot, and unmistakable amber hazard stripes.
void drawDesignSiren(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  if (scene.phase == ReelPhase::Working) {
    for (uint8_t lane = 0; lane < 5; ++lane) {
      const int16_t y = 15 + lane * 46;
      const int16_t run = ((scene.phaseElapsed / 5 + lane * 53) % 390) - 70;
      canvas.fillRoundRect(run, y, 74, 9, 4, color);
    }
  } else if (scene.phase == ReelPhase::Message) {
    const int16_t h = std::lround(178.0F * show);
    canvas.fillRoundRect(7, (SCREEN_HEIGHT - h) / 2, 306, h, 10, color);
    canvas.drawLine(8, (SCREEN_HEIGHT - h) / 2,
                    160, (SCREEN_HEIGHT + h) / 2, logoBackground);
    canvas.drawLine(312, (SCREEN_HEIGHT - h) / 2,
                    160, (SCREEN_HEIGHT + h) / 2, logoBackground);
  } else if (scene.phase == ReelPhase::Attention) {
    drawHazardStripes(color, (scene.phaseElapsed / 18) % 38);
    const int16_t edge = 5 + std::lround(scene.beat * 7.0F);
    canvas.drawRect(edge, edge, SCREEN_WIDTH - edge * 2,
                    SCREEN_HEIGHT - edge * 2, color);
  } else {
    for (uint8_t ray = 0; ray < 12; ++ray) {
      const float angle = FULL_ROTATION_RADIANS * ray / 12.0F;
      canvas.drawLine(160 + std::cos(angle) * 45,
                      120 + std::sin(angle) * 45,
                      160 + std::cos(angle) * 145,
                      120 + std::sin(angle) * 145, color);
    }
  }
  if (show > 0.7F) {
    canvas.fillRoundRect(20, 63, 280, 114, 10, logoBackground);
    canvas.drawRoundRect(20, 63, 280, 114, 10, color);
    drawCenteredText(reelEventHeadline(scene), 76, 3, color);
    drawReelEventTitle(scene, 116, eyeColor);
    drawCenteredText(scene.phase == ReelPhase::Attention ? "PRESS TO RESPOND"
                                                          : reelDetail(scene),
                     158, 1, color);
  }
}

// PEEK — the strongest version of the original Watch idea. Giant copy pushes
// Puck completely below the fold; only bewildered eyes remain on duty.
void drawDesignPeek(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  FacePose pose = reelPose(scene);
  pose.yOffset += 142.0F * show;
  pose.gazeY = mix(pose.gazeY, -0.88F, show);
  drawFace(pose);
  if (show < 0.01F) {
    drawDesignMinimal(scene);
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  const int16_t panelBottom = std::lround(184.0F * show);
  canvas.fillRect(0, 0, SCREEN_WIDTH, panelBottom, logoBackground);
  canvas.fillRect(0, 0, SCREEN_WIDTH, 10, color);
  if (show < 0.72F) {
    return;
  }
  drawCenteredText(reelEventHeadline(scene), 20, 5, color);
  drawReelEventTitle(scene, 84, eyeColor);
  drawCenteredText(scene.phase == ReelPhase::Attention ? "PRESS. I'LL WATCH."
                                                        : reelDetail(scene),
                   145, 1, color);
}

// STAMP — a restrained cream notice that becomes playful through an enormous
// offset status stamp landing over the thread title.
void drawDesignStamp(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const uint16_t paper = display.color565(232, 227, 194);
  const uint16_t ink = display.color565(38, 42, 30);
  const uint16_t color = reelPhaseColor(scene);
  const int16_t x = std::lround(mix(330.0F, 7.0F, show));
  canvas.fillRect(x, 5, 306, 230, paper);
  canvas.fillRect(x, 5, 9, 230, color);
  canvas.setFont();
  canvas.setTextSize(1);
  canvas.setTextColor(ink);
  canvas.setCursor(x + 25, 22);
  canvas.print("POCKETPUCK / EVENT NOTICE");
  canvas.drawFastHLine(x + 25, 41, 256, ink);
  drawReelEventTitle(scene, 57, ink);
  drawCenteredText(reelProject(scene), 112, 1, ink);
  const float stamp = smoothStep((scene.phaseElapsed - 380) / 250.0F);
  const int16_t stampY = std::lround(mix(-60.0F, 137.0F, stamp));
  canvas.drawRoundRect(x + 31, stampY, 244, 54, 7, color);
  canvas.drawRoundRect(x + 34, stampY + 3, 238, 48, 5, color);
  drawCenteredText(reelEventHeadline(scene), stampY + 11, 3, color);
  drawCenteredText(scene.phase == ReelPhase::Attention ? "PRESS TO REVIEW"
                                                        : reelDetail(scene),
                   211, 1, ink);
}

// BLOCKS — chunky color tiles crash together to spell out the event, then
// separate again. It is intentionally graphic and toy-like.
void drawDesignBlocks(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  const int16_t left = std::lround(mix(-170.0F, 0.0F, show));
  const int16_t right = std::lround(mix(320.0F, 160.0F, show));
  canvas.fillRect(left, 0, 160, 240, color);
  canvas.fillRect(right, 0, 160, 240, color);
  for (uint8_t row = 0; row < 4; ++row) {
    const int16_t y = 16 + row * 58;
    canvas.fillRect(row % 2 ? left + 8 : right + 8, y, 144, 5,
                    logoBackground);
  }
  if (show < 0.74F) {
    return;
  }
  drawCenteredText(reelEventHeadline(scene), 25, 4, logoBackground);
  canvas.fillRoundRect(17, 79, 286, 91, 8, logoBackground);
  drawReelEventTitle(scene, 94, eyeColor);
  drawCenteredText(scene.phase == ReelPhase::Attention ? "PRESS TO UNBLOCK"
                                                        : reelDetail(scene),
                   145, 1, color);
  drawWatchingEyes(scene, 153);
}

// JUKEBOX — the home face stays calm, then the event arrives like a title
// card from an animated show, complete with bouncing dots and a Puck cameo.
void drawDesignJukebox(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  canvas.fillScreen(logoBackground);
  for (uint8_t dot = 0; dot < 18; ++dot) {
    const uint32_t h = reelHash(dot + static_cast<uint8_t>(scene.phase) * 31);
    const float bounce = std::abs(std::sin(scene.phaseElapsed * 0.004F + dot));
    const int16_t x = 7 + h % 307;
    const int16_t y = 8 + (h >> 9) % 220;
    canvas.fillCircle(x, y - std::lround(bounce * 8.0F), 3, color);
  }
  const float pop = std::min(1.18F, reelCardSettle(scene.phaseElapsed));
  const int16_t width = std::lround(286.0F * std::min(1.0F, pop));
  const int16_t x = (SCREEN_WIDTH - width) / 2;
  canvas.fillRoundRect(x, 23, width, 166, 18, color);
  if (show > 0.7F) {
    drawCenteredText(scene.phase == ReelPhase::Working
                         ? "GO GO GO"
                         : scene.phase == ReelPhase::Message
                               ? "DING!"
                               : scene.phase == ReelPhase::Attention
                                     ? "HEY!"
                                     : "NICE.",
                     39, 5, logoBackground);
    drawCenteredText(reelEventHeadline(scene), 98, 2, logoBackground);
    drawReelEventTitle(scene, 127, logoBackground);
    drawCenteredText(scene.phase == ReelPhase::Attention ? "PRESS FOR THE PLOT"
                                                          : reelDetail(scene),
                     172, 1, logoBackground);
  }
  FacePose eyes = reelPose(scene);
  eyes.eyeScale = 0.52F;
  eyes.pupilScale = 0.65F;
  eyes.yOffset = 92.0F;
  eyes.gazeY = -0.75F;
  drawEye(137, eyes.leftEyeOpen, eyes);
  drawEye(183, eyes.rightEyeOpen, eyes);
}

void drawEventPanelCopy(const ReelScene& scene, int16_t headlineY,
                        int16_t titleY, int16_t actionY, uint16_t headline,
                        uint16_t body) {
  drawCenteredText(reelEventHeadline(scene), headlineY,
                   reelHeadlineSize(scene), headline);
  drawReelEventTitle(scene, titleY, body);
  drawCenteredText(scene.phase == ReelPhase::Attention ? "PRESS TO RESPOND"
                                                        : reelDetail(scene),
                   actionY, 1, headline);
}

// AIRLOCK — two heavy doors close around the event while Puck watches through
// the remaining viewport. Working glides; attention shudders on impact.
void drawDesignAirlock(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  const int16_t travel = std::lround(92.0F * show);
  const int16_t shudder = scene.phase == ReelPhase::Attention
                              ? std::lround(std::sin(scene.phaseElapsed * 0.08F) *
                                            3.0F * scene.beat)
                              : 0;
  canvas.fillRect(0, -92 + travel + shudder, SCREEN_WIDTH, 92, color);
  canvas.fillRect(0, 240 - travel - shudder, SCREEN_WIDTH, 92, color);
  for (int16_t x = 12; x < SCREEN_WIDTH; x += 42) {
    canvas.fillRect(x, -84 + travel + shudder, 24, 3, logoBackground);
    canvas.fillRect(x, 315 - travel - shudder, 24, 3, logoBackground);
  }
  if (show < 0.72F) {
    return;
  }
  canvas.fillRoundRect(12, 70, 296, 102, 9, logoBackground);
  canvas.drawRoundRect(12, 70, 296, 102, 9, color);
  drawCenteredText(reelEventHeadline(scene), 79, 3, color);
  drawReelEventTitle(scene, 116, eyeColor);
  drawCenteredText(scene.phase == ReelPhase::Attention ? "AIRLOCKED - PRESS"
                                                        : reelDetail(scene),
                   154, 1, color);
  drawWatchingEyes(scene, 151);
}

// BROADCAST — Puck interrupts normal programming with a giant, readable news
// bulletin. The face becomes the deadpan field reporter in the corner.
void drawDesignBroadcast(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  const int16_t wipe = std::lround(SCREEN_HEIGHT * show);
  canvas.fillRect(0, 0, SCREEN_WIDTH, wipe, logoBackground);
  canvas.fillRect(0, 0, SCREEN_WIDTH, std::min<int16_t>(46, wipe), color);
  if (show < 0.72F) {
    return;
  }
  drawCenteredText(scene.phase == ReelPhase::Attention ? "BREAKING: YOUR MOVE"
                                                        : reelEventHeadline(scene),
                   13, scene.phase == ReelPhase::Attention ? 2 : 3,
                   logoBackground);
  canvas.setFont();
  canvas.setTextSize(1);
  canvas.setTextColor(textureColor);
  canvas.setCursor(14, 58);
  canvas.print("LIVE FROM ");
  canvas.print(reelProject(scene));
  drawReelEventTitle(scene, 79, eyeColor);
  canvas.fillRect(0, 174, SCREEN_WIDTH, 42, color);
  drawCenteredText(scene.phase == ReelPhase::Attention
                       ? "THREAD BLOCKED - PRESS TO REVIEW"
                       : reelDetail(scene),
                   187, 1, logoBackground);
  FacePose reporter = reelPose(scene);
  reporter.eyeScale = 0.42F;
  reporter.pupilScale = 0.58F;
  reporter.yOffset = 91.0F;
  reporter.gazeX = -0.65F;
  drawEye(276, reporter.leftEyeOpen, reporter);
  drawEye(308, reporter.rightEyeOpen, reporter);
  canvas.fillRect(0, 220, SCREEN_WIDTH, 20, logoBackground);
  drawCenteredText("PUCK NEWS NETWORK", 226, 1, color);
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
  canvas.setTextSize(2);
  canvas.setCursor(x + 35, 63);
  canvas.print(reelEventHeadline(scene));
  char firstLine[25];
  char secondLine[25];
  splitTitle(scene.eventTitle, firstLine, secondLine);
  canvas.setTextSize(1);
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

// BEACON — a graphic pulse radiates behind a central message capsule. It has
// Siren's energy but concentrates attention instead of filling every edge.
void drawDesignBeacon(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const float exit = reelEventExit(scene);
  const uint16_t color = reelPhaseColor(scene);
  if (scene.phase == ReelPhase::Working) {
    for (uint8_t dot = 0; dot < 6; ++dot) {
      const float angle = FULL_ROTATION_RADIANS * dot / 6.0F +
                          scene.phaseElapsed * 0.001F;
      canvas.fillCircle(160 + std::cos(angle) * 112,
                        116 + std::sin(angle) * 98, 3, color);
    }
    return;
  }
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

// ELEVATOR — the notification car descends, pushing Puck toward the bottom.
// A floor indicator makes the state hierarchy instantly legible.
void drawDesignElevator(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  FacePose pose = reelPose(scene);
  pose.yOffset += show * 120.0F;
  pose.gazeY = mix(pose.gazeY, -0.82F, show);
  drawFace(pose);
  if (show < 0.01F) {
    drawDesignMinimal(scene);
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  const int16_t y = std::lround(mix(-190.0F, 0.0F, show));
  canvas.fillRect(0, y, SCREEN_WIDTH, 181, logoBackground);
  canvas.fillRect(0, y + 177, SCREEN_WIDTH, 4, color);
  canvas.fillRoundRect(12, y + 12, 43, 43, 7, color);
  const char* floor = scene.phase == ReelPhase::Working
                          ? "W"
                          : scene.phase == ReelPhase::Message
                                ? "M"
                                : scene.phase == ReelPhase::Attention ? "!" : "OK";
  const uint8_t floorSize = scene.phase == ReelPhase::Resolved ? 2 : 3;
  canvas.setFont();
  canvas.setTextSize(floorSize);
  canvas.setTextColor(logoBackground);
  canvas.setCursor(33 - std::strlen(floor) * floorSize * 3,
                   y + (floorSize == 3 ? 22 : 26));
  canvas.print(floor);
  canvas.setFont();
  canvas.setTextSize(1);
  canvas.setTextColor(textureColor);
  canvas.setCursor(69, y + 14);
  canvas.print("NOW ARRIVING");
  canvas.setTextSize(3);
  canvas.setTextColor(color);
  canvas.setCursor(69, y + 30);
  canvas.print(reelEventHeadline(scene));
  drawReelEventTitle(scene, y + 78, eyeColor);
  drawCenteredText(scene.phase == ReelPhase::Attention ? "HOLD DOOR / PRESS"
                                                        : reelDetail(scene),
                   y + 144, 1, color);
}

// SPOTLIGHT — the face remains visible, but a moving cone finds Puck and a
// compact headline drops over it. Attention trembles under the light.
void drawDesignSpotlight(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  FacePose pose = reelPose(scene);
  pose.gazeX = std::sin(scene.phaseElapsed * 0.002F) * show;
  pose.eyeScale += 0.1F * show;
  drawFace(pose);
  if (show < 0.01F) {
    drawDesignMinimal(scene);
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  const int16_t sweep = std::lround(mix(-130.0F, 160.0F, show));
  drawThickLine(sweep - 18, 0, 60, 240, color, 4);
  drawThickLine(sweep + 18, 0, 260, 240, color, 4);
  for (uint8_t glint = 0; glint < 5; ++glint) {
    const int16_t y = 135 + glint * 19;
    const int16_t x = 80 + ((scene.phaseElapsed / 7 + glint * 61) % 170);
    canvas.fillCircle(x, y, 2 + (glint % 2), color);
  }
  canvas.fillRoundRect(12, 15, 296, 104, 12, logoBackground);
  canvas.drawRoundRect(12, 15, 296, 104, 12, color);
  if (show > 0.7F) {
    drawCenteredText(reelEventHeadline(scene), 27, 4, color);
    drawReelEventTitle(scene, 74, eyeColor);
    drawCenteredText(scene.phase == ReelPhase::Attention ? "YOU'RE ON - PRESS"
                                                          : reelDetail(scene),
                     103, 1, color);
  }
}

// FLIPBOARD — three oversized mechanical rows snap into the status, project,
// and action. It is readable from across the room without losing Puck's eyes.
void drawDesignFlipboard(const ReelScene& scene) {
  const float show = reelEventVisibility(scene);
  drawDesignMinimal(scene);
  if (show < 0.01F) {
    return;
  }
  const uint16_t color = reelPhaseColor(scene);
  canvas.fillRect(0, 0, SCREEN_WIDTH, 190, logoBackground);
  char title[27];
  copyEllipsized(title, sizeof(title),
                 scene.eventTitle[0] ? scene.eventTitle : reelProject(scene),
                 24);
  const char* rows[3] = {
      reelEventHeadline(scene), title,
      scene.phase == ReelPhase::Attention ? "PRESS NOW" : reelDetail(scene)};
  for (uint8_t row = 0; row < 3; ++row) {
    const float rowShow = smoothStep((scene.phaseElapsed - row * 130) / 350.0F) *
                          show;
    const int16_t height = std::lround(52.0F * rowShow);
    const int16_t centerY = 31 + row * 58;
    canvas.fillRoundRect(7, centerY - height / 2, 306, height, 5,
                         row == 0 ? color : faceShadowColor);
    if (rowShow > 0.68F) {
      const uint8_t size = row == 0 ? 4 : row == 1 ? 2 : 1;
      drawCenteredText(rows[row], centerY - (size == 4 ? 14 : size == 2 ? 8 : 4),
                       size,
                       row == 0 ? logoBackground : eyeColor);
      canvas.drawFastHLine(13, centerY, 294,
                           row == 0 ? logoBackground : logoHighlightColor);
    }
  }
  drawWatchingEyes(scene, 151);
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
  drawCenteredText(reelEventHeadline(scene),
                   top + (scene.phase == ReelPhase::Attention ? 16 : 11),
                   reelHeadlineSize(scene), logoBackground);
  if (scene.phase == ReelPhase::Message) {
    const int16_t panelY = 66 - std::lround(exit * 180.0F);
    canvas.fillRoundRect(14, panelY, 292, 68, 8, logoBackground);
    canvas.drawRoundRect(14, panelY, 292, 68, 8, unreadColor);
    drawReelEventTitle(scene, panelY + 6, unreadColor);
    return;
  }
  const int16_t bottom = std::lround(mix(240.0F, 184.0F, show));
  canvas.fillRect(0, bottom, SCREEN_WIDTH, 56, color);
  const uint8_t detailSize = std::strlen(reelDetail(scene)) <= 24 ? 2 : 1;
  drawCenteredText(reelDetail(scene), bottom + (detailSize == 2 ? 13 : 18),
                   detailSize, logoBackground);
  if ((show > 0.72F || exit > 0.0F) && scene.eventTitle[0]) {
    char title[37];
    copyEllipsized(title, sizeof(title), scene.eventTitle, 34);
    const int16_t titleY = 148 + std::lround(exit * 100.0F);
    canvas.fillRoundRect(30, titleY, 260, 24, 6, logoBackground);
    canvas.drawRoundRect(30, titleY, 260, 24, 6, color);
    drawCenteredText(title, titleY + 8, 1, color);
  }
}

// Design 1 — WATCH: an editorial notification takeover. Puck is fully
// present by default; important events temporarily sink the face until only
// its watching eyes remain beneath one large, legible thread story.
void drawDesignWatch(const ReelScene& scene) {
  const float takeover = reelTakeoverVisibility(scene);
  FacePose pose = reelPose(scene);
  pose.yOffset += 126.0F * takeover;
  pose.gazeY = mix(pose.gazeY, -0.78F, takeover);
  drawFace(pose);
  if (takeover > 0.01F) {
    drawTakeoverCopy(scene, takeover);
  } else {
    drawWatchShelf(scene);
    if (scene.phase == ReelPhase::Attention) {
      const int16_t edge = 2 + std::lround(scene.beat * 3.0F);
      canvas.fillRect(0, 0, SCREEN_WIDTH, edge, accentColor);
    }
  }
}

void drawQueueRow(int16_t y, const char* state, const char* title,
                  uint16_t color, bool highlighted) {
  canvas.fillRoundRect(10, y, 300, 40, 5,
                       highlighted ? faceShadowColor : logoBackground);
  canvas.fillRect(10, y, 5, 40, color);
  canvas.setFont();
  canvas.setTextSize(1);
  canvas.setTextColor(color);
  canvas.setCursor(24, y + 6);
  canvas.print(state);
  char clipped[43];
  copyEllipsized(clipped, sizeof(clipped), title, 40);
  canvas.setTextColor(eyeColor);
  canvas.setCursor(24, y + 23);
  canvas.print(clipped);
}

uint8_t reelThreadPriority(const AmpThreadSummary& thread) {
  if (stateNeedsAttention(thread.state)) {
    return 4;
  }
  if (thread.unread) {
    return 3;
  }
  if (stateIsWorking(thread.state)) {
    return 2;
  }
  return 1;
}

void reelThreadStatus(const AmpThreadSummary& thread, char* status,
                      size_t statusSize) {
  if (stateNeedsAttention(thread.state)) {
    threadStatusLabel(thread, status, statusSize);
    return;
  }
  if (thread.unread) {
    std::snprintf(status, statusSize, "%s", "NEW MESSAGE");
    return;
  }
  threadStatusLabel(thread, status, statusSize);
}

uint16_t reelThreadColor(const AmpThreadSummary& thread) {
  if (stateNeedsAttention(thread.state)) {
    return accentColor;
  }
  if (thread.unread) {
    return unreadColor;
  }
  if (stateIsWorking(thread.state)) {
    return reelGreenColor();
  }
  return textureColor;
}

// Design 2 — QUEUE: the same watching-eyes transition opens a compact
// priority queue instead of one headline, testing whether multiple active
// threads can remain understandable during escalation.
void drawDesignQueue(const ReelScene& scene) {
  const float takeover = reelTakeoverVisibility(scene);
  FacePose pose = reelPose(scene);
  pose.yOffset += 116.0F * takeover;
  pose.gazeY = mix(pose.gazeY, -0.72F, takeover);
  drawFace(pose);
  if (takeover < 0.82F) {
    const ReelStatus status = reelStatus(scene);
    canvas.fillCircle(13, 18, 4, status.color);
    canvas.setFont();
    canvas.setTextSize(2);
    canvas.setTextColor(status.color);
    canvas.setCursor(25, 10);
    canvas.print(status.text);
    canvas.setTextSize(1);
    canvas.setTextColor(eyeColor);
    canvas.setCursor(25, 35);
    if (scene.eventTitle[0]) {
      char title[43];
      copyEllipsized(title, sizeof(title), scene.eventTitle, 40);
      canvas.print(title);
    } else {
      if (scene.phase == ReelPhase::Resolved) {
        canvas.print("Queue cleared");
      } else {
        char monitored[24];
        std::snprintf(monitored, sizeof(monitored), "%u thread%s monitored",
                      scene.stats.total, scene.stats.total == 1 ? "" : "s");
        canvas.print(monitored);
      }
    }
    return;
  }

  canvas.fillRect(0, 0, SCREEN_WIDTH, 174, logoBackground);
  const ReelStatus status = reelStatus(scene);
  canvas.fillRect(0, 0, SCREEN_WIDTH, 3, status.color);
  canvas.setFont();
  canvas.setTextSize(2);
  canvas.setTextColor(status.color);
  canvas.setCursor(10, 8);
  canvas.print(scene.phase == ReelPhase::Attention ? "PRIORITY QUEUE"
                                                   : "THREAD QUEUE");
  canvas.setTextSize(1);
  canvas.setTextColor(textureColor);
  char liveCount[12];
  std::snprintf(liveCount, sizeof(liveCount), "%u LIVE",
                scene.stats.threadCount);
  canvas.setCursor(310 - std::strlen(liveCount) * 6, 14);
  canvas.print(liveCount);

  bool drawn[AMP_THREAD_SUMMARY_LIMIT] = {};
  const uint8_t rowCount = std::min<uint8_t>(3, scene.stats.threadCount);
  for (uint8_t row = 0; row < rowCount; ++row) {
    int8_t best = -1;
    for (uint8_t index = 0; index < scene.stats.threadCount; ++index) {
      if (!drawn[index] &&
          (best < 0 || reelThreadPriority(scene.stats.threads[index]) >
                           reelThreadPriority(scene.stats.threads[best]))) {
        best = index;
      }
    }
    if (best < 0) {
      break;
    }
    drawn[best] = true;
    const AmpThreadSummary& thread = scene.stats.threads[best];
    char status[32];
    reelThreadStatus(thread, status, sizeof(status));
    drawQueueRow(34 + row * 44, status, thread.title, reelThreadColor(thread),
                 row == 0);
  }
}

void drawDesignSidebar(const ReelScene& scene) {
  FacePose pose = reelPose(scene);
  pose.xOffset = -54.0F;
  drawFace(pose);

  constexpr int16_t panelX = 212;
  canvas.fillRect(panelX, 0, SCREEN_WIDTH - panelX, SCREEN_HEIGHT,
                  logoBackground);
  canvas.drawFastVLine(panelX, 0, SCREEN_HEIGHT, logoHighlightColor);
  canvas.drawFastVLine(panelX + 2, 0, SCREEN_HEIGHT, faceShadowColor);

  struct Row {
    uint16_t count;
    const char* label;
    uint16_t color;
  };
  const Row rows[4] = {
      {scene.stats.needsAttention, "NEED YOU", accentColor},
      {scene.stats.unread, "NEW MSGS", unreadColor},
      {scene.stats.working, "WORKING", reelGreenColor()},
      {scene.stats.idle, "IDLE", eyeColor},
  };
  int8_t activeRow = -1;
  if (scene.phase == ReelPhase::Attention) {
    activeRow = 0;
  } else if (scene.phase == ReelPhase::Message) {
    activeRow = 1;
  } else if (scene.phase == ReelPhase::Working) {
    activeRow = 2;
  }
  for (uint8_t row = 0; row < 4; ++row) {
    const int16_t y = 8 + row * 58;
    const bool active = rows[row].count > 0;
    const bool highlighted = row == activeRow;
    if (highlighted) {
      canvas.fillRoundRect(panelX + 4, y, 102, 52, 6, rows[row].color);
    } else {
      canvas.fillRect(panelX + 4, y + 2, 4, 48,
                      active ? rows[row].color : logoHighlightColor);
    }
    const uint16_t numberColor =
        highlighted ? logoBackground
                    : (active ? rows[row].color : logoHighlightColor);
    const uint16_t labelColor =
        highlighted ? logoBackground
                    : (active ? eyeColor : logoHighlightColor);
    char count[8];
    std::snprintf(count, sizeof(count), "%u", rows[row].count);
    canvas.setFont();
    canvas.setTextSize(3);
    canvas.setTextColor(numberColor);
    canvas.setCursor(panelX + 17, y + 2);
    canvas.print(count);
    canvas.setTextSize(1);
    canvas.setTextColor(labelColor);
    canvas.setCursor(panelX + 17, y + 36);
    canvas.print(rows[row].label);
  }
}

void drawDesignComic(const ReelScene& scene) {
  FacePose pose = reelPose(scene);
  pose.yOffset += 12.0F;
  drawFace(pose);
  if (scene.intro < 0.15F) {
    return;
  }

  const bool urgent = scene.phase == ReelPhase::Attention;
  const uint16_t paper = display.color565(239, 236, 203);
  const uint16_t ink = display.color565(35, 37, 25);
  const uint16_t bubbleColor = urgent ? accentColor : paper;
  const int16_t y = std::lround(mix(-22.0F, 8.0F, scene.intro));
  canvas.fillRoundRect(20, y, 280, 62, 12, bubbleColor);
  canvas.fillTriangle(145, y + 60, 175, y + 60, 160, y + 84,
                      bubbleColor);
  const char* speech = "All quiet out there.";
  if (scene.phase == ReelPhase::Working) speech = "On it! 2 running.";
  if (scene.phase == ReelPhase::Message) speech = "You've got mail!";
  if (scene.phase == ReelPhase::Attention) speech = "Hey! Need you here!";
  if (scene.phase == ReelPhase::Resolved) speech = "All wrapped up!";
  drawCenteredText(speech, y + 11, 2, ink);
  char detail[43];
  if (scene.eventTitle[0]) {
    copyEllipsized(detail, sizeof(detail), scene.eventTitle, 40);
  } else {
    std::snprintf(detail, sizeof(detail), "%u threads resting",
                  scene.stats.total);
  }
  drawCenteredText(detail, y + 40, 1, urgent ? ink : pupilColor);
}

float reelCardSettle(uint32_t elapsed) {
  const float t = elapsed;
  return 1.0F - std::exp(-t * 0.0065F) * std::cos(t * 0.013F);
}

// Design 5 — CARDS: notifications now behave as one physical stack. Each new
// sheet drops from above, pushes older sheets down, and progressively lowers
// Puck; all sheets leave together before the face springs back.
void drawDesignCards(const ReelScene& scene) {
  struct Card {
    const char* label;
    const char* title;
    uint16_t color;
  };
  Card cards[3] = {};
  char workingLabel[24];
  char messageLabel[24];
  char attentionLabel[24];
  std::snprintf(workingLabel, sizeof(workingLabel), "%u THREAD%s ACTIVE",
                scene.stats.working, scene.stats.working == 1 ? "" : "S");
  std::snprintf(messageLabel, sizeof(messageLabel), "%u NEW MESSAGE%s",
                scene.stats.unread, scene.stats.unread == 1 ? "" : "S");
  std::snprintf(attentionLabel, sizeof(attentionLabel), "%u NEED YOU",
                scene.stats.needsAttention);
  uint8_t cardCount = 0;
  const auto addCard = [&](const char* label, ReelPhase phase,
                           uint16_t color) {
    const AmpThreadSummary* thread =
        reelThreadForPhase(scene.stats, phase);
    cards[cardCount++] = {
        label,
        thread ? thread->title
               : (scene.eventTitle[0] ? scene.eventTitle : "Amp thread"),
        color,
    };
  };
  if (scene.stats.working > 0) {
    addCard(workingLabel, ReelPhase::Working, reelGreenColor());
  }
  if (scene.stats.unread > 0 && cardCount < 3) {
    addCard(messageLabel, ReelPhase::Message, unreadColor);
  }
  if (scene.stats.needsAttention > 0 && cardCount < 3) {
    addCard(attentionLabel, ReelPhase::Attention, accentColor);
  }
  if (scene.phase == ReelPhase::Resolved && DESIGN_REEL_SCRIPTED) {
    cardCount = 0;
    cards[cardCount++] = {"THREAD ACTIVE", scene.stats.threads[0].title,
                          reelGreenColor()};
    cards[cardCount++] = {"NEW MESSAGE", scene.stats.threads[0].title,
                          unreadColor};
    cards[cardCount++] = {"NEEDS YOU", scene.stats.threads[1].title,
                          accentColor};
  }
  const uint8_t previousCount =
      scene.phase == ReelPhase::Resolved
          ? cardCount
          : (cardCount > 0 ? cardCount - 1 : 0);

  const float settle = reelCardSettle(scene.phaseElapsed);
  const float dismiss = scene.phase == ReelPhase::Resolved
                            ? smoothStep(scene.phaseElapsed / 850.0F)
                            : 0.0F;
  float depth = 0.0F;
  if (scene.phase == ReelPhase::Resolved && DESIGN_REEL_SCRIPTED) {
    depth = 3.0F * (1.0F - dismiss);
  } else if (cardCount > 0) {
    depth = previousCount + settle;
  }

  FacePose pose = reelPose(scene);
  pose.yOffset += depth * 38.0F;
  pose.gazeY = mix(pose.gazeY, -0.72F, clamp01(depth / 3.0F));
  drawFace(pose);

  for (uint8_t i = 0; i < cardCount; ++i) {
    const bool entering = i == cardCount - 1 && cardCount > previousCount;
    const int16_t oldSlot = previousCount > i ? previousCount - 1 - i : 0;
    const int16_t newSlot = cardCount - 1 - i;
    const float slot = entering ? 0.0F : mix(oldSlot, newSlot, settle);
    const int16_t settledY = 8 + std::lround(slot * 14.0F);
    const int16_t y = entering
                          ? std::lround(mix(-90.0F, settledY, settle))
                          : settledY;
    const int16_t x = std::lround(dismiss * (360.0F + i * 22.0F));
    canvas.fillRoundRect(x + 13, y + 4, 294, 82, 8, logoBackground);
    canvas.fillRoundRect(x + 10, y, 294, 82, 8, faceShadowColor);
    canvas.fillRect(x + 10, y, 6, 82, cards[i].color);
    canvas.setFont();
    canvas.setTextSize(2);
    canvas.setTextColor(cards[i].color);
    canvas.setCursor(x + 26, y + 10);
    canvas.print(cards[i].label);
    char firstLine[25];
    char secondLine[25];
    splitTitle(cards[i].title, firstLine, secondLine);
    canvas.setTextSize(1);
    canvas.setTextColor(eyeColor);
    canvas.setCursor(x + 26, y + 39);
    canvas.print(firstLine);
    if (secondLine[0]) {
      canvas.setCursor(x + 26, y + 55);
      canvas.print(secondLine);
    }
  }
  if (scene.phase == ReelPhase::Idle) {
    canvas.drawFastVLine(160, 198, 34, logoHighlightColor);
    const auto drawCount = [&](int16_t centerX, uint16_t value,
                               const char* label, uint16_t color) {
      char count[8];
      std::snprintf(count, sizeof(count), "%u", value);
      canvas.setFont();
      canvas.setTextSize(2);
      canvas.setTextColor(value ? color : textureColor);
      canvas.setCursor(centerX - std::strlen(count) * 6, 199);
      canvas.print(count);
      canvas.setTextSize(1);
      canvas.setCursor(centerX - std::strlen(label) * 3, 222);
      canvas.print(label);
    };
    drawCount(80, scene.stats.working, "WORKING", reelGreenColor());
    drawCount(240, scene.stats.unread, "NEW MESSAGES", unreadColor);
  } else if (scene.phase == ReelPhase::Resolved && dismiss > 0.72F) {
    const uint16_t green = reelGreenColor();
    canvas.fillCircle(160, 23, 13, green);
    drawThickLine(153, 23, 158, 28, logoBackground, 3);
    drawThickLine(158, 28, 168, 17, logoBackground, 3);
    drawCenteredText("STACK CLEARED", 45, 1, green);
  }
}

// Design 6 — FORECAST: weather-like motion appears only when it carries
// state. Directional green current means work, blue rain means a message, and
// amber lightning means action; explicit thread copy stays in the foreground.
uint32_t reelHash(uint32_t value) {
  value ^= value >> 16;
  value *= 2654435761U;
  value ^= value >> 13;
  return value;
}

void drawDesignForecast(const ReelScene& scene, uint32_t elapsed) {
  const float takeover = reelTakeoverVisibility(scene);
  FacePose pose = reelPose(scene);
  pose.yOffset += 120.0F * takeover;
  pose.gazeY = mix(pose.gazeY, -0.76F, takeover);
  drawFace(pose);

  const uint16_t green = reelGreenColor();
  switch (scene.phase) {
    case ReelPhase::Idle:
      break;
    case ReelPhase::Working:
      for (uint8_t i = 0; i < 9; ++i) {
        const uint32_t h = reelHash(i + 40);
        const float progress = (((elapsed / 7) + (h % 500)) % 500) / 500.0F;
        const int16_t x = -20 + std::lround(progress * 380.0F);
        const int16_t y = 18 + ((h >> 9) % 158);
        canvas.drawLine(x, y, x + 11, y - 5, green);
      }
      break;
    case ReelPhase::Message:
      for (uint8_t i = 0; i < 12; ++i) {
        const uint32_t h = reelHash(i + 80);
        const float fall = (((elapsed / 4) + (h % 500)) % 500) / 500.0F;
        canvas.drawFastVLine(h % 314, std::lround(fall * 235.0F) - 10, 7,
                             unreadColor);
      }
      break;
    case ReelPhase::Attention: {
      const uint16_t hot = display.color565(255, 194, 67);
      for (uint8_t i = 0; i < 7; ++i) {
        const uint32_t h = reelHash(i + 120);
        const int16_t x = 8 + (h % 300);
        const int16_t y = 10 + ((h >> 8) % 205);
        const int16_t kick = std::lround(scene.beat * 6.0F);
        canvas.drawLine(x, y, x + 7 + kick, y + 5,
                        i % 2 ? accentColor : hot);
        canvas.drawLine(x + 7 + kick, y + 5, x + 3, y + 11,
                        i % 2 ? accentColor : hot);
      }
      break;
    }
    case ReelPhase::Resolved:
      break;
  }

  if (takeover >= 0.82F) {
    const ReelStatus status = reelStatus(scene);
    canvas.fillRoundRect(18, 15, 284, 142, 10, logoBackground);
    canvas.drawRoundRect(18, 15, 284, 142, 10, status.color);
    drawCenteredText(status.text, 28, 2, status.color);
    char firstLine[25];
    char secondLine[25];
    splitTitle(scene.eventTitle, firstLine, secondLine);
    drawCenteredText(firstLine, 66, 2, eyeColor);
    if (secondLine[0]) {
      drawCenteredText(secondLine, 88, 2, eyeColor);
    }
    drawCenteredText(reelDetail(scene), 124, 1, eyeColor);
  } else {
    const ReelStatus status = reelStatus(scene);
    char forecast[28];
    if (scene.phase == ReelPhase::Working) {
      std::snprintf(forecast, sizeof(forecast), "CURRENT - %u WORKING",
                    scene.stats.working);
    } else if (scene.phase == ReelPhase::Message) {
      std::snprintf(forecast, sizeof(forecast), "RAIN - %u NEW MESSAGE%s",
                    scene.stats.unread, scene.stats.unread == 1 ? "" : "S");
    } else if (scene.phase == ReelPhase::Attention) {
      std::snprintf(forecast, sizeof(forecast), "STORM - %u NEED YOU",
                    scene.stats.needsAttention);
    } else if (scene.phase == ReelPhase::Resolved) {
      std::snprintf(forecast, sizeof(forecast), "%s", "SKIES CLEAR");
    } else {
      std::snprintf(forecast, sizeof(forecast), "CALM - %u IDLE",
                    scene.stats.idle);
    }
    drawCenteredText(forecast, 207, 2, status.color);
    if (scene.eventTitle[0]) {
      char title[43];
      copyEllipsized(title, sizeof(title), scene.eventTitle, 40);
      drawCenteredText(title, 230, 1, eyeColor);
    } else {
      drawCenteredText(scene.phase == ReelPhase::Resolved
                           ? "Nothing needs you"
                           : "No active weather",
                       230, 1, textureColor);
    }
  }
}

void drawReelOverlay(uint32_t now) {
  if (now - reelModeChangedAt >= REEL_OVERLAY_MS) {
    return;
  }
  if (fontSelecting) {
    char fontLabel[24];
    std::snprintf(fontLabel, sizeof(fontLabel), "%u/%u %s",
                  selectedFont + 1, FONT_COUNT, FONT_NAMES[selectedFont]);
    canvas.fillRoundRect(4, 3, 170, 24, 5, logoBackground);
    canvas.drawRoundRect(4, 3, 170, 24, 5, textureColor);
    drawCenteredText(fontLabel, 6, 2, eyeColor);
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
      (reelModeSelecting || fontSelecting) ? now - reelSelectionStartedAt
                                          : elapsed;
  const bool scripted = DESIGN_REEL_SCRIPTED || reelModeSelecting ||
                        fontSelecting;
  const ReelScene scene = scripted ? reelScene(reelElapsed)
                                   : liveReelScene(now);
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
// ================== END DESIGN REEL drawing ================================

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
  drawStatsPanel(getAmpStats());
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

  Preferences preferences;
  preferences.begin("pocketpuck", false);
  const uint8_t savedReelVersion = preferences.getUChar("designVer", 0);
  const uint8_t savedReelMode = preferences.getUChar("design", 0);
  const uint8_t savedFont = preferences.getUChar("font", 0);
  blinkingDisabled = preferences.getBool("noBlink", false);
  reelMode = savedReelVersion == REEL_VERSION &&
                     savedReelMode < REEL_MODE_COUNT
                 ? savedReelMode
                 : 0;
  selectedFont = savedFont < FONT_COUNT ? savedFont : 0;
  preferences.end();
  Serial.printf("Saved design: %u/%u %s\n", reelMode + 1, REEL_MODE_COUNT,
                REEL_MODE_NAMES[reelMode]);
  Serial.printf("Saved font: %u/%u %s\n", selectedFont + 1, FONT_COUNT,
                FONT_NAMES[selectedFont]);
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
  beginAmpStats();
  drawDemoFrame(0);
  lastFrameAt = millis();
}

void loop() {
  const uint32_t now = millis();
  updateControls(now);
  updateAmpStats(now);
  updateNotifications(now);
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
  const uint32_t notificationElapsed = now - notificationStartedAt;
  if (uiPage == UiPage::Face && !DESIGN_REEL_ENABLED &&
      notificationKind != NotificationKind::None &&
      notificationElapsed < NOTIFICATION_DURATION_MS) {
    if (notificationKind == NotificationKind::Attention) {
      drawAttentionPulse(notificationElapsed);
    } else {
      drawStatusNotification(notificationElapsed, notificationKind,
                             notificationThreadTitle, getAmpStats());
    }
    pushCanvas();
  } else if (uiPage == UiPage::MainMenu) {
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
    if (DESIGN_REEL_ENABLED) {
      // DESIGN REEL: temporary replacement for the normal face screen.
      drawDesignReelFrame(mainElapsed, now);
    } else {
      notificationKind = NotificationKind::None;
      drawDemoFrame(mainElapsed);
    }
  }
}
