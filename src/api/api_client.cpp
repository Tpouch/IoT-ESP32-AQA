#include "api_client.h"

#include <WiFi.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "api/commands.h"
#include "api/tls.h"
#include "hal/hal.h"
#include "network/network_manager.h"
#include "utils/config_manager.h"
#include "utils/logger.h"
#include "utils/runtime_config.h"

void ApiClient::begin(FrameQueue& frameQueue) {
    queue = &frameQueue;
    secure = strncmp(Config().getApiUrl(), "http://", 7) != 0;
    if (!secure) {
        LOG_W("api: plain http:// API_URL, token and data travel unencrypted (local tests only)");
    }
    configureTls(tls);
    tls.setTimeout(8);  // seconds
    http.setReuse(true);
    // Worst case per request: 6 s connect + 10 s TLS handshake + 8 s response
    // = 24 s, under the 30 s loop watchdog (fed before every request).
    http.setConnectTimeout(6000);
    http.setTimeout(8000);

    if (Config().getDeviceToken()[0] == '\0') {
        LOG_W("api: DEVICE_TOKEN is empty; the server will refuse the data (set it in .env)");
    }
    LOG_I("api: reporting to %s as %s", Config().getApiUrl(), Config().getSensorId());
}

String ApiClient::devicePath(const char* suffix) const {
    return String("/api/v1/devices/") + Config().getSensorId() + suffix;
}

ApiClient::Result ApiClient::post(const String& path, const JsonDocument& body,
                                  JsonDocument* response) {
    esp_task_wdt_reset();  // acks can chain several requests in one loop()
    String url = String(Config().getApiUrl()) + path;
    String payload;
    serializeJson(body, payload);

    bool begun = secure ? http.begin(tls, url) : http.begin(plain, url);
    if (!begun) {
        LOG_E("api: bad URL %s", url.c_str());
        return Result::RETRY;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + Config().getDeviceToken());
    http.addHeader("X-Firmware-Version", Config().getFirmwareVersion());

    int code = http.POST(payload);
    String reply = code > 0 ? http.getString() : String();
    http.end();

    if (code <= 0) {
        // Negative codes are transport errors (DNS, TLS, timeout).
        LOG_W("api: POST %s failed: %s", path.c_str(), HTTPClient::errorToString(code).c_str());
        stateText = "unreachable";
        return Result::RETRY;
    }
    if (code >= 200 && code < 300) {
        stateText = "ok";
        if (response && reply.length() > 0) {
            if (deserializeJson(*response, reply) != DeserializationError::Ok) {
                LOG_W("api: unparseable response to %s", path.c_str());
                response->clear();
            }
        }
        return Result::OK;
    }

    LOG_W("api: POST %s -> HTTP %d %s", path.c_str(), code, reply.substring(0, 120).c_str());
    if (code == 401 || code == 403) {
        stateText = "token refused";
        LOG_E("api: token refused; check DEVICE_TOKEN in .env and the server's device list");
        return Result::RETRY;
    }
    if (code == 400 || code == 413 || code == 422) {
        stateText = "rejected";
        return Result::REJECTED;  // retrying the same body would fail forever
    }
    stateText = "server error";
    return Result::RETRY;  // 404, 429, 5xx, Cloudflare 52x...
}

void ApiClient::backoff() {
    retryDelayMs = retryDelayMs == 0 ? RETRY_MIN_MS : min(retryDelayMs * 2, RETRY_MAX_MS);
    nextAttemptAt = millis() + retryDelayMs;
    LOG_I("api: next attempt in %lu s", retryDelayMs / 1000);
}

