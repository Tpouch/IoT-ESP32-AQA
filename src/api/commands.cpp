#include "commands.h"

#include <Arduino.h>

#include "api/api_client.h"
#include "ota/ota_manager.h"
#include "utils/logger.h"
#include "utils/runtime_config.h"

namespace {

unsigned long rebootAt = 0;

void scheduleReboot() {
    rebootAt = millis() + 1500;
    if (rebootAt == 0) rebootAt = 1;
}

void ack(const char* id, const char* status, const char* detail = nullptr, int progress = 0) {
    ApiClient::getInstance().ack(id, status, progress, detail);
}

void otaProgress(int percent, void* ctx) {
    ack(static_cast<const char*>(ctx), "in_progress", nullptr, percent);
}

void runStatus(const char* id) {
    ApiClient::getInstance().requestStatus();
    ack(id, "completed");
}

void runConfig(const char* id, JsonObject params) {
    if (params.isNull() || params.size() == 0) {
        ack(id, "failed", "params must be a non-empty object of numbers");
        return;
    }
    RuntimeConfig::getInstance().setParametersFromJson(params);
    ApiClient::getInstance().requestStatus();  // report the new parameters
    ack(id, "completed");
}

void runGeohash(const char* id, JsonObject params) {
    const char* geohash = params["geohash"] | "";
    size_t len = strlen(geohash);
    if (len == 0 || len > 12) {
        ack(id, "failed", "params.geohash must be 1-12 characters");
        return;
    }
    RuntimeConfig::getInstance().setGeohash(geohash);
    LOG_I("geohash set to %s", geohash);
    ack(id, "completed");
}

void runRestart(const char* id) {
    ack(id, "completed", "rebooting");
    scheduleReboot();
}

void runOta(const char* id, JsonObject params) {
    const char* url = params["url"] | "";
    const char* sigUrl = params["signature_url"] | "";
    const char* version = params["version"] | "";
    if (!*url || !*sigUrl) {
        ack(id, "failed", "params.url and params.signature_url are required");
        return;
    }
    LOG_I("OTA to %s from %s", *version ? version : "?", url);
    ack(id, "in_progress", "downloading", 0);

    // Free the API's TLS session first: two at once is tight on heap.
    ApiClient::getInstance().releaseConnection();
    String error;
    // id lives in the response document, which outlives this call.
    bool ok = OTAManager::getInstance().install(url, sigUrl, error, otaProgress,
                                                const_cast<char*>(id));
    if (!ok) {
        LOG_E("OTA failed: %s", error.c_str());
        ack(id, "failed", error.c_str());
        return;
    }
    ack(id, "completed", "verified; rebooting into new image", 100);
    scheduleReboot();
}

void runRollback(const char* id) {
    String error;
    if (!OTAManager::getInstance().rollback(error)) {
        ack(id, "failed", error.c_str());
        return;
    }
    ack(id, "completed", "rebooting into previous image");
    scheduleReboot();
}

}  // namespace

void Commands::execute(JsonObject command) {
    const char* id = command["id"] | "";
    const char* type = command["type"] | "";
    JsonObject params = command["params"].as<JsonObject>();
    if (!*id || !*type) {
        LOG_W("command without id or type ignored");
        return;
    }
    LOG_I("command %s: %s", id, type);

    RuntimeConfig& rc = RuntimeConfig::getInstance();
    if (rc.getLastCommandId() == id) {
        ack(id, "completed", "already executed");
        return;
    }
    // Record before running: a restart must not run again if its ack is lost.
    rc.setLastCommandId(id);

    if (strcmp(type, "status") == 0)        runStatus(id);
    else if (strcmp(type, "config") == 0)   runConfig(id, params);
    else if (strcmp(type, "geohash") == 0)  runGeohash(id, params);
    else if (strcmp(type, "restart") == 0)  runRestart(id);
    else if (strcmp(type, "ota") == 0)      runOta(id, params);
    else if (strcmp(type, "rollback") == 0) runRollback(id);
    else ack(id, "failed", "unknown command type");
}

void Commands::loop() {
    if (rebootAt != 0 && (long)(millis() - rebootAt) >= 0) {
        LOG_I("rebooting");
        delay(100);
        ESP.restart();
    }
}
