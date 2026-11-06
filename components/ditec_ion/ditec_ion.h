#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <vector>

namespace esphome {
namespace ditec_ion {

namespace espbt = esphome::esp32_ble_tracker;

class DitecIonCover : public cover::Cover, public Component, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void loop() override {}  // work is done from a timer; this is also what BLEClientNode::loop() resolves to
  void dump_config() override;

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  cover::CoverTraits get_traits() override;

  /// Feed one frame received from the gate (esp32_ble_server on_write, or a notification).
  void handle_frame(const std::vector<uint8_t> &frame) { this->handle_frame(frame.data(), frame.size()); }
  void handle_frame(const uint8_t *data, size_t len);

  // Commands (usable from lambdas / template buttons).
  void command_open();
  void command_close();
  void command_stop();
  void command_partial_open();
  void command_toggle_light();

  void set_notify(bool notify) { this->notify_ = notify; }
  void set_reconnect_min_interval(uint32_t ms) { this->reconnect_min_ms_ = ms; }
  void set_reconnect_max_interval(uint32_t ms) { this->reconnect_max_ms_ = ms; }
  void set_operation_counter_sensor(sensor::Sensor *s) { this->operation_counter_ = s; }
  void set_partial_open_position_sensor(sensor::Sensor *s) { this->partial_open_position_ = s; }
  void set_auto_close_timer_sensor(sensor::Sensor *s) { this->auto_close_timer_ = s; }
  void set_auto_close_binary_sensor(binary_sensor::BinarySensor *s) { this->auto_close_ = s; }
  void set_board_info_text_sensor(text_sensor::TextSensor *s) { this->board_info_ = s; }

 protected:
  void control(const cover::CoverCall &call) override;

  bool is_ready_();
  void poll_tick_();
  void send_command_(uint8_t arg);
  void enqueue_(std::vector<uint8_t> frame);
  void pump_tx_();
  void reset_link_state_();
  void update_cover_();
  void reconnect_tick_();

  // Gate state from the last status (0x34) and position (0x3F) frames.
  uint8_t gate_status_{0x01};  // 0x01 = closed
  uint8_t gate_position_{0};

  // GATT link.
  uint16_t char_handle_{0};
  esp_gatt_write_type_t write_type_{ESP_GATT_WRITE_TYPE_NO_RSP};
  bool notify_{false};

  // One write in flight at a time; polls and commands share the queue.
  std::vector<std::vector<uint8_t>> tx_queue_;
  bool tx_busy_{false};
  uint32_t tx_started_{0};

  // Polling / init bookkeeping.
  uint32_t tick_{0};
  bool poll_toggle_{false};
  uint32_t fast_until_{0};
  uint32_t connected_at_{0};
  uint32_t next_init_{0};
  uint8_t init_tries_{0};
  bool have_sysinfo_{false};
  bool have_config_{false};
  bool published_{false};  // cover state published at least once

  // Reconnect back-off (the ble_client must have auto_connect: false; we connect it ourselves).
  uint32_t reconnect_min_ms_{5000};
  uint32_t reconnect_max_ms_{300000};
  uint32_t reconnect_backoff_ms_{5000};
  uint32_t next_connect_{0};
  bool connect_pending_{false};
  bool last_link_good_{false};  // the previous link delivered at least one valid frame

  sensor::Sensor *operation_counter_{nullptr};
  sensor::Sensor *partial_open_position_{nullptr};
  sensor::Sensor *auto_close_timer_{nullptr};
  binary_sensor::BinarySensor *auto_close_{nullptr};
  text_sensor::TextSensor *board_info_{nullptr};
};

}  // namespace ditec_ion
}  // namespace esphome

#endif  // USE_ESP32
