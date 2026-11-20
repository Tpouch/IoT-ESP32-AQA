#pragma once

// Pin definitions for TTGO T7 V1.4 Mini32, as wired on the AQA shield.
//
// Derive these pins from the PCB continuity map (S2) before trusting them.
// Two board generations circulate in the course; the firmware must work on both:
//   - DHT sensors sit on GPIO 13;
//   - the SHT4x I2C bus uses SDA 13 / SCL 22.
// DHT_PIN and I2C_SDA therefore share GPIO 13. Only one of the two temperature
// families is populated on a given board, and autodetection decides which one
// answers.
//
// UART pins are named from the ESP32's point of view: *_ESP_RX is the GPIO the
// ESP32 listens on, so it is wired to the sensor's TX line (and vice versa).
// GPIO 34..39 are input-only on the ESP32: the PMS TX line must land on 34 or
// 35, which makes GPIO 34 the ESP32's RX for the PM sensor.

struct BoardPins {
    static const int DHT_PIN = 13;
    static const int I2C_SDA = 13;
    static const int I2C_SCL = 22;
    static const int ADC_SOUND = 35;

    // PMS7003 on SoftwareSerial.
    static const int PM_ESP_RX = 34;   // <- PMS TX
    static const int PM_ESP_TX = 14;   // -> PMS RX

    // MH-Z19 / SenseAir S8 on Serial2 (the ESP32 core defaults, RX2/TX2).
    static const int CO2_ESP_RX = 16;  // <- sensor TX
    static const int CO2_ESP_TX = 17;  // -> sensor RX
};
