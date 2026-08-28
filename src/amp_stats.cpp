#include "amp_stats.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

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
constexpr uint32_t STATS_TASK_STACK_BYTES = 8192;

AmpStatsSnapshot publishedStats;
AmpStatsSnapshot workerStats;
SemaphoreHandle_t statsMutex = nullptr;
uint32_t statsStartedAt = 0;
uint32_t lastWifiAttemptAt = 0;
uint32_t lastStatsPollAt = 0;

bool hasConfiguration() {
  return POCKETPUCK_WIFI_SSID[0] != '\0' && POCKETPUCK_STATS_URL[0] != '\0';
}

template <size_t Size>
void copyJsonString(char (&output)[Size], JsonVariantConst value,
                    const char* fallback = "") {
  std::strncpy(output, value | fallback, Size - 1);
  output[Size - 1] = '\0';
}

void publishStats() {
  if (!statsMutex) {
    publishedStats = workerStats;
    return;
  }
  if (xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    publishedStats = workerStats;
    xSemaphoreGive(statsMutex);
  }
}

void connectWifi(uint32_t now) {
  lastWifiAttemptAt = now;
  Serial.printf("Connecting to WiFi %s\n", POCKETPUCK_WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(POCKETPUCK_WIFI_SSID, POCKETPUCK_WIFI_PASSWORD);
}

void fetchStats() {
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(POCKETPUCK_STATS_URL)) {
    Serial.println("Unable to initialize the stats request");
    workerStats.available = false;
    workerStats.reconnecting = false;
    return;
  }

  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    Serial.printf("Stats request failed: HTTP %d\n", statusCode);
    workerStats.available = false;
    workerStats.reconnecting = false;
    http.end();
    return;
  }

  JsonDocument response;
  const DeserializationError error = deserializeJson(response, http.getStream());
  http.end();
  if (error || !response["working"].is<uint16_t>() ||
      !response["needsAttention"].is<uint16_t>() ||
      !response["idle"].is<uint16_t>()) {
    Serial.printf("Invalid stats response: %s\n",
                  error ? error.c_str() : "missing required counts");
    workerStats.available = false;
    workerStats.reconnecting = false;
    return;
  }

  if (response["reconnecting"] | false) {
    Serial.println("Amp bridge is reconnecting");
    workerStats.available = false;
    workerStats.reconnecting = true;
    return;
  }

  workerStats.attentionAvailable =
      response["capabilities"]["detailedStates"] | false;
  if (workerStats.attentionAvailable) {
    workerStats.working = response["headline"]["working"] | 0;
    workerStats.needsAttention = response["headline"]["needsAttention"] | 0;
    workerStats.idle = response["headline"]["idle"] | 0;
  } else {
    workerStats.working = response["running"].as<uint16_t>();
    workerStats.needsAttention = 0;
    workerStats.idle = response["idle"].as<uint16_t>();
  }
  workerStats.unreadAvailable = response["capabilities"]["unread"] | false;
  workerStats.unread =
      workerStats.unreadAvailable ? response["unread"] | 0 : 0;
  const bool shippingAvailable =
      response["capabilities"]["shipping"] | false;
  workerStats.shipping = shippingAvailable ? response["shipping"] | 0 : 0;
  workerStats.shippedAvailable = response["capabilities"]["shipped"] | false;
  workerStats.shipped =
      workerStats.shippedAvailable ? response["shipped"] | 0 : 0;
  workerStats.eventsAvailable = response["capabilities"]["events"] | false;
  workerStats.total = response["total"] | static_cast<uint16_t>(
                                            workerStats.working +
                                            workerStats.needsAttention +
                                            workerStats.idle);

  workerStats.threadCount = 0;
  for (JsonObjectConst thread : response["items"].as<JsonArrayConst>()) {
    if (workerStats.threadCount >= AMP_THREAD_SUMMARY_LIMIT) {
      break;
    }
    AmpThreadSummary& summary =
        workerStats.threads[workerStats.threadCount++];
    copyJsonString(summary.id, thread["id"]);
    copyJsonString(summary.title, thread["title"], "Untitled thread");
    copyJsonString(summary.project, thread["project"]);
    copyJsonString(summary.state, thread["state"], "idle");
    summary.unread = thread["unread"] | false;
    summary.shipping = thread["shipping"] | false;
    summary.shipped = thread["shipped"] | false;
    summary.projectResolved = thread["projectResolved"] | false;
  }

  workerStats.eventCount = 0;
  if (workerStats.eventsAvailable) {
    for (JsonObjectConst event : response["events"].as<JsonArrayConst>()) {
      if (workerStats.eventCount >= AMP_THREAD_EVENT_LIMIT) {
        break;
      }
      AmpThreadEvent& summary = workerStats.events[workerStats.eventCount++];
      copyJsonString(summary.id, event["id"]);
      copyJsonString(summary.title, event["title"], "Amp thread");
      copyJsonString(summary.state, event["state"]);
      copyJsonString(summary.kind, event["kind"]);
    }
  }

  workerStats.reconnecting = false;
  workerStats.available = true;
  workerStats.initialAttemptComplete = true;
  Serial.printf(
      "Amp threads: %u working, %u shipping, %u need attention, %u unread, %u idle\n",
      workerStats.working, workerStats.shipping, workerStats.needsAttention,
      workerStats.unread, workerStats.idle);
}

