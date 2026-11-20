#pragma once
#include <PMS.h>
#include <SoftwareSerial.h>
#include <memory>

// PMS7003 in active mode: the sensor streams a 32-byte frame every 0.2 to 2.3 s
// on its own. read() never waits for a frame unless asked to: it drains what
// SoftwareSerial already buffered and returns the newest complete frame, so a
// silent or unplugged sensor costs the scheduler nothing.
class PMSensor {
public:
    PMSensor() = default;

    bool init(int rxPin, int txPin);

    // timeoutMs = 0: non-blocking, true only if a new valid frame was parsed.
    // timeoutMs > 0: wait up to that long for a frame (boot-time detection).
    bool read(uint16_t& pm25, unsigned long timeoutMs = 0);

    // Milliseconds since the last valid frame, or since init() if none yet.
    unsigned long msSinceLastFrame() const;

    void wakeUp();
    void sleep();

private:
    std::unique_ptr<SoftwareSerial> serialPort;
    std::unique_ptr<PMS> pms;
    PMS::DATA data;
    unsigned long lastFrameMs = 0;

    // Room for a few frames between two 400 ms ticks.
    static const int RX_BUFFER_BYTES = 128;
    static const uint16_t MAX_PLAUSIBLE_UG = 1000;
};
