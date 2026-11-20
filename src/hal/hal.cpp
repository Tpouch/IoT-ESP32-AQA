#include "hal.h"
#include "board/ttgo_t7_v14.h"
#include "../utils/logger.h"

HAL::HAL() : 
    tempSensorInitialized(false),
    co2SensorInitialized(false),
    pmSensorInitialized(false),
    soundSensorInitialized(false),
    co2SensorVerified(false),
    verifyCO2SensorTimestamp(0) {
}

bool HAL::init() {
    LOG_I("Initializing HAL...");
    bool success = true;

    // Temperature: SHT4x on I2C or DHT on GPIO 13, autodetected.
    if (!initTemperatureSensor(BoardPins::DHT_PIN, BoardPins::I2C_SCL)) {
        LOG_W("Failed to initialize temperature sensor");
        success = false;
    }

    if (!initCO2Sensor(Serial2, BoardPins::CO2_ESP_RX, BoardPins::CO2_ESP_TX)) {
        LOG_E("Failed to initialize CO2 sensor!");
        success = false;
    }

    if (!initPMSensor(BoardPins::PM_ESP_RX, BoardPins::PM_ESP_TX)) {
        LOG_E("Failed to initialize PM sensor!");
        success = false;
    }

    if (!initSoundSensor(BoardPins::ADC_SOUND)) {
        LOG_E("Failed to initialize sound sensor!");
        success = false;
    }

    if (success) {
        LOG_I("All sensors initialized successfully");
    } else {
        LOG_W("Some sensors failed to initialize");
    }

    return success;
}

// Called from the main loop. The only periodic HAL job is the one-shot CO2
// check once the sensor has had time to warm up.
void HAL::update() {
    if (co2SensorInitialized && !co2SensorVerified &&
        millis() - verifyCO2SensorTimestamp >= CO2_WARMUP_MS) {
        verifyCO2Sensor();
    }
}

void HAL::verifyCO2Sensor() {
    co2SensorVerified = true;  // one shot, whatever the outcome

    int ppm = 0;
    if (!co2Sensor.read(ppm, /*bypassBackoff=*/true)) {
        // Production firmware reboots here. On a teaching bench a missing or
        // cold sensor must not turn into a boot loop: report once and keep
        // sampling.
        LOG_E("CO2 check failed: no valid reading %lu s after init; check power and UART",
              CO2_WARMUP_MS / 1000);
        return;
    }

    LOG_I("CO2 check passed after warm-up: %d ppm", ppm);
}

bool HAL::isSensorInitialized(SensorType sensor) {
    switch (sensor) {
        case SENSOR_TEMPERATURE: return tempSensorInitialized;
        case SENSOR_CO2: return co2SensorInitialized;
        case SENSOR_PM: return pmSensorInitialized;
        case SENSOR_SOUND: return soundSensorInitialized;
        default: return false;
    }
}

bool HAL::initTemperatureSensor(int dhtPin, int sclPin) {
    LOG_I("Initializing temperature sensor...");
    
    for (int i = 0; i < MAX_INIT_RETRIES; i++) {
        try {
            // Auto-detect sensor type
            if (tempSensor.init(dhtPin, sclPin, TemperatureSensor::SensorType::NONE, true)) {
                // Test the sensor
                float temp, humidity;
                if (tempSensor.read(temp, humidity)) {
                    LOG_I("Temperature sensor initialized successfully: %.1f°C, %.1f%%", temp, humidity);
                    tempSensorInitialized = true;
                    return true;
                }
            }
            
            LOG_W("Temperature sensor initialization attempt %d failed, retrying...", i + 1);
            delay(100);
        } catch (...) {
            LOG_E("Exception during temperature sensor initialization");
        }
    }
    
    return false;
}

bool HAL::initCO2Sensor(HardwareSerial& serial, int espRx, int espTx,
                        CO2Sensor::SensorType co2Type) {
    LOG_I("Initializing CO2 sensor...");

    // Detection only needs the sensor to answer its protocol: right after
    // power-up both families report 0 ppm for a while, which is not a failure.
    for (int i = 0; i < CO2_INIT_ATTEMPTS; i++) {
        if (co2Sensor.init(serial, espRx, espTx, co2Type)) {
            co2SensorInitialized = true;
            co2SensorVerified = false;
            verifyCO2SensorTimestamp = millis();
            LOG_I("CO2 sensor (%s) ready; reading check in %lu s",
                  co2Sensor.typeName(), CO2_WARMUP_MS / 1000);
            return true;
        }
        LOG_W("CO2 sensor detection attempt %d failed", i + 1);
        delay(500);
    }

    co2SensorInitialized = false;
    return false;
}

bool HAL::initPMSensor(int espRx, int espTx) {
    LOG_I("Initializing PM sensor...");
    
    pmSensor.init(espRx, espTx);

    // The sensor streams on its own; one frame within the longest PMS7003
    // interval (2.3 s) is enough to call it present. Reopening the port
    // between attempts would not make it talk sooner.
    uint16_t pm25;
    if (pmSensor.read(pm25, PM_DETECT_TIMEOUT_MS)) {
        LOG_I("PM sensor initialized. Current PM2.5 reading: %u µg/m³", pm25);
        pmSensorInitialized = true;
        return true;
    }

    LOG_W("No PMS frame within %lu ms; check the PMS TX line and 5 V supply",
          PM_DETECT_TIMEOUT_MS);
    return false;
}

bool HAL::initSoundSensor(int adcPin) {
    LOG_I("Initializing sound sensor...");
    
    try {
        soundSensor.init(adcPin);
        
        // Test the sensor by reading values
        unsigned int peakToPeak;
        if (soundSensor.read(peakToPeak)) {
            LOG_I("Sound sensor initialized. Current reading: %u", peakToPeak);
            soundSensorInitialized = true;
            return true;
        }
    } catch (...) {
        LOG_E("Exception during sound sensor initialization");
    }
    
    return false;
}

CO2Sensor& HAL::getCO2Sensor() {
    if (!co2SensorInitialized) {
        LOG_W("Accessing uninitialized CO2 sensor!");
    }
    return co2Sensor;
}

PMSensor& HAL::getPMSensor() {
    if (!pmSensorInitialized) {
        LOG_W("Accessing uninitialized PM sensor!");
    }
    return pmSensor;
}

SoundSensor& HAL::getSoundSensor() {
    if (!soundSensorInitialized) {
        LOG_W("Accessing uninitialized sound sensor!");
    }
    return soundSensor;
}

TemperatureSensor& HAL::getTemperatureSensor() {
    if (!tempSensorInitialized) {
        LOG_W("Accessing uninitialized temperature sensor!");
    }
    return tempSensor;
} 