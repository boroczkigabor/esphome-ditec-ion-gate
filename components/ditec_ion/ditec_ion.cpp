#include "ditec_ion.h"
#include <algorithm>
#include <cmath>
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32

namespace esphome {
namespace ditec_ion {

static const char *const TAG = "ditec_ion";

// The gate is polled about once a second; only publish when a value actually changed so Home
// Assistant (and anything exporting its states) is not flooded with identical updates.
static void publish_if_changed(sensor::Sensor *s, float v) {
  if (s != nullptr && (!s->has_state() || s->state != v))
    s->publish_state(v);
}
static void publish_if_changed(binary_sensor::BinarySensor *s, bool v) {
  if (s != nullptr && (!s->has_state() || s->state != v))
    s->publish_state(v);
}
static void publish_if_changed(text_sensor::TextSensor *s, const std::string &v) {
  if (s != nullptr && (!s->has_state() || s->state != v))
    s->publish_state(v);
}

// Protocol constants, taken from trapacska/ditec-ion-gate-home-assistant (src/ble_gate.h).
static const char *const SERVICE_UUID = "01FF0100-BA5E-F4EE-5CA1-EB1E5E4B1CE0";
static const char *const CHAR_UUID = "01FF0101-BA5E-F4EE-5CA1-EB1E5E4B1CE0";

// Command: {0x3D, arg, 0, 0, 0}
static const uint8_t OP_COMMAND = 0x3D;
static const uint8_t ARG_OPEN = 0x01;
static const uint8_t ARG_CLOSE = 0x02;
static const uint8_t ARG_PARTIAL = 0x03;
static const uint8_t ARG_LIGHT = 0x04;
static const uint8_t ARG_STOP = 0x05;

// Single-byte polls and the frame type each one is answered with.
static const uint8_t OP_POLL_STATUS = 0x33;    // -> FRAME_STATUS
static const uint8_t OP_POLL_POSITION = 0x3E;  // -> FRAME_POSITION
static const uint8_t OP_POLL_SYSTEM = 0x35;    // -> FRAME_SYSTEM
static const uint8_t OP_POLL_CONFIG = 0x30;    // -> FRAME_CONFIG
static const uint8_t FRAME_STATUS = 0x34;
static const uint8_t FRAME_POSITION = 0x3F;
static const uint8_t FRAME_SYSTEM = 0x36;
static const uint8_t FRAME_CONFIG = 0x32;

// Status codes (frame 0x34, byte 1).
static const uint8_t STATUS_TRANSITIONING = 0x00;
static const uint8_t STATUS_CLOSED = 0x01;
static const uint8_t STATUS_OPEN = 0x03;
static const uint8_t STATUS_CLOSING = 0x07;
static const uint8_t STATUS_OPENING = 0x08;
static const uint8_t STATUS_IR_REVERSING = 0x0B;

static const uint32_t POLL_TICK_MS = 500;      // alternate status / position: 1 s normally, 0.5 s while moving
static const uint32_t FAST_POLL_HOLD_MS = 5000;
static const uint32_t INIT_FIRST_DELAY_MS = 2000;  // let pairing / encryption settle
static const uint32_t INIT_RETRY_MS = 4000;
static const uint8_t INIT_MAX_TRIES = 6;
static const uint32_t TX_TIMEOUT_MS = 2000;
static const size_t TX_QUEUE_MAX = 8;

void DitecIonCover::setup() {
  this->position = cover::COVER_CLOSED;
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->reconnect_backoff_ms_ = this->reconnect_min_ms_;
  this->set_interval("ditec_poll", POLL_TICK_MS, [this]() { this->poll_tick_(); });
  this->set_interval("ditec_reconnect", 1000, [this]() { this->reconnect_tick_(); });
  if (this->signal_dbm_ != nullptr || this->signal_percent_ != nullptr || this->signal_quality_ != nullptr)
    this->set_interval("ditec_rssi", this->rssi_interval_ms_, [this]() { this->poll_rssi_(); });
}

// ---- Bluetooth signal strength ---------------------------------------------------------------
// dBm -> percent: -100 dBm (or worse) = 0 %, -50 dBm (or better) = 100 %, linear in between.
// Quality text, from the percentage: >= 80 % (-60 dBm) Excellent, >= 50 % (-75 dBm) Good,
// >= 30 % (-85 dBm) Weak, below that Poor. Disconnected -> "Disconnected" / unknown values.
void DitecIonCover::poll_rssi_() {
  if (!this->is_ready_())
    return;
  esp_err_t err = esp_ble_gap_read_rssi(this->parent()->get_remote_bda());
  if (err != ESP_OK)
    ESP_LOGW(TAG, "esp_ble_gap_read_rssi failed: %d", (int) err);
}

void DitecIonCover::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (event != ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT)
    return;
  if (!this->parent()->check_addr(param->read_rssi_cmpl.remote_addr))
    return;
  if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS)
    this->publish_signal_(true, param->read_rssi_cmpl.rssi);
}

