#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

#include "utils/logger.h"

// One set of Wi-Fi credentials: WPA2-Personal, or WPA2-Enterprise (PEAP)
// when eapUser is set.
struct WifiCreds {
    String ssid;
    String password;
    String eapUser;
    String eapPassword;
    String eapIdentity;
    bool isEnterprise() const { return eapUser.length() > 0; }
};

// Wi-Fi association (network set remotely, then .env, then captive portal),
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

    // Moves to another network (the "wifi" command). Persists it only if the
    // join succeeds; otherwise rejoins the previous network and returns false.
    // candidate == nullptr goes back to the .env network.
    bool switchNetwork(const WifiCreds* candidate, String& error);

    // True once NTP has answered: time(nullptr) is then real epoch seconds.
    static bool timeSynced();

    // MAC-derived, stable across reboots, unique per board.
    String generateDeviceID();

private:
    NetworkManager() = default;
    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    static WifiCreds compiledCreds();
    static WifiCreds activeCreds();
    bool tryJoin(const WifiCreds& creds, unsigned long timeoutMs);
    bool connectWithSecrets(unsigned long timeoutMs);
    bool connectWithPortal(int timeoutSeconds);
    void startNtp();

    bool ntpStarted = false;
    unsigned long lastWiFiReconnectAttempt = 0;
    int wifiReconnectCount = 0;

    static const unsigned long WIFI_RECONNECT_INTERVAL = 30000;
    static const int MAX_WIFI_RETRIES = 3;
    static const unsigned long SWITCH_TIMEOUT_MS = 20000;
};
