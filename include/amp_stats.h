#pragma once

#include <Arduino.h>

struct AmpStatsSnapshot {
  bool configured = false;
  bool wifiConnected = false;
  bool available = false;
  uint16_t running = 0;
  uint16_t idle = 0;
};

void beginAmpStats();
void updateAmpStats(uint32_t now);
AmpStatsSnapshot getAmpStats();