bool ApiClient::sendStatus() {
    HAL& hal = HAL::getInstance();
    RuntimeConfig& rc = RuntimeConfig::getInstance();
    NetworkManager& net = NetworkManager::getInstance();

    JsonDocument doc;
    if (NetworkManager::timeSynced()) doc["t"] = (uint32_t)time(nullptr); else doc["t"] = nullptr;
    doc["uptime_s"] = millis() / 1000;
    doc["boot_count"] = rc.getBootCount();
    doc["firmware_version"] = Config().getFirmwareVersion();
    doc["hardware_version"] = Config().getHardwareVersion();
    doc["ip"] = net.ip();
    doc["rssi"] = net.rssi();
    doc["ssid"] = WiFi.SSID();
    doc["geohash"] = rc.getGeohash();
    doc["free_heap"] = ESP.getFreeHeap();

    JsonObject sensors = doc["sensors"].to<JsonObject>();
    sensors["temperature"] = hal.isSensorInitialized(SENSOR_TEMPERATURE);
    if (hal.isSensorInitialized(SENSOR_CO2)) {
        sensors["co2"] = hal.getCO2Sensor().typeName();
    } else {
        sensors["co2"] = nullptr;
    }
    sensors["pm"] = hal.isSensorInitialized(SENSOR_PM);
    sensors["sound"] = hal.isSensorInitialized(SENSOR_SOUND);

    JsonObject q = doc["queue"].to<JsonObject>();
    q["pending"] = queue->size();
    q["dropped"] = queue->droppedCount();

    rc.getParametersJson(doc["parameters"].to<JsonObject>());
    String last = rc.getLastCommandId();
    if (last.length()) doc["last_command_id"] = last; else doc["last_command_id"] = nullptr;

    JsonDocument response;
    Result r = post(devicePath("/status"), doc, &response);
    if (r == Result::RETRY) return false;
    statusDue = false;
    lastStatusAt = millis();
    if (r == Result::OK) runCommands(response);
    return true;
}

bool ApiClient::sendFrames() {
    size_t n = queue->size() < MAX_BATCH ? queue->size() : MAX_BATCH;
    uint32_t now = millis();

    JsonDocument doc;
    doc["geohash"] = RuntimeConfig::getInstance().getGeohash();
    JsonArray frames = doc["frames"].to<JsonArray>();
    for (size_t i = 0; i < n; i++) {
        queue->at(i).toJson(frames.add<JsonObject>(), now);
    }

    JsonDocument response;
    Result r = post(devicePath("/frames"), doc, &response);
    if (r == Result::RETRY) return false;
    if (r == Result::REJECTED) {
        LOG_E("api: server rejected %u frame(s); dropping them", (unsigned)n);
    }
    queue->popFront(n);
    if (r == Result::OK) {
        LOG_I("api: %u frame(s) uploaded, %u pending", (unsigned)n, (unsigned)queue->size());
        runCommands(response);
    }
    return true;
}

void ApiClient::runCommands(JsonDocument& response) {
    JsonArray commands = response["commands"].as<JsonArray>();
    if (commands.isNull()) return;
    for (JsonObject cmd : commands) {
        Commands::execute(cmd);
    }
}

bool ApiClient::ack(const char* commandId, const char* status, int progress, const char* detail) {
    if (!commandId || !*commandId) return false;
    JsonDocument doc;
    doc["status"] = status;
    doc["progress"] = progress;
    if (detail && *detail) doc["detail"] = detail; else doc["detail"] = nullptr;
    doc["t"] = NetworkManager::timeSynced() ? (uint32_t)time(nullptr) : 0;
    String path = devicePath("/commands/") + commandId + "/ack";
    return post(path, doc, nullptr) != Result::RETRY;
}

void ApiClient::releaseConnection() {
    http.end();
    tls.stop();
    plain.stop();
}

void ApiClient::loop() {
    Commands::loop();  // deferred reboots

    if (!queue || !NetworkManager::getInstance().isWifiConnected()) {
        stateText = "offline";
        return;
    }
    unsigned long now = millis();
    if (nextAttemptAt != 0 && (long)(now - nextAttemptAt) < 0) {
        return;
    }

    bool statusWanted = statusDue || now - lastStatusAt >= STATUS_INTERVAL_MS;
    bool ok;
    if (statusWanted) {
        ok = sendStatus();
    } else if (!queue->empty()) {
        ok = sendFrames();
    } else {
        return;
    }

    if (ok) {
        retryDelayMs = 0;
        nextAttemptAt = 0;
    } else {
        backoff();
    }
}
