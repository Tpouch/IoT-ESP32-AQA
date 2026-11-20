#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <limits.h>

// One 15 s measurement window. Missing values (sensor absent, or no sample in
// the window) use the sentinels below and are sent as JSON null.
struct Frame {
    static const uint16_t NO_U16 = 0xFFFF;
    static const int16_t NO_I16 = INT16_MIN;

    uint32_t capturedAtMs = 0;  // millis() at capture, for age_s
    uint32_t epoch = 0;         // NTP time at capture, 0 if not synced yet
    uint16_t snd = NO_U16;
    uint16_t co2 = NO_U16;
    uint16_t pm25 = NO_U16;
    int16_t hum10 = NO_I16;     // % x10
    int16_t temp10 = NO_I16;    // °C x10
    bool stale = false;

    // API representation (docs/api.md, "Frame object"). nowMs is the send time:
    // age_s lets the server date frames captured before NTP answered.
    void toJson(JsonObject out, uint32_t nowMs) const {
        uint32_t ageS = (nowMs - capturedAtMs) / 1000;
        if (epoch) out["t"] = epoch; else out["t"] = nullptr;
        out["age_s"] = ageS;
        putU16(out, "snd", snd);
        putU16(out, "co2", co2);
        putU16(out, "pm25", pm25);
        putX10(out, "hum", hum10);
        putX10(out, "temp", temp10);
        out["stale"] = stale;
    }

private:
    static void putU16(JsonObject out, const char* key, uint16_t v) {
        if (v == NO_U16) out[key] = nullptr; else out[key] = v;
    }
    static void putX10(JsonObject out, const char* key, int16_t v) {
        if (v == NO_I16) out[key] = nullptr; else out[key] = v / 10.0f;
    }
};

// Fixed-size ring buffer: when full, the oldest frame is dropped (and counted)
// so a long outage keeps the most recent CAPACITY frames.
class FrameQueue {
public:
    static const size_t CAPACITY = 1440;  // 6 h at one frame every 15 s

    void push(const Frame& frame) {
        if (count == CAPACITY) {
            head = (head + 1) % CAPACITY;
            count--;
            dropped++;
        }
        buffer[(head + count) % CAPACITY] = frame;
        count++;
    }

    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    uint32_t droppedCount() const { return dropped; }

    // i = 0 is the oldest frame.
    const Frame& at(size_t i) const { return buffer[(head + i) % CAPACITY]; }

    void popFront(size_t n) {
        if (n > count) n = count;
        head = (head + n) % CAPACITY;
        count -= n;
    }

private:
    Frame buffer[CAPACITY];
    size_t head = 0;
    size_t count = 0;
    uint32_t dropped = 0;
};
