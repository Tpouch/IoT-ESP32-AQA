#pragma once

#include <Arduino.h>

class ConfigManager;

extern ConfigManager& Config();

// Build-time configuration (secrets.h / .env) plus the MAC-derived identity.
// Values changed at runtime by commands live in RuntimeConfig (NVS).
class ConfigManager {
public:
    void init(const String& deviceID);

    const char* getSensorId() const { return sensorId.c_str(); }

    const char* getApiUrl() const { return apiUrl; }
    const char* getDeviceToken() const { return deviceToken; }

    int getHardwareVersion() const { return hardwareVersion; }
    const char* getFirmwareVersion() const { return firmwareVersion; }

private:
    ConfigManager();
    friend ConfigManager& Config();

    String sensorId;
    const char* apiUrl;
    const char* deviceToken;
    int hardwareVersion;
    const char* firmwareVersion;
};
