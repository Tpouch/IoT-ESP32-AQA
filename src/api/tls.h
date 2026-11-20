#pragma once

#include <WiFiClientSecure.h>

#include "api/ca_roots.h"
#include "utils/secrets.h"

// Trust anchors for every HTTPS request the node makes: API_CA_PEM from .env
// (API_CA_FILE) when set, else the public roots Cloudflare issues from.
inline void configureTls(WiFiClientSecure& client) {
#ifdef API_CA_PEM
    client.setCACert(API_CA_PEM);
#else
    client.setCACert(API_CA_ROOTS);
#endif
    client.setHandshakeTimeout(10);  // seconds; see the budget in api_client.cpp
}
