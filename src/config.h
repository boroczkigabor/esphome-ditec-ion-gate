#pragma once

// Credentials (WiFi, MQTT, gate address/PIN) live in credentials.h which is
// gitignored. Copy src/credentials.h.example → src/credentials.h and fill it in.
#include "credentials.h"

// --- MQTT Topics (generally no need to change) ---
#define MQTT_BASE_TOPIC     "ditec/gate"
#define MQTT_DEVICE_ID      "ditec_gate"
#define MQTT_DEVICE_NAME    "Ditec Gate"
