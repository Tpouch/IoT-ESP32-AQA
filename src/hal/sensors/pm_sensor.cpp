#include "pm_sensor.h"
#include "../../utils/logger.h"

bool PMSensor::init(int rxPin, int txPin) {
    serialPort.reset(new SoftwareSerial());
    // An input-only txPin (GPIO 34..39) is silently skipped by EspSoftwareSerial:
    // RX still works, commands to the sensor just go nowhere.
    serialPort->begin(9600, SWSERIAL_8N1, rxPin, txPin, false, RX_BUFFER_BYTES);

    pms.reset(new PMS(*serialPort));
    pms->wakeUp();
    lastFrameMs = millis();

    LOG_I("PM sensor initialized on pins RX:%d, TX:%d", rxPin, txPin);
    return true;
}

bool PMSensor::read(uint16_t& pm25, unsigned long timeoutMs) {
    if (!pms) {
        LOG_E("PM sensor not initialized");
        return false;
    }

    bool got = false;
    unsigned long start = millis();
    for (;;) {
        // PMS::read() consumes one byte per call; drain the buffer and keep the
        // newest complete frame.
        while (serialPort->available()) {
            if (!pms->read(data)) {
                continue;
            }
            if (data.PM_AE_UG_2_5 > MAX_PLAUSIBLE_UG) {
                LOG_W("PM sensor reading out of range: %u µg/m³", data.PM_AE_UG_2_5);
                continue;
            }
            pm25 = data.PM_AE_UG_2_5;
            lastFrameMs = millis();
            got = true;
        }
        if (got || millis() - start >= timeoutMs) {
            break;
        }
        delay(10);
    }

    if (got) {
        LOG_V("PM2.5 reading: %u µg/m³", pm25);
    }
    return got;
}

unsigned long PMSensor::msSinceLastFrame() const {
    return millis() - lastFrameMs;
}

void PMSensor::wakeUp() {
    if (pms) {
        pms->wakeUp();
    } else {
        LOG_W("Cannot wake up uninitialized PM sensor");
    }
}

void PMSensor::sleep() {
    if (pms) {
        pms->sleep();
    } else {
        LOG_W("Cannot sleep uninitialized PM sensor");
    }
}
