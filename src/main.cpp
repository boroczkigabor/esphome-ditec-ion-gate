#include <Arduino.h>
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "ble_gate.h"

// --- Globals ---
BleGate gate(GATE_BLE_ADDR, GATE_PIN);
AsyncMqttClient mqtt;
TimerHandle_t mqttReconnectTimer;

// Topic strings (built from config)
String topicGateJson;           // Single JSON topic for state+position
String topicAttributes;
String topicAvailability;       // ESP-level: online when MQTT connected (will = offline)
String topicGateAvailability;   // BLE-level: online only when BLE connected
String topicCommand;
String topicEnabled;
String topicEnabledState;
String topicBleStatus;
String topicRepair;             // Delete pairing command
String topicRepairAvail;        // Availability for repair button (offline while BLE connected)
String topicFailureLog;         // Failure log sensor
String topicWifiRssi;           // WiFi signal strength
String topicBleRssi;            // BLE signal strength

bool mqttConnected = false;
bool discoveryPublished = false;

// Track previous values to only publish changes
String prevGateJson;
String prevGateAvail;
String prevBleStatus;
String prevRepairAvail;
unsigned long lastWifiRssiPublish = 0;
bool attributesPublished = false;

// --- Forward declarations ---
void connectToMqtt();
void publishDiscovery();
void publishChanged();
void publishAttributes();
void publishEnabled();

// --- WiFi ---

void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
            connectToMqtt();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("[WiFi] Disconnected");
            mqttConnected = false;
            xTimerStop(mqttReconnectTimer, 0);
            // Don't call WiFi.begin() — ESP32 auto-reconnects by default.
            // Calling WiFi.begin() again causes ASSOC_LEAVE disconnect loops.
            break;
        default:
            break;
    }
}

// --- MQTT ---

void connectToMqtt() {
    Serial.println("[MQTT] Connecting...");
    mqtt.connect();
}

void onMqttConnect(bool sessionPresent) {
    Serial.println("[MQTT] Connected");
    mqttConnected = true;

    // Subscribe to command, enable, and repair topics
    mqtt.subscribe(topicCommand.c_str(), 1);
    mqtt.subscribe(topicEnabled.c_str(), 1);
    mqtt.subscribe(topicRepair.c_str(), 1);

    // Only publish discovery once — HA retains it across MQTT reconnects
    if (!discoveryPublished) {
        publishDiscovery();
        discoveryPublished = true;
    }

    // Mark available (will message handles offline on disconnect)
    mqtt.publish(topicAvailability.c_str(), 1, true, "online");

    // Force publish current state on connect
    prevGateJson = "";
    prevGateAvail = "";
    prevBleStatus = "";
    attributesPublished = false;
    publishChanged();
    publishEnabled();

    // Publish current failure log (may already have entries from before MQTT connected)
    String failPayload = gate.buildFailureLogJson();
    mqtt.publish(topicFailureLog.c_str(), 1, true, failPayload.c_str());
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    Serial.printf("[MQTT] Disconnected (reason=%d)\n", (int)reason);
    mqttConnected = false;
    if (WiFi.isConnected()) {
        xTimerStart(mqttReconnectTimer, 0);
    }
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total) {
    String msg;
    msg.reserve(len);
    for (size_t i = 0; i < len; i++) {
        msg += (char)payload[i];
    }

    Serial.printf("[MQTT] Received %s: %s\n", topic, msg.c_str());

    String topicStr(topic);

    if (topicStr == topicCommand) {
        if (msg == "OPEN")         gate.open();
        else if (msg == "CLOSE")   gate.close();
        else if (msg == "STOP")    gate.stop();
        else if (msg == "PARTIAL") gate.partial();
        else if (msg == "LIGHT")   gate.light();
    } else if (topicStr == topicEnabled) {
        if (msg == "ON")       gate.setEnabled(true);
        else if (msg == "OFF") gate.setEnabled(false);
    } else if (topicStr == topicRepair) {
        if (msg == "PRESS") gate.clearBondAndRepair();
    }
}

// --- MQTT Discovery ---

