#pragma once

// Defaults so the firmware builds as cloned. Real values come from the
// gitignored .env at the project root: tools/load_env.py turns it into the
// generated header env_secrets.h, whose defines win over the defaults below.
//
//     cp .env.example .env    # then edit .env
//
// Never commit a real password, token or private key.

#if __has_include("env_secrets.h")
#include "env_secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "CHANGE_ME"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "CHANGE_ME"
#endif

// WPA2-Enterprise (PEAP / MSCHAPv2), e.g. eduroam or a residence 802.1X
// network. Setting WIFI_EAP_USERNAME switches the join to enterprise mode and
// WIFI_PASSWORD is ignored. WIFI_EAP_IDENTITY is the "anonymous identity"
// (outer identity); empty means the username is used.
#ifndef WIFI_EAP_IDENTITY
#define WIFI_EAP_IDENTITY ""
#endif
#ifndef WIFI_EAP_USERNAME
#define WIFI_EAP_USERNAME ""
#endif
#ifndef WIFI_EAP_PASSWORD
#define WIFI_EAP_PASSWORD ""
#endif
// WIFI_EAP_CA_PEM (from WIFI_EAP_CA_FILE in .env) is optional; without it the
// RADIUS server's certificate is not checked.

// HTTPS API the node reports to (see docs/api.md). No trailing slash.
#ifndef API_URL
#define API_URL "https://sensor.devgoblin.fr"
#endif
// Per-device secret sent as "Authorization: Bearer <token>". Empty = the
// server will answer 401 and nothing is stored.
#ifndef DEVICE_TOKEN
#define DEVICE_TOKEN ""
#endif
// API_CA_PEM (from API_CA_FILE in .env) overrides the built-in root bundle in
// src/api/ca_roots.h, e.g. for a self-hosted server with a private CA.

// Identity and location defaults (Medellín, for the course).
#define SENSOR_PREFIX "aqa-"
#ifndef GEOHASH_DEFAULT
#define GEOHASH_DEFAULT "d347"
#endif

// Metadata carried in status payloads.
#define HARDWARE_VERSION 14
#define FIRMWARE_VERSION "2.0.0"
