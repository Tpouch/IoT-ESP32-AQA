#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

#include "utils/logger.h"

// Wi-Fi association (secrets/.env first, captive portal as fallback),
// reconnection and NTP. The HTTPS API client lives in src/api/.
class NetworkManager {
public:
    static NetworkManager& getInstance() {
        static NetworkManager instance;
        return instance;
    }

    bool initWiFi(int portalTimeoutSeconds = 180);

    // Non-blocking except for a bounded reconnect attempt every 30 s.
    void loop();

    bool isWifiConnected() { return WiFi.status() == WL_CONNECTED; }
    int32_t rssi() { return WiFi.RSSI(); }
    String ip() { return WiFi.localIP().toString(); }

    // True once NTP has answered: time(nullptr) is then real epoch seconds.
    static bool timeSynced();

    // MAC-derived, stable across reboots, unique per board.
    String generateDeviceID();

private:
    NetworkManager() = default;
    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    bool connectWithSecrets(unsigned long timeoutMs);
    bool connectWithPortal(int timeoutSeconds);
    void startNtp();

    bool ntpStarted = false;
    unsigned long lastWiFiReconnectAttempt = 0;
    int wifiReconnectCount = 0;

    static const unsigned long WIFI_RECONNECT_INTERVAL = 30000;
    static const int MAX_WIFI_RETRIES = 3;
};
