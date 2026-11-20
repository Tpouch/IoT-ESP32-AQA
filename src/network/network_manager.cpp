#include "network_manager.h"

#include "utils/secrets.h"

String NetworkManager::generateDeviceID() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

bool NetworkManager::connectWithSecrets(unsigned long timeoutMs) {
    if (String(WIFI_SSID) == "CHANGE_ME") {
        LOG_I("no WIFI_SSID in .env; using the config portal");
        return false;
    }

    WiFi.mode(WIFI_STA);

    if (strlen(WIFI_EAP_USERNAME) > 0) {
        // WPA2-Enterprise, PEAP outer tunnel, MSCHAPv2 inside (the IDF default).
        const char* identity = strlen(WIFI_EAP_IDENTITY) > 0 ? WIFI_EAP_IDENTITY
                                                             : WIFI_EAP_USERNAME;
#ifdef WIFI_EAP_CA_PEM
        const char* caPem = WIFI_EAP_CA_PEM;
#else
        const char* caPem = nullptr;
        LOG_W("no WIFI_EAP_CA_FILE: the RADIUS server certificate is not verified");
#endif
        LOG_I("connecting to SSID %s (WPA2-Enterprise PEAP, user %s)",
              WIFI_SSID, WIFI_EAP_USERNAME);
        WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, identity, WIFI_EAP_USERNAME,
                   WIFI_EAP_PASSWORD, caPem);
    } else {
        LOG_I("connecting to SSID %s", WIFI_SSID);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(250);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool NetworkManager::connectWithPortal(int timeoutSeconds) {
    WiFiManager wifiManager;
    wifiManager.setConfigPortalTimeout(timeoutSeconds);

    LOG_I("opening config portal 'SenPork-setup' for %d s", timeoutSeconds);
    if (!wifiManager.autoConnect("SenPork-setup")) {
        LOG_E("config portal timed out without credentials");
        return false;
    }
    return WiFi.status() == WL_CONNECTED;
}

bool NetworkManager::initWiFi(int portalTimeoutSeconds) {
    if (connectWithSecrets(15000)) {
        LOG_I("WiFi connected ip=%s rssi=%d dBm", ip().c_str(), (int)rssi());
        wifiReconnectCount = 0;
        startNtp();
        return true;
    }

    if (connectWithPortal(portalTimeoutSeconds)) {
        LOG_I("WiFi connected via portal ip=%s rssi=%d dBm",
              ip().c_str(), (int)rssi());
        wifiReconnectCount = 0;
        startNtp();
        return true;
    }

    LOG_E("WiFi unavailable; sampling continues offline");
    return false;
}

void NetworkManager::loop() {
    if (WiFi.status() != WL_CONNECTED) {
        unsigned long now = millis();
        if (now - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
            lastWiFiReconnectAttempt = now;
            LOG_W("WiFi lost; reconnect attempt %d of %d",
                  wifiReconnectCount + 1, MAX_WIFI_RETRIES);
            WiFi.reconnect();

            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
                delay(250);
            }

            if (WiFi.status() == WL_CONNECTED) {
                LOG_I("WiFi reconnected ip=%s rssi=%d dBm",
                      ip().c_str(), (int)rssi());
                wifiReconnectCount = 0;
                startNtp();
            } else {
                wifiReconnectCount++;
                if (wifiReconnectCount >= MAX_WIFI_RETRIES) {
                    LOG_E("%d failed reconnects; keeping the node alive", wifiReconnectCount);
                    wifiReconnectCount = 0;
                }
            }
        }
    }
}

void NetworkManager::startNtp() {
    // Once is enough: SNTP keeps resyncing in the background. Doing it on the
    // first successful join (not only at boot) covers a node that boots
    // offline and gets Wi-Fi later.
    if (ntpStarted) return;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    ntpStarted = true;
}

bool NetworkManager::timeSynced() {
    return time(nullptr) > 1600000000L;
}
