#include <Arduino.h>
#include <esp_task_wdt.h>

#include "api/api_client.h"
#include "api/frame_queue.h"
#include "data/data_manager.h"
#include "hal/hal.h"
#include "network/network_manager.h"
#include "ota/ota_manager.h"
#include "public_key.h"
#include "utils/config_manager.h"
#include "utils/logger.h"
#include "utils/runtime_config.h"
#include "utils/secrets.h"
#include "utils/task_manager.h"

// Sensors are sampled by TaskScheduler into 15 s windows; each window becomes
// a Frame in an in-RAM queue (6 h deep). The API client drains the queue over
// HTTPS in batches and runs the commands the server returns. See docs/api.md.

static const int HEARTBEAT_LED = LED_BUILTIN;
static const unsigned long STATUS_LOG_INTERVAL_MS = 10000;

static HAL& hal = HAL::getInstance();
static NetworkManager& network = NetworkManager::getInstance();
static ApiClient& api = ApiClient::getInstance();
static FrameQueue frameQueue;

static const char* ok(bool value) { return value ? "PASS" : "FAIL"; }

void setup() {
    Logger::getInstance().begin(115200, LOG_INFO);
    pinMode(HEARTBEAT_LED, OUTPUT);

    LOG_I("firmware %s on %s", FIRMWARE_VERSION, ARDUINO_BOARD);

    Config().init(network.generateDeviceID());
    RuntimeConfig::getInstance().begin();  // boot count
    LOG_I("sensor id %s", Config().getSensorId());

    if (!hal.init()) {
        LOG_W("some sensors failed to initialise; continuing with the rest");
    }
    LOG_I("self-test | temperature %s | co2 %s | pm %s | sound %s",
          ok(hal.isSensorInitialized(SENSOR_TEMPERATURE)),
          ok(hal.isSensorInitialized(SENSOR_CO2)),
          ok(hal.isSensorInitialized(SENSOR_PM)),
          ok(hal.isSensorInitialized(SENSOR_SOUND)));

    network.initWiFi();

    TaskManager::getInstance().init();
    DataManager::getInstance().init(hal, TaskManager::getInstance().getScheduler(), frameQueue);

    OTAManager::getInstance().init(public_key_der, public_key_der_len);
    api.begin(frameQueue);

    // Watchdog: if the loop stops feeding it for 30 s, the board reboots. Long
    // operations (TLS handshakes, OTA download) feed it or stay under that.
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);
}

void loop() {
    static unsigned long lastBlink = 0;
    static unsigned long lastStatusLog = 0;
    static bool ledOn = false;

    unsigned long now = millis();

    if (now - lastBlink >= 500) {
        lastBlink = now;
        ledOn = !ledOn;
        digitalWrite(HEARTBEAT_LED, ledOn ? HIGH : LOW);
    }

    network.loop();
    TaskManager::getInstance().execute();
    hal.update();
    esp_task_wdt_reset();

    api.loop();  // at most one HTTPS request per iteration
    esp_task_wdt_reset();

    if (now - lastStatusLog >= STATUS_LOG_INTERVAL_MS) {
        lastStatusLog = now;
        if (network.isWifiConnected()) {
            LOG_I("wifi ip=%s rssi=%d dBm api=%s queue=%u",
                  network.ip().c_str(), (int)network.rssi(), api.state(),
                  (unsigned)frameQueue.size());
        } else {
            LOG_W("wifi offline, queue=%u", (unsigned)frameQueue.size());
        }
    }
}
