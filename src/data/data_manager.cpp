#include "data_manager.h"
#include "network/network_manager.h"
#include "utils/task_manager.h"

#include <time.h>

// Initialize static member
DataManager* DataManager::instance = nullptr;

DataManager::DataManager() :
    scheduler(nullptr),
    hal(nullptr),
    humidity(0),
    temperature(0) {
    
    instance = this;
}

void DataManager::init(HAL& halInstance, Scheduler& taskRunner, FrameQueue& frameQueue) {
    hal = &halInstance;
    scheduler = &taskRunner;
    queue = &frameQueue;
    setupTasks();
}

void DataManager::setupTasks() {
    scheduler->addTask(soundSampleTask);
    scheduler->addTask(pmSampleTask);
    scheduler->addTask(htSampleTask);
    scheduler->addTask(co2SampleTask);
    scheduler->addTask(sendDataFrameTask);
    
    soundSampleTask.enable();
    pmSampleTask.enable();
    htSampleTask.enable();
    co2SampleTask.enable();
    sendDataFrameTask.enable();
}

void DataManager::loop() {
    // Let the main loop execute the scheduler
    // scheduler->execute();
}

// Static task callbacks
void DataManager::soundSampleCallback() {
    if (instance && instance->hal) {
        if (instance->hal->isSensorInitialized(SENSOR_SOUND)) {
            unsigned int peakToPeak;
            if (instance->hal->getSoundSensor().readWithRetry(peakToPeak, 3, 200)) {
                instance->soundSamples.push_back(peakToPeak);
                LOG_D("Sound level: %u", peakToPeak);
            } else {
                LOG_W("Sound sensor read failed after retries");
            }
        } else {
            LOG_V("Sound sensor not initialized, skipping reading");
        }
    }
}

void DataManager::pmSampleCallback() {
    if (instance && instance->hal) {
        uint16_t pm25;
        if (instance->hal->isSensorInitialized(SENSOR_PM)) {
            // Non-blocking: most ticks find no new frame, which is normal.
            PMSensor& pm = instance->hal->getPMSensor();
            if (pm.read(pm25)) {
                instance->pm25Samples.push_back(pm25);
                LOG_D("PM2.5: %u µg/m³", pm25);
                if (instance->pmStaleReported) {
                    LOG_I("PM sensor frames are back");
                    instance->pmStaleReported = false;
                }
            } else if (!instance->pmStaleReported &&
                       pm.msSinceLastFrame() > PM_STALE_MS) {
                LOG_W("No PM frame for %lu s; check the PMS TX line and 5 V supply",
                      PM_STALE_MS / 1000);
                instance->pmStaleReported = true;
            }
        } else {
            LOG_V("PM sensor not initialized, skipping reading");
        }
    }
}

void DataManager::co2SampleCallback() {
    if (instance && instance->hal) {
        if (instance->hal->isSensorInitialized(SENSOR_CO2)) {
            // One attempt per tick: the next tick is CO2_SAMPLE_TIME away, and
            // retrying here would block the sound and PM tasks for seconds.
            int co2;
            if (instance->hal->getCO2Sensor().read(co2)) {
                instance->co2Samples.push_back(co2);
                LOG_D("CO2: %d ppm", co2);
            }
        } else {
            LOG_V("CO2 sensor not initialized, skipping reading");
        }
    }
}

void DataManager::htSampleCallback() {
    if (instance && instance->hal) {
        if (instance->hal->isSensorInitialized(SENSOR_TEMPERATURE)) {
            float temp, humidity;
            if (instance->hal->getTemperatureSensor().readWithRetry(temp, humidity, 3, 200)) {
                // Keep the precision the sensor gives: truncating to an integer
                // loses 0.1 C and 0.1 %RH on every frame.
                instance->temperature = temp;
                instance->humidity = humidity;
                instance->haveHumTemp = true;
                LOG_D("Temperature: %.1f°C, Humidity: %.1f%%", temp, humidity);
            } else {
                LOG_W("Temperature sensor read failed after retries");
            }
        } else {
            LOG_V("Temperature sensor not initialized, skipping reading");
        }
    }
}

void DataManager::sendDataFrameCallback() {
    if (instance) {
        instance->createDataFrame();
    }
}

bool DataManager::takeAverage(std::vector<unsigned int>& samples, uint16_t& out) {
    if (samples.empty()) return false;
    double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
    samples.clear();
    out = (uint16_t)lround(mean);
    return true;
}

void DataManager::createDataFrame() {
    // Sound, PM and CO2 are means over the 15 s window; temperature and
    // humidity are the latest reading (one sample per window). A sensor with
    // no sample in the window is sent as null, never as 0.
    Frame frame;
    frame.capturedAtMs = millis();
    if (NetworkManager::timeSynced()) {
        frame.epoch = (uint32_t)time(nullptr);
    }
    bool any = false;
    any |= takeAverage(soundSamples, frame.snd);
    any |= takeAverage(co2Samples, frame.co2);
    any |= takeAverage(pm25Samples, frame.pm25);
    if (haveHumTemp) {
        frame.hum10 = (int16_t)lroundf(humidity * 10.0f);
        frame.temp10 = (int16_t)lroundf(temperature * 10.0f);
        any = true;
    }

    // Frozen-value detection: identical readings for three frames in a row
    // are suspicious (stuck driver, unplugged sensor returning last value).
    bool frozen = haveLastFrame &&
                  frame.snd == lastFrame.snd && frame.co2 == lastFrame.co2 &&
                  frame.pm25 == lastFrame.pm25 && frame.hum10 == lastFrame.hum10 &&
                  frame.temp10 == lastFrame.temp10;
    frozenFrames = frozen ? frozenFrames + 1 : 0;
    frame.stale = frozenFrames >= 2;
    if (frame.stale) {
        LOG_W("values unchanged for %u frames; flagging stale", frozenFrames + 1);
    }
    lastFrame = frame;
    haveLastFrame = true;

    if (!any) {
        LOG_W("frame built with no samples from any sensor");
    }

    queue->push(frame);

    // Same JSON as sent to the API; tools/aqa_tui.py parses this line.
    JsonDocument doc;
    frame.toJson(doc.to<JsonObject>(), frame.capturedAtMs);
    String json;
    serializeJson(doc, json);
    LOG_I("frame queued (%u pending): %s", (unsigned)queue->size(), json.c_str());
}
