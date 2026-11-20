#include "ota_manager.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <memory>

#include "api/tls.h"
#include "utils/logger.h"

// WiFiClient::setTimeout() takes SECONDS on the ESP32 core. Keep it below the
// 30 s loop watchdog, which is also fed between chunks.
static const uint32_t READ_TIMEOUT_S = 20;
static const size_t MAX_SIGNATURE_LEN = 1024;
static const size_t CHUNK = 4096;

namespace {

// Owns whichever transport the URL needs; must outlive the HTTPClient.
struct Transport {
    WiFiClient plain;
    WiFiClientSecure secure;

    bool begin(HTTPClient& http, const char* url) {
        if (strncmp(url, "https://", 8) == 0) {
            configureTls(secure);
            secure.setTimeout(READ_TIMEOUT_S);
            return http.begin(secure, url);
        }
        plain.setTimeout(READ_TIMEOUT_S);
        return http.begin(plain, url);
    }
};

struct Crypto {
    mbedtls_pk_context pk;
    mbedtls_md_context_t md;
    Crypto() { mbedtls_pk_init(&pk); mbedtls_md_init(&md); }
    ~Crypto() { mbedtls_md_free(&md); mbedtls_pk_free(&pk); }
};

}  // namespace

void OTAManager::init(const uint8_t* publicKeyDer, size_t publicKeyLen) {
    publicKey = publicKeyDer;
    keyLen = publicKeyLen;
}

bool OTAManager::install(const char* firmwareUrl, const char* signatureUrl,
                         String& error, ProgressFn progress, void* ctx) {
    if (!publicKey || !firmwareUrl || !*firmwareUrl || !signatureUrl || !*signatureUrl) {
        error = "missing url or public key";
        return false;
    }

    Crypto crypto;
    if (mbedtls_pk_parse_public_key(&crypto.pk, publicKey, keyLen) != 0 ||
        mbedtls_md_setup(&crypto.md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0 ||
        mbedtls_md_starts(&crypto.md) != 0) {
        error = "crypto setup failed";
        return false;
    }

    // 1. Signature (small, read fully).
    uint8_t signature[MAX_SIGNATURE_LEN];
    size_t signatureLen = 0;
    {
        Transport transport;
        HTTPClient http;
        if (!transport.begin(http, signatureUrl)) {
            error = "bad signature url";
            return false;
        }
        int code = http.GET();
        int size = http.getSize();
        if (code != HTTP_CODE_OK || size <= 0 || size > (int)MAX_SIGNATURE_LEN) {
            error = String("signature download: http ") + code + ", size " + size;
            http.end();
            return false;
        }
        signatureLen = http.getStreamPtr()->readBytes(signature, size);
        http.end();
        if (signatureLen != (size_t)size) {
            error = "signature truncated";
            return false;
        }
    }

    // 2. Firmware: hash and write in one pass.
    Transport transport;
    HTTPClient http;
    if (!transport.begin(http, firmwareUrl)) {
        error = "bad firmware url";
        return false;
    }
    int code = http.GET();
    int total = http.getSize();
    if (code != HTTP_CODE_OK || total <= 0) {
        error = String("firmware download: http ") + code + ", size " + total;
        http.end();
        return false;
    }
    if (!Update.begin(total, U_FLASH)) {
        error = String("no room for image: ") + Update.errorString();
        http.end();
        return false;
    }

    LOG_I("OTA: downloading %d bytes", total);
    std::unique_ptr<uint8_t[]> buf(new uint8_t[CHUNK]);
    WiFiClient* stream = http.getStreamPtr();
    int remaining = total;
    int lastReported = -1;
    while (remaining > 0) {
        esp_task_wdt_reset();
        size_t want = remaining < (int)CHUNK ? remaining : CHUNK;
        size_t got = stream->readBytes(buf.get(), want);
        if (got == 0) {
            error = "download stalled";
            Update.abort();
            http.end();
            return false;
        }
        mbedtls_md_update(&crypto.md, buf.get(), got);
        if (Update.write(buf.get(), got) != got) {
            error = String("flash write: ") + Update.errorString();
            Update.abort();
            http.end();
            return false;
        }
        remaining -= got;
        int percent = (int)(100LL * (total - remaining) / total);
        if (progress && percent / 25 != lastReported / 25) {
            lastReported = percent;
            progress(percent, ctx);
        }
    }
    http.end();

    // 3. Verify before the slot becomes bootable.
    uint8_t hash[32];
    mbedtls_md_finish(&crypto.md, hash);
    if (mbedtls_pk_verify(&crypto.pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                          signature, signatureLen) != 0) {
        error = "signature verification failed";
        Update.abort();
        return false;
    }
    if (!Update.end()) {
        error = String("finalise: ") + Update.errorString();
        return false;
    }
    LOG_I("OTA: signature verified, new image will boot after restart");
    return true;
}

bool OTAManager::rollback(String& error) {
    if (!Update.canRollBack()) {
        error = "no valid previous image";
        return false;
    }
    if (!Update.rollBack()) {
        error = "could not switch boot partition";
        return false;
    }
    return true;
}
