#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <MHZ19.h>
#include <s8_uart.h>
#include <memory>

// One CO2 driver per sensor family. Both talk 9600 8N1 over UART but speak
// different protocols, so detection asks each protocol in turn instead of
// guessing from a ppm value (a cold sensor legitimately reports 0 ppm).
class ICO2Sensor {
public:
    enum class ReadResult {
        OK,         // plausible ppm written
        NOT_READY,  // sensor answered, value not plausible yet (warm-up)
        NO_REPLY,   // nothing usable came back over the UART
    };

    virtual ~ICO2Sensor() = default;

    // True when the sensor answered its own protocol. Says nothing about warm-up.
    virtual bool probe() = 0;

    // One attempt, bounded by the library's UART timeout. ppm is only written
    // on OK.
    virtual ReadResult read(int& ppm) = 0;

    // Zero-point calibration: the sensor must sit in fresh air (~400 ppm) for
    // at least 20 minutes beforehand.
    virtual bool zeroPointCalibration() = 0;

    virtual const char* name() const = 0;
};

class MHZ19Sensor : public ICO2Sensor {
public:
    explicit MHZ19Sensor(Stream& stream) : stream(stream) {}

    bool probe() override;
    ReadResult read(int& ppm) override;
    bool zeroPointCalibration() override;
    const char* name() const override { return "MH-Z19"; }

private:
    Stream& stream;
    MHZ19 mhz19;
};

class SenseAirS8Sensor : public ICO2Sensor {
public:
    explicit SenseAirS8Sensor(Stream& stream) : s8(new S8_UART(stream)) {}

    bool probe() override;
    ReadResult read(int& ppm) override;
    bool zeroPointCalibration() override;
    const char* name() const override { return "SenseAir S8"; }

private:
    std::unique_ptr<S8_UART> s8;
};

// Front-end used by the HAL: owns the UART, detects the family, and keeps a
// broken or unplugged sensor from stalling the scheduler.
class CO2Sensor {
public:
    enum class SensorType { NONE, MHZ19, SENSEAIR_S8 };

    // Valid readings fall in this window; 0 ppm is the warm-up value of both
    // families and is rejected rather than averaged into a frame.
    static const int MIN_PPM = 1;
    static const int MAX_PPM = 10000;

    // espRx/espTx are the ESP32 GPIOs (espRx is wired to the sensor's TX).
    // SensorType::NONE means autodetect: MH-Z19 first, then SenseAir S8.
    bool init(HardwareSerial& serial, int espRx, int espTx,
              SensorType type = SensorType::NONE);

    // Single, non-retrying read. After MAX_CONSECUTIVE_FAILURES reads in a row
    // with no reply, reads are skipped for BACKOFF_MS so an unplugged sensor
    // (whose library may block for seconds per attempt) cannot starve other
    // tasks. Warm-up readings do not count. bypassBackoff forces an attempt.
    bool read(int& ppm, bool bypassBackoff = false);

    bool performZeroPointCalibration();

    bool isInitialized() const { return sensor != nullptr; }
    SensorType getType() const { return type; }
    const char* typeName() const { return sensor ? sensor->name() : "none"; }

private:
    static const uint8_t MAX_CONSECUTIVE_FAILURES = 3;
    static const unsigned long BACKOFF_MS = 30000;

    bool tryType(SensorType candidate, HardwareSerial& serial);
    static void drain(HardwareSerial& serial);

    std::unique_ptr<ICO2Sensor> sensor;
    SensorType type = SensorType::NONE;
    uint8_t consecutiveFailures = 0;
    unsigned long backoffUntil = 0;
};
