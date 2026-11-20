#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "api/frame_queue.h"

// HTTPS client for the AQA API (docs/api.md).
//
//   POST {API_URL}/api/v1/devices/{sensor_id}/status    every 60 s and at boot
//   POST {API_URL}/api/v1/devices/{sensor_id}/frames    whenever frames are queued
//   POST {API_URL}/api/v1/devices/{sensor_id}/commands/{id}/ack
//
// Both status and frames responses may carry pending commands, which are
// handed to Commands::execute(). One request per loop() call, so the sampler
// keeps running between requests; failures back off exponentially.
class ApiClient {
public:
    static ApiClient& getInstance() {
        static ApiClient instance;
        return instance;
    }

    void begin(FrameQueue& queue);
    void loop();

    // Send a status report on the next loop() (e.g. after a "status" command).
    void requestStatus() { statusDue = true; }

    // Report a command's progress or outcome. status: in_progress|completed|failed.
    bool ack(const char* commandId, const char* status, int progress, const char* detail);

    // Close the kept-alive TLS session (frees ~40 KB before an OTA download).
    void releaseConnection();

    // Short human-readable state for the serial status line.
    const char* state() const { return stateText; }

private:
    enum class Result { OK, REJECTED, RETRY };

    ApiClient() = default;
    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    Result post(const String& path, const JsonDocument& body, JsonDocument* response);
    bool sendStatus();
    bool sendFrames();
    void runCommands(JsonDocument& response);
    void backoff();
    void diagnoseTransport();  // logs whether DNS or TCP is what fails
    void probePlainHttp(const String& host, const IPAddress& addr);

    String devicePath(const char* suffix) const;

    FrameQueue* queue = nullptr;
    bool secure = true;   // false only for an http:// API_URL (local tests)
    WiFiClient plain;
    WiFiClientSecure tls;
    HTTPClient http;

    bool statusDue = true;
    unsigned long lastStatusAt = 0;
    unsigned long nextAttemptAt = 0;
    unsigned long retryDelayMs = 0;
    const char* stateText = "idle";

    static const unsigned long STATUS_INTERVAL_MS = 60000;
    static const unsigned long RETRY_MIN_MS = 5000;
    static const unsigned long RETRY_MAX_MS = 300000;
    static const size_t MAX_BATCH = 20;
};
