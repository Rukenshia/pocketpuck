#include "amp_stats.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <cstring>

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
constexpr uint32_t INITIAL_ATTEMPT_TIMEOUT_MS = 20000;
constexpr uint16_t HTTP_TIMEOUT_MS = 3000;

AmpStatsSnapshot stats;
uint32_t statsStartedAt = 0;
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
    stats.reconnecting = false;
    return;
  }

  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    Serial.printf("Stats request failed: HTTP %d\n", statusCode);
    stats.available = false;
    stats.reconnecting = false;
    http.end();
    return;
  }

  JsonDocument response;
  const DeserializationError error = deserializeJson(response, http.getStream());
  http.end();
  if (error || !response["working"].is<uint16_t>() ||
      !response["needsAttention"].is<uint16_t>() ||
      !response["idle"].is<uint16_t>()) {
    Serial.printf("Invalid stats response: %s\n", error.c_str());
    stats.available = false;
    stats.reconnecting = false;
    return;
  }

  if (response["reconnecting"] | false) {
    Serial.println("Amp bridge is reconnecting");
    stats.available = false;
    stats.reconnecting = true;
    return;
  }

  stats.attentionAvailable = response["capabilities"]["detailedStates"] | false;
  if (stats.attentionAvailable) {
    stats.working = response["headline"]["working"] | 0;
    stats.needsAttention = response["headline"]["needsAttention"] | 0;
    stats.idle = response["headline"]["idle"] | 0;
  } else {
    stats.working = response["running"].as<uint16_t>();
    stats.needsAttention = 0;
    stats.idle = response["idle"].as<uint16_t>();
  }
  stats.unreadAvailable = response["capabilities"]["unread"] | false;
  stats.unread = stats.unreadAvailable ? response["unread"] | 0 : 0;
  stats.total = response["total"] | static_cast<uint16_t>(
                                      stats.working + stats.needsAttention +
                                      stats.idle);
  stats.threadCount = 0;
  for (JsonObject thread : response["items"].as<JsonArray>()) {
    if (stats.threadCount >= AMP_THREAD_SUMMARY_LIMIT) {
      break;
    }
    AmpThreadSummary& summary = stats.threads[stats.threadCount++];
    std::strncpy(summary.id, thread["id"] | "", sizeof(summary.id) - 1);
    summary.id[sizeof(summary.id) - 1] = '\0';
    std::strncpy(summary.title, thread["title"] | "Untitled thread",
                 sizeof(summary.title) - 1);
    summary.title[sizeof(summary.title) - 1] = '\0';
    std::strncpy(summary.project, thread["project"] | "",
                 sizeof(summary.project) - 1);
    summary.project[sizeof(summary.project) - 1] = '\0';
    std::strncpy(summary.state, thread["state"] | "idle",
                 sizeof(summary.state) - 1);
    summary.state[sizeof(summary.state) - 1] = '\0';
    summary.executorConnected = thread["executorConnected"] | false;
    summary.unread = thread["unread"] | false;
  }
  stats.reconnecting = false;
  stats.available = true;
  stats.initialAttemptComplete = true;
  Serial.printf(
      "Amp threads: %u working, %u need attention, %u unread, %u idle\n",
      stats.working, stats.needsAttention, stats.unread, stats.idle);
}

}  // namespace

void beginAmpStats() {
  statsStartedAt = millis();
  stats.configured = hasConfiguration();
  if (!stats.configured) {
    stats.initialAttemptComplete = true;
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

  if (!stats.initialAttemptComplete &&
      now - statsStartedAt >= INITIAL_ATTEMPT_TIMEOUT_MS) {
    stats.initialAttemptComplete = true;
    Serial.println("Initial Amp connection attempt timed out; continuing retries");
  }

  stats.wifiConnected = WiFi.status() == WL_CONNECTED;
  if (!stats.wifiConnected) {
    stats.available = false;
    stats.reconnecting = false;
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