void publishDiscovery() {
    String deviceJson;
    {
        JsonDocument doc;
        JsonArray ids = doc["ids"].to<JsonArray>();
        ids.add(MQTT_DEVICE_ID);
        doc["name"] = MQTT_DEVICE_NAME;
        doc["mf"] = "DITEC";
        doc["mdl"] = "RCB100";
        serializeJson(doc, deviceJson);
    }

    // 1. Cover entity (the gate)
    // State string is authoritative for open/closed/opening/closing.
    // Position uses a template: 0% is remapped to 1% when state != closed (uncalibrated gate),
    // 100% is remapped to 99% to keep all buttons usable when fully open.
    {
        JsonDocument doc;
        doc["name"] = "Gate";
        doc["uniq_id"] = String(MQTT_DEVICE_ID) + "_gate";
        doc["cmd_t"] = topicCommand;
        doc["stat_t"] = topicGateJson;
        doc["val_tpl"] = "{{ value_json.state }}";
        doc["avty_t"] = topicGateAvailability;
        doc["json_attr_t"] = topicAttributes;
        doc["pl_open"] = "OPEN";
        doc["pl_cls"] = "CLOSE";
        doc["pl_stop"] = "STOP";
        doc["stat_open"] = "open";
        doc["stat_clsd"] = "closed";
        doc["stat_opening"] = "opening";
        doc["stat_closing"] = "closing";
        doc["stat_stopped"] = "stopped";
        doc["pos_t"] = topicGateJson;
        // 0% when state != closed → show 1% (gate is open but uncalibrated/not yet learned position)
        doc["pos_tpl"] = "{{ 1 if value_json.position == 0 and value_json.state != 'closed' else value_json.position }}";
        doc["pos_open"] = 100;
        doc["pos_clsd"] = 0;
        doc["dev_cla"] = "gate";
        doc["dev"] = serialized(deviceJson);

        String payload;
        serializeJson(doc, payload);
        String topic = "homeassistant/cover/" + String(MQTT_DEVICE_ID) + "/config";
        mqtt.publish(topic.c_str(), 1, true, payload.c_str());
    }

    // 2. Button entity (light toggle)
    {
        JsonDocument doc;
        doc["name"] = "Light";
        doc["uniq_id"] = String(MQTT_DEVICE_ID) + "_light";
        doc["cmd_t"] = topicCommand;
        doc["avty_t"] = topicGateAvailability;
        doc["pl_prs"] = "LIGHT";
        doc["ic"] = "mdi:lightbulb";
        doc["dev"] = serialized(deviceJson);

        String payload;
        serializeJson(doc, payload);
        String topic = "homeassistant/button/" + String(MQTT_DEVICE_ID) + "_light/config";
        mqtt.publish(topic.c_str(), 1, true, payload.c_str());
    }

    // 3. Button entity (partial open)
    {
        JsonDocument doc;
        doc["name"] = "Partial Open";
        doc["uniq_id"] = String(MQTT_DEVICE_ID) + "_partial";
        doc["cmd_t"] = topicCommand;
        doc["avty_t"] = topicGateAvailability;
        doc["pl_prs"] = "PARTIAL";
        doc["ic"] = "mdi:gate";
        doc["dev"] = serialized(deviceJson);

        String payload;
        serializeJson(doc, payload);
        String topic = "homeassistant/button/" + String(MQTT_DEVICE_ID) + "_partial/config";
        mqtt.publish(topic.c_str(), 1, true, payload.c_str());
    }

    // 4. Switch entity (BLE enable/disable toggle) — always available when ESP32 is online
    {
        JsonDocument doc;
        doc["name"] = "BLE Connection";
        doc["uniq_id"] = String(MQTT_DEVICE_ID) + "_enabled";
        doc["cmd_t"] = topicEnabled;
        doc["stat_t"] = topicEnabledState;
        doc["avty_t"] = topicAvailability;
        doc["ic"] = "mdi:bluetooth";
        doc["dev"] = serialized(deviceJson);

        String payload;
        serializeJson(doc, payload);
        String topic = "homeassistant/switch/" + String(MQTT_DEVICE_ID) + "_enabled/config";
        mqtt.publish(topic.c_str(), 1, true, payload.c_str());
    }

    // 5. Sensor entity (BLE connection status) — always available when ESP32 is online
    {
        JsonDocument doc;
        doc["name"] = "BLE Status";
        doc["uniq_id"] = String(MQTT_DEVICE_ID) + "_ble_status";
        doc["stat_t"] = topicBleStatus;
        doc["avty_t"] = topicAvailability;
        doc["ic"] = "mdi:bluetooth-connect";
        doc["dev"] = serialized(deviceJson);

        String payload;
        serializeJson(doc, payload);
        String topic = "homeassistant/sensor/" + String(MQTT_DEVICE_ID) + "_ble_status/config";
        mqtt.publish(topic.c_str(), 1, true, payload.c_str());
    }

    // 6. Button entity (delete pairing) — available only when BLE is not actively connected
    {
        JsonDocument doc;
        doc["name"] = "Delete pairing";
        doc["uniq_id"] = String(MQTT_DEVICE_ID) + "_repair";
        doc["cmd_t"] = topicRepair;
        JsonArray avty = doc["avty"].to<JsonArray>();
        avty.add<JsonObject>()["t"] = topicAvailability;   // ESP must be online
        avty.add<JsonObject>()["t"] = topicRepairAvail;    // BLE must not be connected
        doc["avty_mode"] = "all";
        doc["pl_prs"] = "PRESS";
        doc["ic"] = "mdi:bluetooth-off";
        doc["dev"] = serialized(deviceJson);

        String payload;
        serializeJson(doc, payload);
        String topic = "homeassistant/button/" + String(MQTT_DEVICE_ID) + "_repair/config";
        mqtt.publish(topic.c_str(), 1, true, payload.c_str());
    }

    // 7. Sensor entity (BLE failure log) — always available when ESP32 is online
    {
        JsonDocument doc;
        doc["name"] = "BLE Failures";
        doc["uniq_id"] = String(MQTT_DEVICE_ID) + "_failures";
        doc["stat_t"] = topicFailureLog;
        doc["val_tpl"] = "{{ value_json.last }}";
        doc["json_attr_t"] = topicFailureLog;
        doc["avty_t"] = topicAvailability;
        doc["ic"] = "mdi:alert-circle-outline";
        doc["dev"] = serialized(deviceJson);

        String payload;
        serializeJson(doc, payload);
        String topic = "homeassistant/sensor/" + String(MQTT_DEVICE_ID) + "_failures/config";
        mqtt.publish(topic.c_str(), 1, true, payload.c_str());
    }

    // 8. Sensor entity (WiFi RSSI)
    {
        JsonDocument doc;
        doc["name"] = "WiFi Signal";
        doc["uniq_id"] = String(MQTT_DEVICE_ID) + "_wifi_rssi";
        doc["stat_t"] = topicWifiRssi;
        doc["dev_cla"] = "signal_strength";
        doc["stat_cla"] = "measurement";
        doc["unit_of_meas"] = "dBm";
        doc["avty_t"] = topicAvailability;
        doc["ic"] = "mdi:wifi";
        doc["dev"] = serialized(deviceJson);

        String payload;
        serializeJson(doc, payload);
        String topic = "homeassistant/sensor/" + String(MQTT_DEVICE_ID) + "_wifi_rssi/config";
        mqtt.publish(topic.c_str(), 1, true, payload.c_str());
    }

    // 9. Sensor entity (BLE RSSI) — only meaningful when BLE is connected
    {
        JsonDocument doc;
        doc["name"] = "BLE Signal";
        doc["uniq_id"] = String(MQTT_DEVICE_ID) + "_ble_rssi";
        doc["stat_t"] = topicBleRssi;
        doc["dev_cla"] = "signal_strength";
        doc["stat_cla"] = "measurement";
        doc["unit_of_meas"] = "dBm";
        doc["avty_t"] = topicGateAvailability;
        doc["ic"] = "mdi:bluetooth-audio";
        doc["dev"] = serialized(deviceJson);

        String payload;
        serializeJson(doc, payload);
        String topic = "homeassistant/sensor/" + String(MQTT_DEVICE_ID) + "_ble_rssi/config";
        mqtt.publish(topic.c_str(), 1, true, payload.c_str());
    }

    Serial.println("[MQTT] Discovery published");
}

