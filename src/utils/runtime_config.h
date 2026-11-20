#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

class RuntimeConfig {
public:
    static RuntimeConfig& getInstance() {
        static RuntimeConfig instance;
        return instance;
    }

    void begin();

    // Geohash of the node's location, sent with every frame batch.
    String getGeohash() const;
    void setGeohash(const char* geohash);

    // Last command id executed, persisted so a command the server delivers
    // twice (e.g. "restart", whose ack may be lost in the reboot) runs once.
    String getLastCommandId() const;
    void setLastCommandId(const char* id);

    // Parameters (float key-values from the "config" command)
    bool getParameter(const char* key, float* out) const;
    void setParameter(const char* key, float value);
    void setParametersFromJson(JsonObject params);
    void getParametersJson(JsonObject out) const;

    // Boot count (for rollback logic and status payload)
    int getBootCount() const;
    void incrementBootCount();

private:
    RuntimeConfig() = default;
    RuntimeConfig(const RuntimeConfig&) = delete;
    RuntimeConfig& operator=(const RuntimeConfig&) = delete;

    static const size_t GEOHASH_MAX_LEN = 32;
    static const char* NVS_DEVICE;
    static const char* NVS_PARAMS;
    static const char* KEY_GEOHASH;
    static const char* KEY_BOOT_COUNT;
    static const char* KEY_PARAMS_JSON;
    static const char* KEY_LAST_COMMAND;
};
