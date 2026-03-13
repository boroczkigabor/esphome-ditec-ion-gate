#include "ble_gate.h"
#include <Arduino.h>

// --- GateData helpers ---

const char* GateData::statusName() const {
    switch (status) {
        case STATUS_TRANSITIONING: return "Transitioning";
        case STATUS_CLOSED:        return "Closed";
        case STATUS_OPEN:          return "Open";
        case STATUS_STOPPED:       return "Stopped";
        case STATUS_CLOSING:       return "Closing";
        case STATUS_OPENING:       return "Opening";
        case STATUS_PARTIAL_OPEN:  return "Partial Open";
        case STATUS_IR_REVERSING:  return "IR Reversing";
        case STATUS_ERROR:         return "Error";
        default:                   return "Unknown";
    }
}

const char* GateData::haState() const {
    switch (status) {
        case STATUS_CLOSED:        return "closed";
        case STATUS_OPEN:          return "open";
        case STATUS_PARTIAL_OPEN:  return "open";
        case STATUS_OPENING:       return "opening";
        case STATUS_CLOSING:       return "closing";
        case STATUS_STOPPED:       return "stopped";
        case STATUS_IR_REVERSING:  return "opening";
        case STATUS_TRANSITIONING: return "opening";
        case STATUS_ERROR:         return "stopped";
        default:                   return "stopped";
    }
}

// --- Server Callbacks (gate connects to our GATT server) ---

void GateServerCallback::onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) {
    Serial.printf("[BLE-SRV] Gate connected to our server (addr=%s)\n",
                  connInfo.getAddress().toString().c_str());
}

void GateServerCallback::onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) {
    Serial.printf("[BLE-SRV] Gate disconnected from server (reason=%d)\n", reason);
}

void GateServerCallback::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    Serial.printf("[BLE-SRV] Server auth complete: encrypted=%d, bonded=%d\n",
                  connInfo.isEncrypted(), connInfo.isBonded());
}

// --- Client Callbacks (our outgoing connection to gate) ---

void GateClientCallback::onConnect(NimBLEClient* client) {
    Serial.println("[BLE-CLI] Connected to gate");
}

void GateClientCallback::onDisconnect(NimBLEClient* client, int reason) {
    Serial.printf("[BLE-CLI] Disconnected from gate (reason=0x%02X)\n", reason);
    if (onDisconnectCb) onDisconnectCb(reason);
}

void GateClientCallback::onPassKeyEntry(NimBLEConnInfo& connInfo) {
    Serial.printf("[BLE-SEC] *** PASSKEY ENTRY REQUESTED → responding with PIN %lu ***\n", pin);
    NimBLEDevice::injectPassKey(connInfo, pin);
}

void GateClientCallback::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    authComplete = true;
    authSuccess = connInfo.isEncrypted();
    Serial.printf("[BLE-SEC] Client auth complete: encrypted=%d, bonded=%d\n",
                  connInfo.isEncrypted(), connInfo.isBonded());
    if (authSuccess) {
        Serial.println("[BLE-SEC] *** PAIRING SUCCESSFUL ***");
    } else {
        Serial.println("[BLE-SEC] Pairing FAILED");
    }
}

// --- GATT Server Response Callback ---

void GateResponseCallback::onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) {
    NimBLEAttValue val = characteristic->getValue();
    if (onData && val.size() > 0) {
        onData(val.data(), val.size());
    }
}

// --- BleGate ---

BleGate::BleGate(const char* address, uint32_t pin)
    : _addressStr(address), _address(std::string(address), BLE_ADDR_PUBLIC), _pin(pin) {}