void runStatsTask(void*) {
  statsStartedAt = millis();
  WiFi.mode(WIFI_STA);
  connectWifi(statsStartedAt);
  publishStats();

  while (true) {
    const uint32_t now = millis();
    bool changed = false;
    if (!workerStats.initialAttemptComplete &&
        now - statsStartedAt >= INITIAL_ATTEMPT_TIMEOUT_MS) {
      workerStats.initialAttemptComplete = true;
      changed = true;
      Serial.println(
          "Initial Amp connection attempt timed out; continuing retries");
    }

    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (workerStats.wifiConnected != wifiConnected) {
      workerStats.wifiConnected = wifiConnected;
      changed = true;
    }
    if (!wifiConnected) {
      if (workerStats.available || workerStats.reconnecting) {
        workerStats.available = false;
        workerStats.reconnecting = false;
        changed = true;
      }
      if (now - lastWifiAttemptAt >= WIFI_RETRY_INTERVAL_MS) {
        connectWifi(now);
      }
      if (changed) publishStats();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (changed) publishStats();
    if (lastStatsPollAt == 0 ||
        now - lastStatsPollAt >= STATS_POLL_INTERVAL_MS) {
      lastStatsPollAt = now;
      fetchStats();
      publishStats();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // namespace

void beginAmpStats() {
  workerStats.configured = hasConfiguration();
  if (!workerStats.configured) {
    workerStats.initialAttemptComplete = true;
    publishedStats = workerStats;
    Serial.println("WiFi stats disabled: create include/network_config.h");
    return;
  }

  statsMutex = xSemaphoreCreateMutex();
  if (!statsMutex) {
    workerStats.initialAttemptComplete = true;
    publishedStats = workerStats;
    Serial.println("Unable to create the Amp stats mutex");
    return;
  }
  publishedStats = workerStats;
  if (xTaskCreate(runStatsTask, "amp-stats", STATS_TASK_STACK_BYTES, nullptr, 1,
                  nullptr) != pdPASS) {
    workerStats.initialAttemptComplete = true;
    publishedStats = workerStats;
    vSemaphoreDelete(statsMutex);
    statsMutex = nullptr;
    Serial.println("Unable to start the Amp stats task");
  }
}

AmpStatsSnapshot getAmpStats() {
  AmpStatsSnapshot snapshot;
  if (!statsMutex) {
    return publishedStats;
  }
  if (xSemaphoreTake(statsMutex, portMAX_DELAY) == pdTRUE) {
    snapshot = publishedStats;
    xSemaphoreGive(statsMutex);
  }
  return snapshot;
}