// --- State publishing (only publishes what changed) ---

void publishChanged() {
    if (!mqttConnected) return;

    const GateData& d = gate.data();

    // Gate availability — BLE dependent (only on change)
    String gateAvail = d.bleConnected ? "online" : "offline";
    if (gateAvail != prevGateAvail) {
        mqtt.publish(topicGateAvailability.c_str(), 1, true, gateAvail.c_str());
        prevGateAvail = gateAvail;
    }

    // Repair button availability — available only when BLE is NOT connected
    // (safe to delete pairing when disconnected or disabled, not while actively in use)
    String repairAvail = d.bleConnected ? "offline" : "online";
    if (repairAvail != prevRepairAvail) {
        mqtt.publish(topicRepairAvail.c_str(), 1, true, repairAvail.c_str());
        prevRepairAvail = repairAvail;
    }

    // BLE status sensor (only on change)
    if (d.bleStatus != prevBleStatus) {
        mqtt.publish(topicBleStatus.c_str(), 0, false, d.bleStatus.c_str());
        prevBleStatus = d.bleStatus;
    }

    // Gate state + position — single JSON message, QoS 0
    char json[64];
    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"position\":%d}", d.haState(), d.position);
    String jsonStr = json;
    if (jsonStr != prevGateJson) {
        mqtt.publish(topicGateJson.c_str(), 0, true, json);
        prevGateJson = jsonStr;
    }

    // Attributes — publish once on connect (mostly static data)
    if (!attributesPublished && d.bleConnected && d.manufacturer.length()) {
        publishAttributes();
        attributesPublished = true;
    }
}