void BleGate::begin() {
    Serial.println("[BLE] Initializing NimBLE...");
    NimBLEDevice::init("HADitec");

    // Log existing bonds — do NOT delete them; bonded reconnections skip pairing
    int bondCount = NimBLEDevice::getNumBonds();
    if (bondCount > 0) {
        Serial.printf("[BLE] %d existing bond(s) found (will reuse)\n", bondCount);
    }

    // Security: LE Secure Connections + MITM + Bonding
    // Match iPhone's SMP params: IO_CAP=KeyboardDisplay, AuthReq=0x0D (Bond+MITM+SC)
    NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM, SC
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);  // 0x04 = KeyboardDisplay
    NimBLEDevice::setSecurityPasskey(_pin);
    // Match iOS key distribution: 0x0B = ENC + ID + LINK (not SIGN)
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | 0x08 /* LINK key, matches iOS 0x0B */);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID | 0x08 /* LINK key, matches iOS 0x0B */);

    Serial.printf("[BLE] Security: PIN=%lu IO=KeyboardDisplay Auth=Bond+MITM+SC (NimBLE)\n", _pin);

    // Set PIN for client callback
    _clientCb.pin = _pin;

    // Setup GATT server for bidirectional communication
    _startGattServer();

    // Setup client disconnect callback
    _clientCb.onDisconnectCb = [this](int reason) {
        _data.bleConnected = false;
        _remoteChar = nullptr;
        _stateChanged = true;

        // NimBLE wraps HCI error codes with 0x200 base offset
        uint16_t hci = (reason >= 0x200) ? (reason - 0x200) : reason;
        const char* desc;
        switch (hci) {
            case 0x08: desc = "Connection Timeout (supervision timeout - signal loss)"; break;
            case 0x13: desc = "Remote User Terminated";                break;
            case 0x16: desc = "Local Host Terminated (GATT timeout or stack reset)"; break;
            case 0x22: desc = "LL Response Timeout";                   break;
            case 0x3E: desc = "Connection Failed to Establish";        break;
            case 0x05: desc = "Auth Failure (LTK mismatch or security error)"; break;
            case 0x1A: desc = "Unsupported Remote Feature";            break;
            case 0x20: desc = "Unspecified Error";                     break;
            default:   desc = "Unknown";                               break;
        }

        char msg[80];
        snprintf(msg, sizeof(msg), "Disconnected: %s (raw=0x%X hci=0x%02X)", desc, reason, hci);
        _data.bleStatus = String("Disconnected: ") + desc;
        _logFailure(msg);
    };

    // Setup response callback
    _responseCb.onData = [this](const uint8_t* data, size_t len) {
        _parseResponse(data, len);
    };

    Serial.println("[BLE] Initialized (NimBLE stack)");
}

void BleGate::_startGattServer() {
    _server = NimBLEDevice::createServer();
    _server->setCallbacks(&_serverCb);

    NimBLEService* service = _server->createService(DITEC_SERVICE_UUID);
    NimBLECharacteristic* chr = service->createCharacteristic(
        DITEC_CHAR_UUID,
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::WRITE_NR
    );
    chr->setCallbacks(&_responseCb);
    service->start();

    // Start advertising to commit GATT table
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(DITEC_SERVICE_UUID);
    adv->start();

    Serial.println("[BLE] GATT server started");
}

void BleGate::loop() {
    if (!_enabled) {
        if (_data.bleConnected) {
            _disconnect();
        }
        return;
    }

    if (!_data.bleConnected) {
        // iOS log shows 5+ connection attempts during initial pairing — be patient
        // Only stop after 10 consecutive failures
        if (_failCount >= 10) {
            if (_failCount == 10) {
                Serial.println("[BLE] Too many failures — stopped. Restart ESP32 to retry.");
                _failCount = 11;  // only print once
            }
            return;
        }
        unsigned long now = millis();
        if (now - _lastReconnect >= 2000) {
            _lastReconnect = now;
            if (!_connect()) {
                _failCount++;
                Serial.printf("[BLE] Reconnect failed (%d/10), retrying in 2s\n", _failCount);
            } else {
                _failCount = 0;
            }
        }
        return;
    }

    // Update fast polling state based on gate status
    if (_isActiveStatus(_data.status)) {
        _fastPolling = true;
        _fastPollUntil = millis() + 5000;  // stay fast for 5s after last active status
    } else if (_fastPolling && millis() > _fastPollUntil) {
        _fastPolling = false;
    }

    // Poll the gate
    unsigned long now = millis();
    if (now - _lastPoll >= _pollInterval()) {
        _lastPoll = now;
        _poll();
    }
}

