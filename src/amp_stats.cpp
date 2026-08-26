#include "amp_stats.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#if __has_include("network_config.h")
#include "network_config.h"
#else
#define POCKETPUCK_WIFI_SSID ""
#define POCKETPUCK_WIFI_PASSWORD ""
#define POCKETPUCK_STATS_URL ""
#endif

namespace {

constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
constexpr uint32_t STATS_POLL_INTERVAL_MS = 10000;
constexpr uint16_t HTTP_TIMEOUT_MS = 3000;

AmpStatsSnapshot stats;
uint32_t lastWifiAttemptAt = 0;
uint32_t lastStatsPollAt = 0;

bool hasConfiguration() {
  return POCKETPUCK_WIFI_SSID[0] != '\0' && POCKETPUCK_STATS_URL[0] != '\0';
}

void connectWifi(uint32_t now) {
  lastWifiAttemptAt = now;
  Serial.printf("Connecting to WiFi %s\n", POCKETPUCK_WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(POCKETPUCK_WIFI_SSID, POCKETPUCK_WIFI_PASSWORD);
}

void fetchStats(uint32_t now) {
  lastStatsPollAt = now;

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(POCKETPUCK_STATS_URL)) {
    Serial.println("Unable to initialize the stats request");
    stats.available = false;
    return;
  }

  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    Serial.printf("Stats request failed: HTTP %d\n", statusCode);
    stats.available = false;
    http.end();
    return;
  }

  JsonDocument response;
  const DeserializationError error = deserializeJson(response, http.getStream());
  http.end();
  if (error || !response["running"].is<uint16_t>() ||
      !response["idle"].is<uint16_t>()) {
    Serial.printf("Invalid stats response: %s\n", error.c_str());
    stats.available = false;
    return;
  }

  if (response["reconnecting"] | false) {
    Serial.println("Amp bridge is reconnecting");
    stats.available = false;
    return;
  }

  stats.running = response["running"].as<uint16_t>();
  stats.idle = response["idle"].as<uint16_t>();
  stats.available = true;
  Serial.printf("Amp threads: %u running, %u idle\n", stats.running,
                stats.idle);
}

}  // namespace

void beginAmpStats() {
  stats.configured = hasConfiguration();
  if (!stats.configured) {
    Serial.println("WiFi stats disabled: create include/network_config.h");
    return;
  }

  WiFi.mode(WIFI_STA);
  connectWifi(millis());
}

void updateAmpStats(uint32_t now) {
  if (!stats.configured) {
    return;
  }

  stats.wifiConnected = WiFi.status() == WL_CONNECTED;
  if (!stats.wifiConnected) {
    stats.available = false;
    if (now - lastWifiAttemptAt >= WIFI_RETRY_INTERVAL_MS) {
      connectWifi(now);
    }
    return;
  }

  if (lastStatsPollAt == 0 || now - lastStatsPollAt >= STATS_POLL_INTERVAL_MS) {
    fetchStats(now);
  }
}

AmpStatsSnapshot getAmpStats() { return stats; }