void DitecIonCover::publish_signal_(bool valid, int rssi) {
  float dbm = NAN;
  float pct = NAN;
  const char *quality = "Disconnected";
  if (valid) {
    dbm = (float) rssi;
    int p = std::max(0, std::min(100, (rssi + 100) * 2));
    pct = (float) p;
    quality = p >= 80 ? "Excellent" : p >= 50 ? "Good" : p >= 30 ? "Weak" : "Poor";
  }
  if (this->signal_dbm_ != nullptr && (!valid || !this->signal_dbm_->has_state() || this->signal_dbm_->state != dbm))
    this->signal_dbm_->publish_state(dbm);
  if (this->signal_percent_ != nullptr &&
      (!valid || !this->signal_percent_->has_state() || this->signal_percent_->state != pct))
    this->signal_percent_->publish_state(pct);
  if (this->signal_quality_ != nullptr)
    publish_if_changed(this->signal_quality_, std::string(quality));
}

// Connects the ble_client ourselves, with exponential back-off, so a gate that is out of range or
// refuses pairing is not hammered with connection attempts. After a link that worked (at least one
// valid frame was received) the next attempt is made after the minimum interval.
void DitecIonCover::reconnect_tick_() {
  auto *client = this->parent();
  if (!client->enabled || client->state() != espbt::ClientState::IDLE) {
    this->connect_pending_ = false;
    return;
  }
  const uint32_t now = millis();
  if (!this->connect_pending_) {
    uint32_t delay;
    if (this->last_link_good_) {
      this->reconnect_backoff_ms_ = this->reconnect_min_ms_;
      delay = this->reconnect_min_ms_;
    } else {
      delay = this->reconnect_backoff_ms_;
      this->reconnect_backoff_ms_ = std::min(this->reconnect_backoff_ms_ * 2, this->reconnect_max_ms_);
    }
    this->last_link_good_ = false;
    this->next_connect_ = now + delay;
    this->connect_pending_ = true;
    ESP_LOGD(TAG, "Next connection attempt in %u ms", (unsigned) delay);
    return;
  }
  if ((int32_t) (now - this->next_connect_) >= 0) {
    this->connect_pending_ = false;
    client->connect();
  }
}

void DitecIonCover::dump_config() {
  LOG_COVER("", "Ditec ION Cover", this);
  ESP_LOGCONFIG(TAG, "  Notifications: %s", YESNO(this->notify_));
  ESP_LOGCONFIG(TAG, "  Reconnect interval: %u ms .. %u ms", (unsigned) this->reconnect_min_ms_,
                (unsigned) this->reconnect_max_ms_);
}

cover::CoverTraits DitecIonCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_is_assumed_state(false);
  traits.set_supports_position(true);
  traits.set_supports_stop(true);
  return traits;
}

void DitecIonCover::control(const cover::CoverCall &call) {
  if (call.get_stop()) {
    this->command_stop();
  }
  auto pos = call.get_position();
  if (pos.has_value()) {
    if (*pos >= cover::COVER_OPEN) {
      this->command_open();
    } else if (*pos <= cover::COVER_CLOSED) {
      this->command_close();
    } else {
      ESP_LOGW(TAG, "Arbitrary positions are not supported by the gate; use open, close or the partial-open command");
    }
  }
}

void DitecIonCover::command_open() { this->send_command_(ARG_OPEN); }
void DitecIonCover::command_close() { this->send_command_(ARG_CLOSE); }
void DitecIonCover::command_stop() { this->send_command_(ARG_STOP); }
void DitecIonCover::command_partial_open() { this->send_command_(ARG_PARTIAL); }
void DitecIonCover::command_toggle_light() { this->send_command_(ARG_LIGHT); }