bool BleGate::_connect() {
    Serial.printf("[BLE] Connecting to %s...\n", _addressStr.c_str());
    _data.bleStatus = "Connecting";
    _stateChanged = true;

    if (_client == nullptr) {
        _client = NimBLEDevice::createClient();
        _client->setClientCallbacks(&_clientCb);
    }

    // Connect with security enabled
    if (!_client->connect(_address)) {
        Serial.println("[BLE] Connection failed");
        _data.bleStatus = "Connection Failed";
        _stateChanged = true;
        _logFailure("Connection failed (could not reach gate)");
        return false;
    }

    Serial.printf("[BLE] Connected! MTU=%d\n", _client->getMTU());

    // Give the gate time to discover our GATT server and send Security Request
    // (iOS log shows ~1s between connection and SMP Security Request)
    delay(500);

    // Initiate secure connection (triggers SMP pairing if not bonded,
    // or restores encryption from stored LTK if bonded)
    _data.bleStatus = "Securing";
    _stateChanged = true;
    _clientCb.authComplete = false;
    _clientCb.authSuccess = false;

    Serial.println("[BLE] Requesting secure connection...");
    if (!_client->secureConnection()) {
        Serial.println("[BLE] secureConnection() returned false — waiting for async result...");
    }

    // Wait for auth to complete (gate has 30s SMP timeout)
    // iOS log shows pairing takes ~2s once passkey rounds start
    unsigned long authStart = millis();
    while (!_clientCb.authComplete && (millis() - authStart) < 25000) {
        if (!_client->isConnected()) {
            Serial.println("[BLE] Disconnected during pairing");
            _data.bleStatus = "Pairing Failed - Disconnected";
            _stateChanged = true;
            _logFailure("Disconnected during pairing (bond preserved - use Re-pair button if needed)");
            return false;
        }
        delay(100);
    }

    if (_clientCb.authComplete && _clientCb.authSuccess) {
        Serial.println("[BLE] *** Paired and encrypted! ***");
    } else if (_clientCb.authComplete) {
        Serial.println("[BLE] Auth failed (bond preserved - use Re-pair button if needed)");
        _data.bleStatus = "Auth Failed - Use Re-pair";
        _stateChanged = true;
        _logFailure("Auth failed - bond preserved. If this persists, use Re-pair button.");
        _client->disconnect();
        return false;
    } else {
        Serial.println("[BLE] Pairing timeout — proceeding anyway (temp access?)");
    }

    _data.bleStatus = "Discovering Services";
    _stateChanged = true;

    NimBLERemoteService* svc = _client->getService(DITEC_SERVICE_UUID);
    if (!svc) {
        Serial.println("[BLE] Service not found on gate");
        _data.bleStatus = "Service Not Found";
        _stateChanged = true;
        _logFailure("Service not found on gate (unexpected firmware change?)");
        _client->disconnect();
        return false;
    }

    _remoteChar = svc->getCharacteristic(DITEC_CHAR_UUID);
    if (!_remoteChar) {
        Serial.println("[BLE] Characteristic not found on gate");
        _data.bleStatus = "Characteristic Not Found";
        _stateChanged = true;
        _logFailure("Characteristic not found on gate");
        _client->disconnect();
        return false;
    }

    Serial.println("[BLE] Found gate service + characteristic");

    _data.bleConnected = true;
    _data.bleStatus = "Reading Device Info";
    _stateChanged = true;

    // Read device info once
    if (!_deviceInfoRead) {
        _readDeviceInfo();
        _deviceInfoRead = true;
    }

    _data.rssi = _client->getRssi();
    _data.bleStatus = "Connected";
    _stateChanged = true;
    return true;
}

