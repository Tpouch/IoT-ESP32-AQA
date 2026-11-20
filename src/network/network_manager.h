#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

#include <memory>

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

    // Starts joining in the background and returns at once: sampling must not
    // wait for Wi-Fi. If no configured network answers, loop() opens the
    // captive portal (non-blocking too), then keeps retrying every 30 s.
    void initWiFi(int portalTimeoutSeconds = 180);

    // Never blocks: drives the join / portal / retry state machine.
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

    enum class State { JOINING, PORTAL, CONNECTED, OFFLINE };

    static WifiCreds compiledCreds();
    static WifiCreds activeCreds();
    static bool usable(const WifiCreds& c);
    void beginJoin(const WifiCreds& creds);           // returns at once
    bool tryJoin(const WifiCreds& creds, unsigned long timeoutMs);  // blocking, for switchNetwork
    void loadCandidates();
    bool joinNextCandidate();
    void startPortal();
    void closePortal();
    void enter(State next);
    void startNtp();
    void reportDisconnectReason();
    static const char* reasonHint(uint8_t reason);

    // Last STA_DISCONNECTED reason, written by the Wi-Fi event task and
    // logged from loop() (logging from the event task would race the logger).
    static volatile uint8_t lastReason;
    uint8_t reportedReason = 0;
    bool eventHooked = false;

    State state = State::OFFLINE;
    unsigned long stateSince = 0;

    // Networks to try, in order: the one set remotely, then the .env one.
    WifiCreds candidates[2];
    int candidateCount = 0;
    int candidateIndex = 0;

    std::unique_ptr<WiFiManager> portal;
    int portalTimeoutSeconds = 180;
    bool portalOpened = false;  // once per boot

    bool ntpStarted = false;

    static const unsigned long JOIN_TIMEOUT_MS = 15000;
    static const unsigned long RETRY_INTERVAL_MS = 30000;
    static const unsigned long SWITCH_TIMEOUT_MS = 20000;
};
