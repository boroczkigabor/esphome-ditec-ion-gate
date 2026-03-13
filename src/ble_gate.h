#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <functional>

// Ditec BLE constants
static const NimBLEUUID DITEC_SERVICE_UUID("01FF0100-BA5E-F4EE-5CA1-EB1E5E4B1CE0");
static const NimBLEUUID DITEC_CHAR_UUID("01FF0101-BA5E-F4EE-5CA1-EB1E5E4B1CE0");

// Device info GATT UUIDs
static const NimBLEUUID CHAR_MANUFACTURER_UUID((uint16_t)0x2A29);
static const NimBLEUUID CHAR_MODEL_UUID((uint16_t)0x2A24);
static const NimBLEUUID CHAR_HW_REV_UUID((uint16_t)0x2A27);
static const NimBLEUUID CHAR_FW_REV_UUID((uint16_t)0x2A26);
static const NimBLEUUID CHAR_SERIAL_UUID((uint16_t)0x2A25);

// Gate status codes
enum GateStatus : uint8_t {
    STATUS_TRANSITIONING = 0x00,
    STATUS_CLOSED        = 0x01,
    STATUS_OPEN          = 0x03,
    STATUS_STOPPED       = 0x05,
    STATUS_CLOSING       = 0x07,
    STATUS_OPENING       = 0x08,
    STATUS_PARTIAL_OPEN  = 0x09,
    STATUS_IR_REVERSING  = 0x0B,
    STATUS_ERROR         = 0xFF,
};

// Commands
static const uint8_t CMD_OPEN[]    = {0x3D, 0x01, 0x00, 0x00, 0x00};
static const uint8_t CMD_CLOSE[]   = {0x3D, 0x02, 0x00, 0x00, 0x00};
static const uint8_t CMD_PARTIAL[] = {0x3D, 0x03, 0x00, 0x00, 0x00};
static const uint8_t CMD_LIGHT[]   = {0x3D, 0x04, 0x00, 0x00, 0x00};
static const uint8_t CMD_STOP[]    = {0x3D, 0x05, 0x00, 0x00, 0x00};

static const uint8_t CMD_POLL_STATUS[]   = {0x33};
static const uint8_t CMD_POLL_POSITION[] = {0x3E};
static const uint8_t CMD_POLL_SYSTEM[]   = {0x35};
static const uint8_t CMD_POLL_CONFIG[]   = {0x30};

struct GateData {
    // State
    uint8_t  status           = STATUS_CLOSED;
    uint8_t  position         = 0;
    uint8_t  operationCounter = 0;
    bool     bleConnected     = false;
    int      rssi             = 0;
    String   bleStatus        = "Idle";

    // Device info (read once)
    String manufacturer;
    String model;
    String hwRev;
    String fwRev;
    String serial;
    String bleMac;
    uint8_t boardHw           = 0;
    uint8_t boardRev          = 0;
    String  boardFw;
    bool    autoCloseEnabled  = false;
    uint8_t autoCloseTimer    = 0;
    uint8_t partialOpenPct    = 0;

    const char* statusName() const;
    const char* haState() const;
};

class BleGate;

// Server callbacks — handles gate connecting to our GATT server + auth events
class GateServerCallback : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override;
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override;
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;

    volatile bool authComplete = false;
    volatile bool authSuccess = false;
};

// Client callbacks — handles our outgoing connection events
class GateClientCallback : public NimBLEClientCallbacks {
public:
    void onConnect(NimBLEClient* client) override;
    void onDisconnect(NimBLEClient* client, int reason) override;
    void onPassKeyEntry(NimBLEConnInfo& connInfo) override;
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;

    std::function<void(int)> onDisconnectCb;
    uint32_t pin = 0;
    volatile bool authComplete = false;
    volatile bool authSuccess = false;
};

// Callback for when gate writes to our GATT characteristic
class GateResponseCallback : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override;
    std::function<void(const uint8_t*, size_t)> onData;
};

static const uint8_t FAILURE_LOG_SIZE = 20;

class BleGate {
public:
    BleGate(const char* address, uint32_t pin);

    void begin();
    void loop();

    // Commands
    void open();
    void close();
    void partial();
    void light();
    void stop();

    // Enable/disable BLE connection
    void setEnabled(bool enabled);
    bool isEnabled() const { return _enabled; }

    // Explicitly clear bond and trigger fresh re-pair on next connect
    void clearBondAndRepair();

    // State
    const GateData& data() const { return _data; }
    bool stateChanged();

    // Failure log
    bool failureLogChanged();
    String buildFailureLogJson() const;

private:
    void _startGattServer();
    bool _connect();
    void _readDeviceInfo();
    void _sendCommand(const uint8_t* cmd, size_t len);
    void _poll();
    void _parseResponse(const uint8_t* data, size_t len);
    void _disconnect();
    void _logFailure(const char* msg);
    String _uptimeStr() const;

    String          _addressStr;
    NimBLEAddress   _address;
    uint32_t        _pin;
    NimBLEClient*   _client      = nullptr;
    NimBLEServer*   _server      = nullptr;
    NimBLERemoteCharacteristic* _remoteChar = nullptr;

    GateClientCallback    _clientCb;
    GateServerCallback    _serverCb;
    GateResponseCallback  _responseCb;

    GateData _data;
    bool     _enabled        = true;
    bool     _stateChanged   = false;
    bool     _deviceInfoRead = false;
    uint8_t  _pollIndex      = 0;
    bool     _fastPolling    = false;
    unsigned long _fastPollUntil = 0;

    unsigned long _lastPoll       = 0;
    unsigned long _lastReconnect  = 0;
    uint8_t       _failCount      = 0;

    // Circular failure log
    String  _failLog[FAILURE_LOG_SIZE];
    uint8_t _failLogHead  = 0;   // next write position
    uint8_t _failLogCount = 0;   // how many entries filled (0..FAILURE_LOG_SIZE)
    bool    _failLogChanged = false;

    bool _isActiveStatus(uint8_t status) const;
    unsigned long _pollInterval() const;

    static const unsigned long POLL_NORMAL_MS = 1000;
    static const unsigned long POLL_FAST_MS   = 500;
};