void BleGate::_readDeviceInfo() {
    if (!_client || !_client->isConnected()) return;

    NimBLERemoteService* dis = _client->getService(NimBLEUUID((uint16_t)0x180A));
    if (dis) {
        auto readStr = [&](const NimBLEUUID& uuid) -> String {
            NimBLERemoteCharacteristic* c = dis->getCharacteristic(uuid);
            if (c && c->canRead()) {
                std::string val = c->readValue();
                return String(val.c_str());
            }
            return "";
        };
        _data.manufacturer = readStr(CHAR_MANUFACTURER_UUID);
        _data.model        = readStr(CHAR_MODEL_UUID);
        _data.hwRev        = readStr(CHAR_HW_REV_UUID);
        _data.fwRev        = readStr(CHAR_FW_REV_UUID);
        _data.serial       = readStr(CHAR_SERIAL_UUID);

        Serial.printf("[BLE] Device: %s %s (HW:%s FW:%s)\n",
                      _data.manufacturer.c_str(), _data.model.c_str(),
                      _data.hwRev.c_str(), _data.fwRev.c_str());
    }

    // One-time system + config polls
    _sendCommand(CMD_POLL_SYSTEM, sizeof(CMD_POLL_SYSTEM));
    delay(600);
    _sendCommand(CMD_POLL_CONFIG, sizeof(CMD_POLL_CONFIG));
    delay(600);
}

void BleGate::_disconnect() {
    if (_client && _client->isConnected()) {
        _client->disconnect();
    }
    _data.bleConnected = false;
    _data.bleStatus = "Disconnected";
    _remoteChar = nullptr;
    _stateChanged = true;
    Serial.println("[BLE] Disconnected");
}

void BleGate::_sendCommand(const uint8_t* cmd, size_t len) {
    if (!_remoteChar || !_client || !_client->isConnected()) return;
    _remoteChar->writeValue(cmd, len, false);
}

void BleGate::_poll() {
    static const uint8_t* polls[] = {CMD_POLL_STATUS, CMD_POLL_POSITION};
    static const size_t   sizes[] = {sizeof(CMD_POLL_STATUS), sizeof(CMD_POLL_POSITION)};

    _sendCommand(polls[_pollIndex], sizes[_pollIndex]);
    _pollIndex = (_pollIndex + 1) % 2;

    // Update RSSI periodically
    if (_pollIndex == 0 && _client && _client->isConnected()) {
        _data.rssi = _client->getRssi();
    }
}

void BleGate::_parseResponse(const uint8_t* data, size_t len) {
    if (len < 2) return;

    uint8_t frameType = data[0];

    if (frameType == 0x34) {
        uint8_t oldStatus = _data.status;
        _data.status = data[1];
        if (_data.status != oldStatus) {
            _stateChanged = true;
            Serial.printf("[BLE] Status: %s (0x%02X)\n", _data.statusName(), _data.status);
        }
    } else if (frameType == 0x3F && len >= 10) {
        uint8_t oldPos = _data.position;
        _data.position = data[1];
        _data.operationCounter = data[9];
        if (_data.position != oldPos) {
            _stateChanged = true;
        }
    } else if (frameType == 0x36 && len >= 42) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 data[41], data[40], data[39], data[38], data[37], data[36]);
        _data.bleMac = String(mac);
        _data.boardHw = data[4];
        _data.boardRev = data[5];
        char fw[12];
        snprintf(fw, sizeof(fw), "%d.%d.%d", data[6], data[7], data[8]);
        _data.boardFw = String(fw);
        _stateChanged = true;
        Serial.printf("[BLE] Board: HW %d Rev.%d FW %s MAC %s\n",
                      _data.boardHw, _data.boardRev, _data.boardFw.c_str(), _data.bleMac.c_str());
    } else if (frameType == 0x32 && len > 50) {
        _data.autoCloseEnabled = data[7] != 0;
        _data.autoCloseTimer = data[8];
        _data.partialOpenPct = data[25];
        _stateChanged = true;
    }
}

// --- Adaptive polling helpers ---

bool BleGate::_isActiveStatus(uint8_t status) const {
    return status == STATUS_OPENING ||
           status == STATUS_CLOSING ||
           status == STATUS_IR_REVERSING ||
           status == STATUS_TRANSITIONING;
}