void publishAttributes() {
    if (!mqttConnected) return;

    const GateData& d = gate.data();
    JsonDocument doc;
    doc["operation_counter"] = d.operationCounter;
    doc["ble_connected"] = d.bleConnected;
    doc["rssi"] = d.rssi;
    if (d.manufacturer.length()) doc["manufacturer"] = d.manufacturer;
    if (d.model.length())        doc["model"] = d.model;
    if (d.hwRev.length())        doc["hw_revision"] = d.hwRev;
    if (d.fwRev.length())        doc["fw_revision"] = d.fwRev;
    if (d.bleMac.length())       doc["ble_mac"] = d.bleMac;
    if (d.boardFw.length()) {
        doc["board_firmware"] = d.boardFw;
        doc["board_hardware"] = String(d.boardHw) + " Rev. " + String(d.boardRev);
    }
    doc["auto_close_enabled"] = d.autoCloseEnabled;
    doc["auto_close_timer"] = d.autoCloseTimer;
    doc["partial_open_pct"] = d.partialOpenPct;

    String payload;
    serializeJson(doc, payload);
    mqtt.publish(topicAttributes.c_str(), 1, true, payload.c_str());
}

void publishEnabled() {
    if (!mqttConnected) return;
    mqtt.publish(topicEnabledState.c_str(), 1, true, gate.isEnabled() ? "ON" : "OFF");
}

// --- Setup & Loop ---

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Ditec Gate ESP32 Bridge ===\n");

    // Build topic strings
    String base = MQTT_BASE_TOPIC;
    topicGateJson         = base + "/state";
    topicAttributes       = base + "/attributes";
    topicAvailability     = base + "/availability";
    topicGateAvailability = base + "/gate_availability";
    topicCommand          = base + "/command";
    topicEnabled          = base + "/enabled/set";
    topicEnabledState     = base + "/enabled/state";
    topicBleStatus        = base + "/ble_status";
    topicRepair           = base + "/repair/set";
    topicRepairAvail      = base + "/repair/availability";
    topicFailureLog       = base + "/failures";
    topicWifiRssi         = base + "/wifi_rssi";
    topicBleRssi          = base + "/ble_rssi";

    // MQTT reconnect timer (WiFi auto-reconnects on its own)
    mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(5000), pdFALSE,
                                      nullptr, [](TimerHandle_t) { connectToMqtt(); });

    // BLE init
    Serial.println("[BLE] Starting init...");
    gate.begin();
    Serial.println("[BLE] Init complete");

    // WiFi + MQTT
    WiFi.onEvent(WiFiEvent);
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCredentials(MQTT_USER, MQTT_PASS);
    mqtt.setKeepAlive(5);  // Broker detects disconnect within ~7.5s
    mqtt.setWill(topicAvailability.c_str(), 1, true, "offline");
    mqtt.onConnect(onMqttConnect);
    mqtt.onDisconnect(onMqttDisconnect);
    mqtt.onMessage(onMqttMessage);
    Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
    gate.loop();

    // Publish only what changed to MQTT
    if (gate.stateChanged()) {
        publishChanged();
        publishEnabled();
    }

    // Publish failure log whenever a new entry is added
    if (mqttConnected && gate.failureLogChanged()) {
        String payload = gate.buildFailureLogJson();
        mqtt.publish(topicFailureLog.c_str(), 1, true, payload.c_str());
    }

    // Publish WiFi and BLE RSSI every 5s
    if (mqttConnected && (millis() - lastWifiRssiPublish >= 5000)) {
        lastWifiRssiPublish = millis();
        mqtt.publish(topicWifiRssi.c_str(), 0, false, String(WiFi.RSSI()).c_str());
        if (gate.data().bleConnected) {
            mqtt.publish(topicBleRssi.c_str(), 0, false, String(gate.data().rssi).c_str());
        }
    }

    delay(10);
}
