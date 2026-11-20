#include "network_manager.h"

#include <esp_task_wdt.h>

#include "utils/runtime_config.h"
#include "utils/secrets.h"

volatile uint8_t NetworkManager::lastReason = 0;

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

bool NetworkManager::usable(const WifiCreds& c) {
    return c.ssid.length() > 0 && c.ssid != "CHANGE_ME";
}

void NetworkManager::beginJoin(const WifiCreds& c) {
    WiFi.disconnect(true, true);  // drop the current association and its EAP state
    delay(100);
    WiFi.mode(WIFI_STA);
    lastReason = 0;
    reportedReason = 0;

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
}

bool NetworkManager::tryJoin(const WifiCreds& c, unsigned long timeoutMs) {
    if (!usable(c)) return false;
    beginJoin(c);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        esp_task_wdt_reset();
        delay(250);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool NetworkManager::switchNetwork(const WifiCreds* candidate, String& error) {
    // candidate == nullptr: forget the remote network and go back to .env.
    // Blocking on purpose: the command must ack with the outcome.
    WifiCreds previous = activeCreds();
    WifiCreds target = candidate ? *candidate : compiledCreds();

    if (tryJoin(target, SWITCH_TIMEOUT_MS)) {
        if (candidate) {
            RuntimeConfig::getInstance().setWifiOverride(*candidate);
        } else {
            RuntimeConfig::getInstance().clearWifiOverride();
        }
        LOG_I("now on SSID %s ip=%s rssi=%d dBm", target.ssid.c_str(), ip().c_str(), (int)rssi());
        enter(State::CONNECTED);
        return true;
    }

    error = String("could not join ") + target.ssid + "; back on " + previous.ssid;
    LOG_W("%s", error.c_str());
    if (tryJoin(previous, SWITCH_TIMEOUT_MS)) {
        enter(State::CONNECTED);
    } else {
        LOG_E("previous network unreachable too; retrying in the background");
        enter(State::OFFLINE);
    }
    return false;
}

void NetworkManager::loadCandidates() {
    candidateCount = 0;
    candidateIndex = 0;
    WifiCreds remote;
    if (RuntimeConfig::getInstance().getWifiOverride(remote) && usable(remote)) {
        candidates[candidateCount++] = remote;
    }
    WifiCreds compiled = compiledCreds();
    if (usable(compiled)) {
        candidates[candidateCount++] = compiled;
    }
}

bool NetworkManager::joinNextCandidate() {
    if (candidateIndex >= candidateCount) return false;
    beginJoin(candidates[candidateIndex++]);
    enter(State::JOINING);
    return true;
}

void NetworkManager::startPortal() {
    portalOpened = true;
    portal.reset(new WiFiManager());
    portal->setConfigPortalBlocking(false);
    portal->setConfigPortalTimeout(portalTimeoutSeconds);
    LOG_I("opening config portal 'SenPork-setup' for %d s; sensors keep sampling",
          portalTimeoutSeconds);
    portal->startConfigPortal("SenPork-setup");
    enter(State::PORTAL);
}

void NetworkManager::closePortal() {
    if (!portal) return;
    if (portal->getConfigPortalActive()) portal->stopConfigPortal();
    portal.reset();
}

void NetworkManager::enter(State next) {
    state = next;
    stateSince = millis();
}

void NetworkManager::initWiFi(int portalTimeout) {
    portalTimeoutSeconds = portalTimeout;
    if (!eventHooked) {
        WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
            lastReason = info.wifi_sta_disconnected.reason;
        }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        eventHooked = true;
    }
    WiFi.setAutoReconnect(true);
    loadCandidates();
    if (!joinNextCandidate()) {
        LOG_I("no WIFI_SSID in .env; using the config portal");
        startPortal();
    }
}

void NetworkManager::loop() {
    if (portal) portal->process();  // serves the portal page, handles its timeout
    reportDisconnectReason();

    if (isWifiConnected()) {
        if (state != State::CONNECTED) {
            if (state == State::PORTAL) {
                // Keep what was typed in the portal: the next join wipes the
                // driver's stored config, the remote-override slot survives.
                WifiCreds typed;
                typed.ssid = WiFi.SSID();
                typed.password = WiFi.psk();
                RuntimeConfig::getInstance().setWifiOverride(typed);
                closePortal();
            }
            LOG_I("WiFi connected ssid=%s ip=%s rssi=%d dBm",
                  WiFi.SSID().c_str(), ip().c_str(), (int)rssi());
            enter(State::CONNECTED);
            startNtp();
        }
        return;
    }

    unsigned long elapsed = millis() - stateSince;
    switch (state) {
    case State::CONNECTED:
        // The driver auto-reconnects; we only take over if that fails.
        LOG_W("WiFi lost; sampling continues, frames queue until it is back");
        enter(State::OFFLINE);
        break;

    case State::JOINING:
        if (elapsed < JOIN_TIMEOUT_MS) break;
        LOG_W("no answer from SSID %s after %lu s (last reason: %s)",
              candidates[candidateIndex - 1].ssid.c_str(), JOIN_TIMEOUT_MS / 1000,
              lastReason ? WiFi.disconnectReasonName((wifi_err_reason_t)lastReason)
                         : "none, the AP never answered");
        if (joinNextCandidate()) break;
        if (!portalOpened) {
            startPortal();
        } else {
            LOG_W("WiFi unavailable; sampling continues offline, retry in %lu s",
                  RETRY_INTERVAL_MS / 1000);
            enter(State::OFFLINE);
        }
        break;

    case State::PORTAL:
        if (portal && portal->getConfigPortalActive()) break;
        LOG_W("config portal closed without credentials; sampling continues offline");
        closePortal();
        enter(State::OFFLINE);
        break;

    case State::OFFLINE:
        if (elapsed < RETRY_INTERVAL_MS) break;
        loadCandidates();
        if (!joinNextCandidate()) enter(State::OFFLINE);  // nothing configured: wait again
        break;
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

void NetworkManager::reportDisconnectReason() {
    uint8_t reason = lastReason;
    if (reason == 0 || reason == reportedReason) return;  // once per distinct reason and join
    reportedReason = reason;
    LOG_W("wifi disconnect reason %u (%s): %s", reason,
          WiFi.disconnectReasonName((wifi_err_reason_t)reason), reasonHint(reason));
}

const char* NetworkManager::reasonHint(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "SSID not seen: check the exact name (case, spaces) and that it is on 2.4 GHz";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "authentication refused: check WIFI_EAP_USERNAME / WIFI_EAP_PASSWORD";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "no handshake: wrong password, or the network does not use PEAP/MSCHAPv2";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "association refused by the AP (MAC filter or too many clients?)";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal lost: board too far from the AP";
    default:
        return "see esp_wifi_types.h";
    }
}