unsigned long BleGate::_pollInterval() const {
    return _fastPolling ? POLL_FAST_MS : POLL_NORMAL_MS;
}

// --- Commands ---

void BleGate::open()    { _fastPolling = true; _fastPollUntil = millis() + 5000; _sendCommand(CMD_OPEN, sizeof(CMD_OPEN));       Serial.println("[BLE] CMD: OPEN"); }
void BleGate::close()   { _fastPolling = true; _fastPollUntil = millis() + 5000; _sendCommand(CMD_CLOSE, sizeof(CMD_CLOSE));     Serial.println("[BLE] CMD: CLOSE"); }
void BleGate::partial() { _fastPolling = true; _fastPollUntil = millis() + 5000; _sendCommand(CMD_PARTIAL, sizeof(CMD_PARTIAL)); Serial.println("[BLE] CMD: PARTIAL"); }
void BleGate::light()   { _sendCommand(CMD_LIGHT, sizeof(CMD_LIGHT));     Serial.println("[BLE] CMD: LIGHT"); }
void BleGate::stop()    { _fastPolling = true; _fastPollUntil = millis() + 5000; _sendCommand(CMD_STOP, sizeof(CMD_STOP));       Serial.println("[BLE] CMD: STOP"); }

void BleGate::setEnabled(bool enabled) {
    if (_enabled == enabled) return;
    _enabled = enabled;
    _stateChanged = true;
    _data.bleStatus = enabled ? "Idle" : "Disabled";
    Serial.printf("[BLE] %s\n", enabled ? "Enabled" : "Disabled");
    if (enabled) {
        _lastReconnect = 0;
    }
}

bool BleGate::stateChanged() {
    bool changed = _stateChanged;
    _stateChanged = false;
    return changed;
}

// --- Re-pair ---

void BleGate::clearBondAndRepair() {
    Serial.println("[BLE] Re-pair requested: clearing bond and resetting failure count");
    NimBLEDevice::deleteBond(_address);
    _failCount = 0;
    _lastReconnect = 0;
    _data.bleStatus = "Re-pairing...";
    _stateChanged = true;
    _logFailure("Manual re-pair triggered (bond cleared)");
}

// --- Failure log ---

void BleGate::_logFailure(const char* msg) {
    String entry = "[" + _uptimeStr() + "] " + msg;
    Serial.printf("[BLE-FAIL] %s\n", entry.c_str());
    _failLog[_failLogHead] = entry;
    _failLogHead = (_failLogHead + 1) % FAILURE_LOG_SIZE;
    if (_failLogCount < FAILURE_LOG_SIZE) _failLogCount++;
    _failLogChanged = true;
}

String BleGate::_uptimeStr() const {
    uint32_t s = millis() / 1000;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
    return String(buf);
}

bool BleGate::failureLogChanged() {
    bool changed = _failLogChanged;
    _failLogChanged = false;
    return changed;
}

String BleGate::buildFailureLogJson() const {
    // Entries in chronological order (oldest first)
    String json = "{\"count\":";
    json += _failLogCount;
    json += ",\"last\":\"";
    if (_failLogCount == 0) {
        json += "No failures";
    } else {
        // Last written entry is one slot before head
        uint8_t lastIdx = (_failLogHead + FAILURE_LOG_SIZE - 1) % FAILURE_LOG_SIZE;
        String last = _failLog[lastIdx];
        last.replace("\"", "'");  // avoid breaking JSON
        json += last;
    }
    json += "\",\"entries\":[";
    uint8_t start = (_failLogCount < FAILURE_LOG_SIZE) ? 0 : _failLogHead;
    for (uint8_t i = 0; i < _failLogCount; i++) {
        uint8_t idx = (start + i) % FAILURE_LOG_SIZE;
        String entry = _failLog[idx];
        entry.replace("\"", "'");
        if (i > 0) json += ",";
        json += "\"";
        json += entry;
        json += "\"";
    }
    json += "]}";
    return json;
}
