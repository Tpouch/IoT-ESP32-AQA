#include "network_manager.h"

#include <esp_task_wdt.h>

#include "utils/runtime_config.h"
#include "utils/secrets.h"

String NetworkManager::generateDeviceID() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

WifiCreds NetworkManager::compiledCreds() {
    WifiCreds c;
    c.ssid = WIFI_SSID;
    c.password = WIFI_PASSWORD;
    c.eapUser = WIFI_EAP_USERNAME;
    c.eapPassword = WIFI_EAP_PASSWORD;
    c.eapIdentity = WIFI_EAP_IDENTITY;
    return c;
}

WifiCreds NetworkManager::activeCreds() {
    WifiCreds c;
    if (RuntimeConfig::getInstance().getWifiOverride(c)) return c;
    return compiledCreds();
}

bool NetworkManager::tryJoin(const WifiCreds& c, unsigned long timeoutMs) {
    if (c.ssid.length() == 0 || c.ssid == "CHANGE_ME") return false;

    WiFi.disconnect(true, true);  // drop the current association and its EAP state
    delay(100);
    WiFi.mode(WIFI_STA);

    if (c.isEnterprise()) {
        // WPA2-Enterprise, PEAP outer tunnel, MSCHAPv2 inside (the IDF default).
        const char* identity = c.eapIdentity.length() ? c.eapIdentity.c_str() : c.eapUser.c_str();
#ifdef WIFI_EAP_CA_PEM
        const char* caPem = WIFI_EAP_CA_PEM;
#else
        const char* caPem = nullptr;
        LOG_W("no WIFI_EAP_CA_FILE: the RADIUS server certificate is not verified");
#endif
        LOG_I("connecting to SSID %s (WPA2-Enterprise PEAP, user %s)", c.ssid.c_str(), c.eapUser.c_str());
        WiFi.begin(c.ssid.c_str(), WPA2_AUTH_PEAP, identity, c.eapUser.c_str(), c.eapPassword.c_str(), caPem);
    } else {
        LOG_I("connecting to SSID %s", c.ssid.c_str());
        WiFi.begin(c.ssid.c_str(), c.password.c_str());
    }

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        esp_task_wdt_reset();  // no-op before the watchdog is armed in setup()
        delay(250);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool NetworkManager::connectWithSecrets(unsigned long timeoutMs) {
    WifiCreds remote;
    bool hasRemote = RuntimeConfig::getInstance().getWifiOverride(remote);
    if (hasRemote) {
        if (tryJoin(remote, timeoutMs)) return true;
        // A network set remotely may have disappeared; the compiled one is the safety net.
        LOG_W("remotely configured SSID %s unreachable; trying the one from .env", remote.ssid.c_str());
    }
    WifiCreds compiled = compiledCreds();
    if (compiled.ssid == "CHANGE_ME") {
        LOG_I("no WIFI_SSID in .env; using the config portal");
        return false;
    }
    return tryJoin(compiled, timeoutMs);
}

bool NetworkManager::switchNetwork(const WifiCreds* candidate, String& error) {
    // candidate == nullptr: forget the remote network and go back to .env.
    WifiCreds previous = activeCreds();
    WifiCreds target = candidate ? *candidate : compiledCreds();

    if (tryJoin(target, SWITCH_TIMEOUT_MS)) {
        if (candidate) {
            RuntimeConfig::getInstance().setWifiOverride(*candidate);
        } else {
            RuntimeConfig::getInstance().clearWifiOverride();
        }
        LOG_I("now on SSID %s ip=%s rssi=%d dBm", target.ssid.c_str(), ip().c_str(), (int)rssi());
        return true;
    }

    error = String("could not join ") + target.ssid + "; back on " + previous.ssid;
    LOG_W("%s", error.c_str());
    if (!tryJoin(previous, SWITCH_TIMEOUT_MS)) {
        LOG_E("previous network unreachable too; loop() keeps retrying");
    }
    return false;
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
