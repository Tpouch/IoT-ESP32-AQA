#include "config_manager.h"
#include "secrets.h"

ConfigManager::ConfigManager() :
    apiUrl(API_URL),
    deviceToken(DEVICE_TOKEN),
    hardwareVersion(HARDWARE_VERSION),
    firmwareVersion(FIRMWARE_VERSION)
{
}

void ConfigManager::init(const String& deviceID) {
    sensorId = String(SENSOR_PREFIX) + deviceID;
}

ConfigManager& Config() {
    static ConfigManager instance;
    return instance;
}
