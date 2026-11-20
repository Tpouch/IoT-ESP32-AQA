#include "co2_sensor.h"
#include "../../utils/logger.h"

static bool plausible(int ppm) {
    return ppm >= CO2Sensor::MIN_PPM && ppm <= CO2Sensor::MAX_PPM;
}

// ---------------------------------------------------------------- MH-Z19

bool MHZ19Sensor::probe() {
    // begin() sends a verify command and records the reply in errorCode.
    mhz19.begin(stream);
    if (mhz19.errorCode != RESULT_OK) {
        return false;
    }
    // Automatic baseline correction (24 h cycle), as on the production node.
    mhz19.autoCalibration(true);
    return true;
}

ICO2Sensor::ReadResult MHZ19Sensor::read(int& ppm) {
    // The "limited" 0x86 command is the one every MH-Z19 revision and clone
    // implements; the library's default 0x85 "unlimited" command is not.
    int value = mhz19.getCO2(false, true);
    if (mhz19.errorCode != RESULT_OK) {
        LOG_W("MH-Z19 read failed (error code %d)", mhz19.errorCode);
        return ReadResult::NO_REPLY;
    }
    if (!plausible(value)) {
        LOG_D("MH-Z19 returned %d ppm (warming up?)", value);
        return ReadResult::NOT_READY;
    }
    ppm = value;
    return ReadResult::OK;
}

bool MHZ19Sensor::zeroPointCalibration() {
    mhz19.calibrate();
    return mhz19.errorCode == RESULT_OK;
}

// ---------------------------------------------------------------- SenseAir S8

bool SenseAirS8Sensor::probe() {
    // A firmware-version query is answered even during warm-up, unlike ppm.
    char version[S8_LEN_FIRMVER + 1] = {0};
    s8->get_firmware_version(version);
    if (version[0] == '\0') {
        return false;
    }
    LOG_I("SenseAir S8 firmware %s, ABC period %d h", version, s8->get_ABC_period());
    return true;
}

ICO2Sensor::ReadResult SenseAirS8Sensor::read(int& ppm) {
    // The library returns 0 both on timeout and on a bad frame, so 0 cannot be
    // told apart from silence and is treated as no reply.
    int value = s8->get_co2();
    if (value == 0) {
        LOG_W("SenseAir S8 read failed (no valid reply)");
        return ReadResult::NO_REPLY;
    }
    if (!plausible(value)) {
        LOG_D("SenseAir S8 returned %d ppm", value);
        return ReadResult::NOT_READY;
    }
    ppm = value;
    return ReadResult::OK;
}

bool SenseAirS8Sensor::zeroPointCalibration() {
    return s8->manual_calibration();
}

// ---------------------------------------------------------------- front-end

void CO2Sensor::drain(HardwareSerial& serial) {
    while (serial.available()) {
        serial.read();
    }
}

bool CO2Sensor::tryType(SensorType candidate, HardwareSerial& serial) {
    std::unique_ptr<ICO2Sensor> driver;
    switch (candidate) {
        case SensorType::MHZ19:       driver.reset(new MHZ19Sensor(serial)); break;
        case SensorType::SENSEAIR_S8: driver.reset(new SenseAirS8Sensor(serial)); break;
        default: return false;
    }

    drain(serial);  // leftovers from a previous probe would corrupt the reply
    if (!driver->probe()) {
        LOG_D("no %s on the CO2 UART", driver->name());
        return false;
    }

    sensor = std::move(driver);
    type = candidate;
    return true;
}

bool CO2Sensor::init(HardwareSerial& serial, int espRx, int espTx, SensorType requested) {
    sensor.reset();
    type = SensorType::NONE;
    consecutiveFailures = 0;
    backoffUntil = 0;

    serial.begin(9600, SERIAL_8N1, espRx, espTx);
    LOG_I("CO2 UART on ESP RX=%d TX=%d", espRx, espTx);

    bool found;
    if (requested == SensorType::NONE) {
        found = tryType(SensorType::MHZ19, serial) ||
                tryType(SensorType::SENSEAIR_S8, serial);
    } else {
        found = tryType(requested, serial);
    }

    if (!found) {
        LOG_E("no CO2 sensor answered; check power and that sensor TX goes to GPIO %d", espRx);
        return false;
    }

    LOG_I("CO2 sensor detected: %s", sensor->name());
    return true;
}

bool CO2Sensor::read(int& ppm, bool bypassBackoff) {
    if (!sensor) {
        return false;
    }

    unsigned long now = millis();
    if (backoffUntil != 0 && !bypassBackoff && (long)(now - backoffUntil) < 0) {
        return false;
    }
    backoffUntil = 0;

    ICO2Sensor::ReadResult result = sensor->read(ppm);
    if (result != ICO2Sensor::ReadResult::NO_REPLY) {
        if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
            LOG_I("%s answering again", sensor->name());
        }
        consecutiveFailures = 0;
        return result == ICO2Sensor::ReadResult::OK;
    }

    if (consecutiveFailures < 255) {
        consecutiveFailures++;
    }
    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        if (consecutiveFailures == MAX_CONSECUTIVE_FAILURES) {
            LOG_W("%s: no reply %u times in a row, retrying every %lu s",
                  sensor->name(), consecutiveFailures, BACKOFF_MS / 1000);
        }
        backoffUntil = now + BACKOFF_MS;
        if (backoffUntil == 0) backoffUntil = 1;  // 0 means "no backoff"
    }
    return false;
}

bool CO2Sensor::performZeroPointCalibration() {
    if (!sensor) {
        LOG_W("cannot calibrate: no CO2 sensor");
        return false;
    }
    LOG_W("%s zero-point calibration: sensor must have been in fresh air (~400 ppm) for 20 min",
          sensor->name());
    bool ok = sensor->zeroPointCalibration();
    if (ok) {
        LOG_I("%s zero-point calibration sent", sensor->name());
    } else {
        LOG_E("%s zero-point calibration failed", sensor->name());
    }
    return ok;
}
