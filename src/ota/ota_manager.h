#pragma once

#include <Arduino.h>

// Signed OTA. The image is downloaded once: each chunk is hashed (SHA-256)
// and written to the inactive OTA slot. Only when the RSA signature over that
// hash verifies against the embedded public key is the slot made bootable;
// otherwise the write is aborted and the running firmware is untouched.
class OTAManager {
public:
    typedef void (*ProgressFn)(int percent, void* ctx);

    static OTAManager& getInstance() {
        static OTAManager instance;
        return instance;
    }

    void init(const uint8_t* publicKeyDer, size_t publicKeyLen);

    // https:// URLs are verified against the API trust roots (src/api/tls.h).
    // On success the caller reboots into the new image.
    bool install(const char* firmwareUrl, const char* signatureUrl,
                 String& error, ProgressFn progress = nullptr, void* ctx = nullptr);

    // Boot the other OTA slot if it holds a valid image. Caller reboots.
    bool rollback(String& error);

private:
    OTAManager() = default;
    OTAManager(const OTAManager&) = delete;
    OTAManager& operator=(const OTAManager&) = delete;

    const uint8_t* publicKey = nullptr;
    size_t keyLen = 0;
};
