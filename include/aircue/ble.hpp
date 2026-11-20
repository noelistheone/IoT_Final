#pragma once
#include <cstdint>
#include "cJSON.h"

namespace aircue {
struct Climate;
struct View;
bool bluetooth_start();
void bluetooth_update_live(const Climate& climate,const View& view,uint64_t now,uint64_t age);
void bluetooth_pairing_window();
// The passkey is displayed locally and must never enter telemetry or logs.
bool bluetooth_pairing_code(uint32_t& code);
cJSON* bluetooth_json();
}