void DitecIonCover::send_command_(uint8_t arg) {
  ESP_LOGD(TAG, "Command 0x%02X", arg);
  if (arg != ARG_LIGHT) {
    this->fast_until_ = millis() + FAST_POLL_HOLD_MS;
  }
  this->enqueue_({OP_COMMAND, arg, 0x00, 0x00, 0x00});
  this->pump_tx_();
}

bool DitecIonCover::is_ready_() { return this->parent()->connected() && this->char_handle_ != 0; }

void DitecIonCover::enqueue_(std::vector<uint8_t> frame) {
  if (this->tx_queue_.size() >= TX_QUEUE_MAX) {
    ESP_LOGW(TAG, "TX queue full, dropping 0x%02X", frame.empty() ? 0 : frame[0]);
    return;
  }
  this->tx_queue_.push_back(std::move(frame));
}

void DitecIonCover::pump_tx_() {
  if (this->tx_busy_ || this->tx_queue_.empty() || !this->is_ready_())
    return;
  auto &frame = this->tx_queue_.front();
  esp_err_t err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                           this->char_handle_, frame.size(), frame.data(), this->write_type_,
                                           ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Write of 0x%02X failed: %d", frame[0], (int) err);
    this->tx_queue_.erase(this->tx_queue_.begin());
    return;
  }
  this->tx_busy_ = true;
  this->tx_started_ = millis();
}

void DitecIonCover::poll_tick_() {
  if (!this->is_ready_())
    return;
  const uint32_t now = millis();

  // A write whose completion event never arrived must not block the queue forever.
  if (this->tx_busy_ && now - this->tx_started_ > TX_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Write timed out");
    this->tx_busy_ = false;
    if (!this->tx_queue_.empty())
      this->tx_queue_.erase(this->tx_queue_.begin());
  }

  // System info + config once, retried until both frames have arrived.
  if (!(this->have_sysinfo_ && this->have_config_) && this->init_tries_ < INIT_MAX_TRIES &&
      now - this->connected_at_ >= INIT_FIRST_DELAY_MS && (int32_t) (now - this->next_init_) >= 0) {
    this->enqueue_({OP_POLL_SYSTEM});
    this->enqueue_({OP_POLL_CONFIG});
    this->init_tries_++;
    this->next_init_ = now + INIT_RETRY_MS;
  }

  // Alternate status / position polls; twice as often while the gate is moving.
  this->tick_++;
  const bool fast = (int32_t) (this->fast_until_ - now) > 0;
  if ((fast || (this->tick_ % 2) == 0) && this->tx_queue_.empty() && !this->tx_busy_) {
    this->poll_toggle_ = !this->poll_toggle_;
    this->enqueue_({this->poll_toggle_ ? OP_POLL_POSITION : OP_POLL_STATUS});
  }

  this->pump_tx_();
}

void DitecIonCover::reset_link_state_() {
  this->char_handle_ = 0;
  this->tx_queue_.clear();
  this->tx_busy_ = false;
  this->have_sysinfo_ = false;
  this->have_config_ = false;
  this->init_tries_ = 0;
}

void DitecIonCover::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      this->reset_link_state_();
      auto *chr = this->parent()->get_characteristic(espbt::ESPBTUUID::from_raw(SERVICE_UUID),
                                                     espbt::ESPBTUUID::from_raw(CHAR_UUID));
      if (chr == nullptr) {
        ESP_LOGW(TAG, "Gate service/characteristic not found (is a PIN set in Ditec Gate Connect?)");
        this->status_set_warning();
        this->node_state = espbt::ClientState::ESTABLISHED;
        break;
      }
      this->status_clear_warning();
      this->char_handle_ = chr->handle;
      // The original project always writes without response (writeValue(..., false)), so prefer that
      // whenever the characteristic allows it; fall back to acknowledged writes otherwise.
      this->write_type_ = (chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) ? ESP_GATT_WRITE_TYPE_NO_RSP
                                                                             : ESP_GATT_WRITE_TYPE_RSP;
      this->connected_at_ = millis();
      this->next_init_ = this->connected_at_;
      this->poll_rssi_();
      ESP_LOGI(TAG, "Gate characteristic found (handle 0x%04X, %s writes, props 0x%02X)", this->char_handle_,
               this->write_type_ == ESP_GATT_WRITE_TYPE_RSP ? "acknowledged" : "unacknowledged", chr->properties);
      if (this->notify_ && (chr->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) {
        auto status = this->parent()->register_for_notify(chr->handle);
        if (status) {
          ESP_LOGW(TAG, "register_for_notify failed, status=%d", (int) status);
          this->node_state = espbt::ClientState::ESTABLISHED;
        }
        // otherwise ESP_GATTC_REG_FOR_NOTIFY_EVT below marks this node established
      } else {
        if (this->notify_) {
          ESP_LOGW(TAG, "notify: true, but the gate characteristic has no Notify property");
        }
        this->node_state = espbt::ClientState::ESTABLISHED;
      }
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle == this->char_handle_)
        this->node_state = espbt::ClientState::ESTABLISHED;
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle == this->char_handle_ && this->char_handle_ != 0)
        this->handle_frame(param->notify.value, param->notify.value_len);
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.handle != this->char_handle_ || this->char_handle_ == 0)
        break;
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Write failed, status=%d", (int) param->write.status);
      }
      this->tx_busy_ = false;
      if (!this->tx_queue_.empty())
        this->tx_queue_.erase(this->tx_queue_.begin());
      this->pump_tx_();
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT: {
      this->reset_link_state_();
      this->publish_signal_(false, 0);
      break;
    }
    default:
      break;
  }
}

