#pragma once

#include <ArduinoJson.h>

// Commands delivered in API responses: {"id": "...", "type": "...", "params": {...}}.
// Types: status, config, geohash, restart, ota, rollback, wifi (docs/api.md).
namespace Commands {

// Runs one command and acks it through ApiClient. Commands whose id matches
// the last executed one (persisted in NVS) are acked but not run again.
void execute(JsonObject command);

// Performs a reboot scheduled by restart/ota/rollback once its ack is out.
void loop();

}  // namespace Commands
