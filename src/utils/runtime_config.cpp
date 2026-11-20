#include "runtime_config.h"
#include "secrets.h"
#include "network/network_manager.h"
#include <Preferences.h>

const char* RuntimeConfig::NVS_DEVICE = "device";
const char* RuntimeConfig::NVS_PARAMS = "params";
const char* RuntimeConfig::KEY_GEOHASH = "geohash";
const char* RuntimeConfig::KEY_BOOT_COUNT = "boot_count";
const char* RuntimeConfig::KEY_PARAMS_JSON = "p";
const char* RuntimeConfig::KEY_LAST_COMMAND = "last_cmd";
const char* RuntimeConfig::NVS_WIFI = "wifi";

void RuntimeConfig::begin() {
    incrementBootCount();
}

String RuntimeConfig::getGeohash() const {
    Preferences prefs;
    if (!prefs.begin(NVS_DEVICE, true)) return String(GEOHASH_DEFAULT);
    String g;
    if (prefs.isKey(KEY_GEOHASH)) g = prefs.getString(KEY_GEOHASH, "");
    prefs.end();
    if (g.length() > 0) return g;
    return String(GEOHASH_DEFAULT);
}

void RuntimeConfig::setGeohash(const char* geohash) {
    if (!geohash) return;
    Preferences prefs;
    if (!prefs.begin(NVS_DEVICE, false)) return;
    prefs.putString(KEY_GEOHASH, geohash);
    prefs.end();
}

String RuntimeConfig::getLastCommandId() const {
    Preferences prefs;
    if (!prefs.begin(NVS_DEVICE, true)) return String();
    String id = prefs.isKey(KEY_LAST_COMMAND) ? prefs.getString(KEY_LAST_COMMAND, "") : String();
    prefs.end();
    return id;
}

void RuntimeConfig::setLastCommandId(const char* id) {
    if (!id) return;
    Preferences prefs;
    if (!prefs.begin(NVS_DEVICE, false)) return;
    prefs.putString(KEY_LAST_COMMAND, id);
    prefs.end();
}

bool RuntimeConfig::getWifiOverride(WifiCreds& out) const {
    Preferences prefs;
    if (!prefs.begin(NVS_WIFI, true)) return false;
    bool present = prefs.isKey("ssid");
    if (present) {
        out.ssid = prefs.getString("ssid", "");
        out.password = prefs.getString("pass", "");
        out.eapUser = prefs.getString("eap_user", "");
        out.eapPassword = prefs.getString("eap_pass", "");
        out.eapIdentity = prefs.getString("eap_id", "");
    }
    prefs.end();
    return present && out.ssid.length() > 0;
}

void RuntimeConfig::setWifiOverride(const WifiCreds& c) {
    Preferences prefs;
    if (!prefs.begin(NVS_WIFI, false)) return;
    prefs.clear();
    prefs.putString("ssid", c.ssid);
    prefs.putString("pass", c.password);
    prefs.putString("eap_user", c.eapUser);
    prefs.putString("eap_pass", c.eapPassword);
    prefs.putString("eap_id", c.eapIdentity);
    prefs.end();
}

void RuntimeConfig::clearWifiOverride() {
    Preferences prefs;
    if (!prefs.begin(NVS_WIFI, false)) return;
    prefs.clear();
    prefs.end();
}

bool RuntimeConfig::getParameter(const char* key, float* out) const {
    if (!out) return false;
    Preferences prefs;
    if (!prefs.begin(NVS_PARAMS, false)) return false;
    if (!prefs.isKey(KEY_PARAMS_JSON)) { prefs.end(); return false; }
    String json = prefs.getString(KEY_PARAMS_JSON, "{}");
    prefs.end();
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
    JsonObject o = doc.as<JsonObject>();
    if (!o[key].is<float>()) return false;
    *out = o[key].as<float>();
    return true;
}

void RuntimeConfig::setParameter(const char* key, float value) {
    Preferences prefs;
    if (!prefs.begin(NVS_PARAMS, false)) return;
    String json = prefs.isKey(KEY_PARAMS_JSON) ? prefs.getString(KEY_PARAMS_JSON, "{}") : String("{}");
    JsonDocument doc;
    deserializeJson(doc, json);
    doc[key] = value;
    String out;
    serializeJson(doc.as<JsonObject>(), out);
    prefs.putString(KEY_PARAMS_JSON, out);
    prefs.end();
}

void RuntimeConfig::setParametersFromJson(JsonObject params) {
    Preferences prefs;
    if (!prefs.begin(NVS_PARAMS, false)) return;
    String json = prefs.isKey(KEY_PARAMS_JSON) ? prefs.getString(KEY_PARAMS_JSON, "{}") : String("{}");
    JsonDocument doc;
    deserializeJson(doc, json);
    JsonObject o = doc.as<JsonObject>();
    for (JsonPair kv : params) {
        if (kv.value().is<float>()) o[kv.key()] = kv.value().as<float>();
        else if (kv.value().is<double>()) o[kv.key()] = (float)kv.value().as<double>();
        else if (kv.value().is<int>()) o[kv.key()] = (float)kv.value().as<int>();
    }
    String out;
    serializeJson(o, out);
    prefs.putString(KEY_PARAMS_JSON, out);
    prefs.end();
}

void RuntimeConfig::getParametersJson(JsonObject out) const {
    Preferences prefs;
    if (!prefs.begin(NVS_PARAMS, false)) return;
    if (!prefs.isKey(KEY_PARAMS_JSON)) { prefs.end(); return; }
    String json = prefs.getString(KEY_PARAMS_JSON, "{}");
    prefs.end();
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;
    for (JsonPair kv : doc.as<JsonObject>())
        out[kv.key()] = kv.value().as<float>();
}

int RuntimeConfig::getBootCount() const {
    Preferences prefs;
    if (!prefs.begin(NVS_DEVICE, true)) return 0;
    int c = prefs.getInt(KEY_BOOT_COUNT, 0);
    prefs.end();
    return c;
}

void RuntimeConfig::incrementBootCount() {
    Preferences prefs;
    if (!prefs.begin(NVS_DEVICE, false)) return;
    int c = prefs.getInt(KEY_BOOT_COUNT, 0);
    prefs.putInt(KEY_BOOT_COUNT, c + 1);
    prefs.end();
}
