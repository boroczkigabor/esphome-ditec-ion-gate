# Ditec ION4B / ION6B gate for ESPHome

Control a **Ditec ION4B / ION6B** sliding gate (RCB100 Bluetooth module) from **Home Assistant**
with an ESP32 running **ESPHome**. No cloud, no MQTT broker: the gate shows up as a native
ESPHome device with a `cover` entity (open / close / stop, live position) plus a few buttons and
diagnostics. The ESP32 stays a normal ESPHome node, so it can do other jobs as well.

```
Ditec ION4B / ION6B ←──BLE──→ ESP32 (ESPHome) ←──Wi-Fi / native API──→ Home Assistant
```

## Credits

This project is an **ESPHome port of
[trapacska/ditec-ion-gate-home-assistant](https://github.com/trapacska/ditec-ion-gate-home-assistant)**,
an ESP32 firmware (NimBLE + MQTT) that works out how to talk to the RCB100 module. Everything about
the protocol comes from that project: the GATT service and characteristic UUIDs, the pairing
setup (LE Secure Connections with passkey), the command opcodes, the status and position frame
layouts, and the polling scheme. Many thanks to its author. If you do not need ESPHome, or want
the MQTT variant, use the original.

## Status

**Working.** Tested on a real Ditec ION6B: pairing with the PIN, status and position updates, and
open, close, partial open and the signal diagnostics all work. The gate's replies arrive as writes
into the ESP32's own GATT server, which ESPHome's ESP-IDF Bluetooth stack does deliver on a link the
ESP32 opened itself. Other gates and modules are untested; please open an issue with a verbose log
if something does not work. See [Troubleshooting](#troubleshooting).

## What you get

| Entity | Type | Description |
|---|---|---|
| **Gate** | Cover | Open, close, stop, with live position (0–100 %) and state |
| **Light** | Button | Toggle the courtesy light |
| **Partial open** | Button | Open to the partial-open position configured on the gate |
| **Delete pairing** | Button | Drop the stored bond and reconnect (config) |
| **Restart** | Button | Reboot the ESP32 (config) |
| **BLE connection** | Switch | Enable / disable the ESP32's BLE connection to the gate (config) |
| **BLE connected** | Binary sensor | Link state (diagnostic) |
| **BLE signal** | Sensor | Bluetooth RSSI to the gate in dBm (diagnostic) |
| **BLE signal strength** | Sensor | The same as a percentage: −100 dBm = 0 %, −50 dBm = 100 % (diagnostic) |
| **BLE signal quality** | Text sensor | Excellent (≥ 80 %, about −60 dBm), Good (≥ 50 %, −75 dBm), Weak (≥ 30 %, −85 dBm), Poor, or Disconnected (diagnostic) |
| **Wi-Fi signal** | Sensor | Wi-Fi RSSI (diagnostic) |
| **Operation counter**, **Partial open position**, **Auto close timer** | Sensor | Read from the gate (diagnostic) |
| **Auto close** | Binary sensor | Auto-close enabled (diagnostic) |
| **Controller board** | Text sensor | Board hardware / firmware and MAC reported by the gate (diagnostic) |
| **Module firmware** | Text sensor | Firmware revision from the BLE Device Information service (diagnostic) |

## Requirements

- An ESP32 with BLE and an **ESP-IDF** build (the example uses `esp32dev`). The ESP-IDF
  framework is required, not Arduino.
- A Ditec ION4B / ION6B (or other gate with the **RCB100** BLE module), initialised with the
  **Ditec Gate Connect** app (next section).
- Home Assistant with the ESPHome integration, and the ESPHome builder / dashboard.

## Setup

### 1. Initialise the gate with the Ditec Gate Connect app

The gate starts in **temporary (unauthenticated) BLE access** mode. Upgrade it to PIN
authentication before the ESP32 can connect:

1. **Enable temporary access:** press the physical button on the gate motor. This opens an
   enrolment window of about 13 hours.
2. **Open Ditec Gate Connect** and connect to the gate.
3. **Set a PIN** in the app. This switches the gate to PIN-protected BLE Secure Connections.
   Write the PIN down.
4. Note the gate's **BLE MAC address** (Ditec Gate Connect → Info).

From now on the gate only accepts devices that know the PIN.

### 2. Secrets

Add to your ESPHome `secrets.yaml`:

```yaml
wifi_ssid: "YourWiFi"
wifi_password: "YourWiFiPassword"
ditec_gate__api_key: "<ESPHome API encryption key>"
ditec_gate__mac: "00:60:37:XX:XX:XX"
ditec_gate__pin: "123456"
```

### 3. Create the device

Copy [`esphome.yaml`](esphome.yaml) into your ESPHome dashboard as a new device and install it.
It pulls in two things from this repository:

```yaml
external_components:            # the `ditec_ion` cover component (components/ditec_ion/)
  - source: github://boroczkigabor/esphome-ditec-ion-gate@main
    components: [ditec_ion]

packages:                       # BLE pairing, GATT server and entities (ditec-ion-gate.yaml)
  remote_package:
    url: https://github.com/boroczkigabor/esphome-ditec-ion-gate/
    ref: main
    files: [ditec-ion-gate.yaml]
```

Change `name`, `friendly_name` and the board to suit. Once everything works, pin `@main` / `ref:
main` to a tag or commit so repository updates cannot change your gate unexpectedly.

The first flash is over USB; later updates go over the air.

### 4. Add to Home Assistant

The device is discovered by the ESPHome integration (Settings → Devices & services). Add it with
the API key from your secrets. Within a few seconds of the BLE link coming up, the gate cover
shows its real state.

## How it works

- `esp32_ble` is set to *KeyboardDisplay* I/O capability with Secure Connections + MITM +
  bonding. The gate asks for the passkey, and `ble_client.passkey_reply` answers with your PIN.
  The bond is stored, so later reconnects skip pairing.
- The `ditec_ion` component writes commands and polls (status and position, alternating) to the
  gate's characteristic. Polling runs every second, and every 0.5 s while the gate is moving.
- The gate replies by **writing to the same service / characteristic UUIDs on a GATT server hosted
  by the ESP32** (`esp32_ble_server`), not by notification. Each received frame is passed to the
  component with `id(gate).handle_frame(x)`.

## Status mapping

| Gate state | Cover state |
|---|---|
| Closed | `closed` |
| Open / Partial open | `open` |
| Opening, IR reversing, transitioning | `opening` |
| Closing, closing finish (0x0A) | `closing` |
| Stopped | `open`, idle, at the last reported position |

Status codes seen on a real gate: `0x01` closed, `0x09` partial open, `0x07` closing, `0x0A` the
last moments of a close (position about 1 %), and the codes from the original project (`0x00`,
`0x03`, `0x05`, `0x08`, `0x0B`). The position (0–100 %) comes from a separate frame and is
published together with the state.

The cover only supports open, close and stop. Setting an arbitrary position is ignored; use the
**Partial open** button.

## Re-pairing

After changing the PIN in the app, or if pairing is stuck on an old bond: press **Delete
pairing** (it disconnects, removes the bond and reconnects), and check that `ditec_gate__pin` is
correct.

## Reconnecting

The `ble_client` is set to `auto_connect: false` and the component connects it itself, with back-off,
so a gate that is out of range or refuses pairing is not flooded with connection attempts (a
stuck ESP32 can occupy the gate's single Bluetooth connection and lock your phone out). After a
failed attempt the wait doubles, from `reconnect_min_interval` (default 5 s) up to
`reconnect_max_interval` (default 5 min). After a link that worked, the next attempt happens after
the minimum interval. Keep `auto_connect: false` in your `ble_client`; the **BLE connection**
switch still turns connecting on and off.

## Signal strength

Put the ESP32 in clear line of sight of the gate motor and with good Wi-Fi. A poor BLE link causes
supervision-timeout disconnects (HCI error 0x08). Aim for a BLE signal above about −75 dBm.

## Troubleshooting

Raise the log level to see the Bluetooth trace (see the commented `logs:` block in
`esphome.yaml`), then:

- **Never connects:** check the PIN was set in Ditec Gate Connect, and that `ditec_gate__mac` is
  right.
- **Connects, "service/characteristic not found" warning:** the gate may only expose its service
  after encryption. ESPHome discovers services right after connecting, while the original
  firmware discovers them only after pairing finishes. Try **Delete pairing**, and report the log.
- **Pairing fails:** check the PIN; press **Delete pairing**. The original firmware also
  distributes the LINK key, which ESPHome does not expose.
- **Connected, but no status updates ("Status 0x.." lines never appear):** this works on a tested
  ION6B, so first check the pairing (above) and the signal. If it still happens, check with a BLE
  scanner such as nRF Connect whether the gate's characteristic supports Notify; if so, set
  `notify: true` on the cover, which feeds notifications into the same parser.
- **Device falls back to an older firmware after an OTA update** ("OTA rollback detected" in the
  log): ESPHome only accepts a new firmware after it has run for a minute. Do not unplug or
  power-cycle the ESP32 within the first minute after an update.
- **Position looks wrong:** after a gate power cycle the position is invalid until one full open
  followed by one full close.

## Security note

The ESP32's GATT server accepts the gate's status writes. By default any BLE device in range could
write to it. On an ESPHome release that supports it (2026.11 or newer), enable
`write_encrypt: true` on the characteristic in `ditec-ion-gate.yaml` (commented there) so only the
bonded, encrypted link is accepted. Older versions reject the option as invalid.

## Repository layout

| Path | Purpose |
|---|---|
| `components/ditec_ion/` | The ESPHome external component (`cover` platform `ditec_ion`) |
| `ditec-ion-gate.yaml` | ESPHome package with pairing, GATT server and entities |
| `esphome.yaml` | Complete example configuration for end users |

## License

No license is set yet. The protocol knowledge comes from
[trapacska/ditec-ion-gate-home-assistant](https://github.com/trapacska/ditec-ion-gate-home-assistant);
check its terms before choosing a license for this repository.

*Not affiliated with Ditec / Entrematic. Use at your own risk: this controls a motorised gate.*
