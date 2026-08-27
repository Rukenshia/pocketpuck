#pragma once

#include <Arduino.h>

constexpr uint8_t AMP_THREAD_SUMMARY_LIMIT = 4;
constexpr size_t AMP_THREAD_ID_LENGTH = 40;
constexpr size_t AMP_THREAD_TITLE_LENGTH = 49;
constexpr size_t AMP_THREAD_PROJECT_LENGTH = 25;
constexpr size_t AMP_THREAD_STATE_LENGTH = 20;

struct AmpThreadSummary {
  char id[AMP_THREAD_ID_LENGTH] = "";
  char title[AMP_THREAD_TITLE_LENGTH] = "";
  char project[AMP_THREAD_PROJECT_LENGTH] = "";
  char state[AMP_THREAD_STATE_LENGTH] = "idle";
  bool executorConnected = false;
  bool unread = false;
  bool shipping = false;
  bool shipped = false;
};

struct AmpStatsSnapshot {
  bool configured = false;
  bool wifiConnected = false;
  bool available = false;
  bool reconnecting = false;
  bool initialAttemptComplete = false;
  bool attentionAvailable = false;
  bool unreadAvailable = false;
  bool shippingAvailable = false;
  bool shippedAvailable = false;
  uint16_t working = 0;
  uint16_t needsAttention = 0;
  uint16_t unread = 0;
  uint16_t shipping = 0;
  uint16_t shipped = 0;
  uint16_t idle = 0;
  uint16_t total = 0;
  uint8_t threadCount = 0;
  AmpThreadSummary threads[AMP_THREAD_SUMMARY_LIMIT];
};

void beginAmpStats();
void updateAmpStats(uint32_t now);
AmpStatsSnapshot getAmpStats();
