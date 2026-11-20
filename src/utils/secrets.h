#pragma once

// Defaults so the firmware builds as cloned. Real values come from the
// gitignored .env at the project root: tools/load_env.py turns it into the
// generated header env_secrets.h, whose defines win over the defaults below.
//
//     cp .env.example .env    # then edit .env
//
// Never commit a real password or a private key.

#if __has_include("env_secrets.h")
#include "env_secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "CHANGE_ME"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "CHANGE_ME"
#endif

// MQTT broker for the local Mosquitto instance. Port 1883 is plaintext.
#ifndef MQTT_BROKER
#define MQTT_BROKER "192.168.1.10"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME "student"
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD "student"
#endif

// Identity and location defaults (Medellín, for the course).
#define SENSOR_PREFIX "aqa-"
#ifndef GEOHASH_DEFAULT
#define GEOHASH_DEFAULT "d347"
#endif

// Metadata carried in status payloads; used by the OTA stage too.
#define HARDWARE_VERSION 14
#define FIRMWARE_VERSION "1.0.0"
#ifndef FIRMWARE_URL
#define FIRMWARE_URL "http://192.168.1.10:8000/firmware.bin"
#endif
#ifndef FIRMWARE_SIG_URL
#define FIRMWARE_SIG_URL "http://192.168.1.10:8000/firmware.sig"
#endif