void DitecIonCover::handle_frame(const uint8_t *d, size_t n) {
  if (n < 2)
    return;
  ESP_LOGV(TAG, "RX %s", format_hex_pretty(d, n).c_str());
  this->last_link_good_ = true;

  switch (d[0]) {
    case FRAME_STATUS: {
      this->gate_status_ = d[1];
      ESP_LOGD(TAG, "Status 0x%02X", d[1]);
      break;
    }
    case FRAME_POSITION: {
      if (n < 10)
        return;
      this->gate_position_ = d[1];
      publish_if_changed(this->operation_counter_, d[9]);
      break;
    }
    case FRAME_SYSTEM: {
      if (n < 42)
        return;
      char buf[96];
      snprintf(buf, sizeof(buf), "HW %u rev %u, FW %u.%u.%u, MAC %02X:%02X:%02X:%02X:%02X:%02X", d[4], d[5], d[6], d[7],
               d[8], d[41], d[40], d[39], d[38], d[37], d[36]);
      publish_if_changed(this->board_info_, std::string(buf));
      this->have_sysinfo_ = true;
      return;
    }
    case FRAME_CONFIG: {
      if (n <= 50)
        return;
      publish_if_changed(this->auto_close_, d[7] != 0);
      publish_if_changed(this->auto_close_timer_, d[8]);
      publish_if_changed(this->partial_open_position_, d[25]);
      this->have_config_ = true;
      return;
    }
    default:
      ESP_LOGD(TAG, "Unhandled frame 0x%02X (%u bytes)", d[0], (unsigned) n);
      return;
  }

  const uint8_t s = this->gate_status_;
  if (s == STATUS_OPENING || s == STATUS_CLOSING || s == STATUS_IR_REVERSING || s == STATUS_TRANSITIONING)
    this->fast_until_ = millis() + FAST_POLL_HOLD_MS;
  this->update_cover_();
}

void DitecIonCover::update_cover_() {
  const uint8_t s = this->gate_status_;
  const int p = this->gate_position_;

  auto op = cover::COVER_OPERATION_IDLE;
  if (s == STATUS_OPENING || s == STATUS_IR_REVERSING || s == STATUS_TRANSITIONING)
    op = cover::COVER_OPERATION_OPENING;
  else if (s == STATUS_CLOSING)
    op = cover::COVER_OPERATION_CLOSING;

  float pos = p / 100.0f;
  if (s == STATUS_CLOSED)
    pos = cover::COVER_CLOSED;
  else if (s == STATUS_OPEN && p == 0)
    pos = cover::COVER_OPEN;  // open, but the position is not calibrated yet
  else if (pos < 0.01f)
    pos = 0.01f;  // never report "closed" unless the gate says so
  if (pos > cover::COVER_OPEN)
    pos = cover::COVER_OPEN;

  if (this->published_ && this->current_operation == op && this->position == pos)
    return;
  this->published_ = true;
  this->current_operation = op;
  this->position = pos;
  this->publish_state(false);
}

}  // namespace ditec_ion
}  // namespace esphome

#endif  // USE_ESP32
