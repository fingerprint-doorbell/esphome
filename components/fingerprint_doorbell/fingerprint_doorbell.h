#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/automation.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include <map>
#include <vector>
#include <Adafruit_Fingerprint.h>

namespace esphome {
namespace fingerprint_doorbell {

enum class ScanResult { NO_FINGER, MATCH_FOUND, NO_MATCH_FOUND, ERROR };
enum class Mode { SCAN, ENROLL, IDLE };
enum class EnrollStep { IDLE, WAITING_FOR_FINGER, CONVERTING, WAITING_REMOVE, STORING, DONE };

struct Match {
  ScanResult scan_result = ScanResult::NO_FINGER;
  uint16_t match_id = 0;
  std::string match_name = "unknown";
  uint16_t match_confidence = 0;
  uint8_t return_code = 0;
};

struct LedConfig {
  uint8_t color;
  uint8_t mode;
  uint8_t speed;
};

struct PinCode {
  uint16_t id;
  std::string code;
  std::string name;
};

class FingerprintDoorbell : public Component {
 public:
  FingerprintDoorbell() = default;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  // Configuration setters
  void set_touch_pin(GPIOPin *pin) { touch_pin_ = pin; }
  void set_doorbell_pin(GPIOPin *pin) { doorbell_pin_ = pin; }
  void set_ignore_touch_ring(bool ignore) { ignore_touch_ring_ = ignore; }
  void set_api_token(const std::string &token) { api_token_ = token; }
  void set_keypad_pins(const std::vector<GPIOPin *> &row_pins, const std::vector<GPIOPin *> &col_pins) {
    keypad_row_pins_ = row_pins;
    keypad_col_pins_ = col_pins;
    keypad_enabled_ = true;
  }

  // LED configuration setters
  void set_led_ready(uint8_t color, uint8_t mode, uint8_t speed) {
    led_ready_ = {color, mode, speed};
  }
  void set_led_error(uint8_t color, uint8_t mode, uint8_t speed) {
    led_error_ = {color, mode, speed};
  }
  void set_led_enroll(uint8_t color, uint8_t mode, uint8_t speed) {
    led_enroll_ = {color, mode, speed};
  }
  void set_led_match(uint8_t color, uint8_t mode, uint8_t speed) {
    led_match_ = {color, mode, speed};
  }
  void set_led_scanning(uint8_t color, uint8_t mode, uint8_t speed) {
    led_scanning_ = {color, mode, speed};
  }
  void set_led_no_match(uint8_t color, uint8_t mode, uint8_t speed) {
    led_no_match_ = {color, mode, speed};
  }

  // Sensor setters
  void set_match_id_sensor(sensor::Sensor *sensor) { match_id_sensor_ = sensor; }
  void set_confidence_sensor(sensor::Sensor *sensor) { confidence_sensor_ = sensor; }
  void set_match_name_sensor(text_sensor::TextSensor *sensor) { match_name_sensor_ = sensor; }
  void set_ring_sensor(binary_sensor::BinarySensor *sensor) { ring_sensor_ = sensor; }
  void set_finger_sensor(binary_sensor::BinarySensor *sensor) { finger_sensor_ = sensor; }
  void set_enroll_status_sensor(text_sensor::TextSensor *sensor) { enroll_status_sensor_ = sensor; }
  void set_last_action_sensor(text_sensor::TextSensor *sensor) { last_action_sensor_ = sensor; }
  void set_pin_match_name_sensor(text_sensor::TextSensor *sensor) { pin_match_name_sensor_ = sensor; }
  void set_invalid_action_sensor(text_sensor::TextSensor *sensor) { invalid_action_sensor_ = sensor; }
  void set_pin_invalid_sensor(binary_sensor::BinarySensor *sensor) { pin_invalid_sensor_ = sensor; }
  void set_lock_action_sensor(binary_sensor::BinarySensor *sensor) { lock_action_sensor_ = sensor; }
  void set_unlock_action_sensor(binary_sensor::BinarySensor *sensor) { unlock_action_sensor_ = sensor; }
  void set_min_unlock_confidence(uint8_t confidence) { min_unlock_confidence_ = confidence; }

  // Public methods for HA services and REST API
  void start_enrollment(uint16_t id, const std::string &name);
  void cancel_enrollment();
  bool delete_fingerprint(uint16_t id);
  bool delete_all_fingerprints();
  bool rename_fingerprint(uint16_t id, const std::string &new_name);
  void set_ignore_touch_ring_state(bool state) { ignore_touch_ring_ = state; }
  uint16_t get_enrolled_count();
  std::string get_fingerprint_name(uint16_t id);
  std::string get_fingerprint_list_json();
  bool is_enrolling() { return mode_ == Mode::ENROLL; }
  bool is_sensor_connected() { return sensor_connected_; }
  bool is_sensor_paired() { return sensor_paired_; }
  std::string get_api_token() { return api_token_; }
  
  // Sensor pairing - requires password to be set before sensor works
  bool pair_sensor(uint32_t password);
  bool unpair_sensor();
  bool factory_reset_sensor(uint32_t old_password = 0xFFFFFFFF);
  
  // Template transfer methods for copying fingerprints between devices
  bool get_template(uint16_t id, std::vector<uint8_t> &template_data);
  bool upload_template(uint16_t id, const std::string &name, const std::vector<uint8_t> &template_data);
  
  // PIN code management
  bool add_pin_code(uint16_t id, const std::string &code, const std::string &name);
  bool delete_pin_code(uint16_t id);
  bool delete_all_pin_codes();
  bool rename_pin_code(uint16_t id, const std::string &new_name);
  bool update_pin_code(uint16_t id, const std::string &new_code);
  std::string get_pin_code_list_json();
  std::string export_pin_code_json(uint16_t id);
  uint16_t get_pin_code_count();
  bool is_keypad_enabled() { return keypad_enabled_; }

 protected:
  GPIOPin *touch_pin_{nullptr};
  GPIOPin *doorbell_pin_{nullptr};
  bool ignore_touch_ring_{false};
  bool last_ignore_touch_ring_{false};
  std::string api_token_{};

  // LED configurations (color, mode, speed)
  LedConfig led_ready_{2, 1, 100};    // blue, breathing, speed 100
  LedConfig led_error_{1, 3, 0};      // red, on, speed 0
  LedConfig led_enroll_{3, 2, 25};    // purple, flashing, speed 25
  LedConfig led_match_{3, 3, 0};      // purple, on, speed 0
  LedConfig led_scanning_{2, 2, 25};  // blue, flashing, speed 25
  LedConfig led_no_match_{1, 2, 25};  // red, flashing, speed 25

  // Unlock action configuration
  uint8_t min_unlock_confidence_{80};

  // Sensors
  sensor::Sensor *match_id_sensor_{nullptr};
  sensor::Sensor *confidence_sensor_{nullptr};
  text_sensor::TextSensor *match_name_sensor_{nullptr};
  binary_sensor::BinarySensor *ring_sensor_{nullptr};
  binary_sensor::BinarySensor *finger_sensor_{nullptr};
  text_sensor::TextSensor *enroll_status_sensor_{nullptr};
  text_sensor::TextSensor *last_action_sensor_{nullptr};
  text_sensor::TextSensor *pin_match_name_sensor_{nullptr};
  text_sensor::TextSensor *invalid_action_sensor_{nullptr};
  binary_sensor::BinarySensor *pin_invalid_sensor_{nullptr};
  binary_sensor::BinarySensor *lock_action_sensor_{nullptr};
  binary_sensor::BinarySensor *unlock_action_sensor_{nullptr};

  // Internal state
  Adafruit_Fingerprint *finger_{nullptr};
  HardwareSerial *hw_serial_{nullptr};
  bool sensor_connected_{false};
  bool last_touch_state_{false};
  std::map<uint16_t, std::string> fingerprint_names_;
  
  uint32_t last_match_time_{0};
  uint32_t last_ring_time_{0};
  uint16_t last_match_id_{0};
  uint32_t last_connect_attempt_{0};
  uint8_t connect_attempts_{0};

  // Enrollment state machine
  Mode mode_{Mode::SCAN};
  EnrollStep enroll_step_{EnrollStep::IDLE};
  uint16_t enroll_id_{0};
  std::string enroll_name_;
  uint8_t enroll_sample_{0};
  uint32_t enroll_timeout_{0};

  // Sensor pairing state
  bool sensor_paired_{false};
  uint32_t sensor_password_{0};

  // Keypad state
  bool keypad_enabled_{false};
  std::vector<GPIOPin *> keypad_row_pins_;
  std::vector<GPIOPin *> keypad_col_pins_;
  std::string keypad_buffer_;
  uint32_t keypad_last_key_time_{0};
  char keypad_last_key_{0};
  uint32_t keypad_last_scan_time_{0};
  bool keypad_input_active_{false};  // Track if PIN input is in progress
  std::map<uint16_t, PinCode> pin_codes_;

  // Internal methods
  void load_sensor_password();
  void save_sensor_password();
  bool connect_sensor();
  bool reset_sensor_to_default();
  bool raw_verify_and_reset_password(uint32_t old_password);
  bool raw_set_password(uint32_t new_password);
  Match scan_fingerprint();
  void process_enrollment();
  void update_touch_state(bool touched);
  bool is_ring_touched();
  void set_led_ring_ready();
  void set_led_ring_error();
  void set_led_ring_enroll();
  void set_led_ring_match();
  void set_led_ring_scanning();
  void set_led_ring_no_match();
  void load_fingerprint_names();
  void save_fingerprint_name(uint16_t id, const std::string &name);
  void delete_fingerprint_name(uint16_t id);
  void publish_enroll_status(const std::string &status);
  void publish_last_action(const std::string &action);
  void setup_web_server();
  
  // Keypad methods
  void setup_keypad();
  void scan_keypad();
  char get_pressed_key();
  void process_keypad_input(char key);
  void verify_pin_code();
  void trigger_lock_action();
  void trigger_unlock_action(uint16_t confidence = 255);
  void load_pin_codes();
  void save_pin_code(uint16_t id, const std::string &code, const std::string &name);
  void delete_pin_code_storage(uint16_t id);
};

// ==================== AUTOMATION ACTIONS ====================

template<typename... Ts>
class EnrollAction : public Action<Ts...>, public Parented<FingerprintDoorbell> {
 public:
  TEMPLATABLE_VALUE(uint16_t, finger_id)
  TEMPLATABLE_VALUE(std::string, name)

  void play(Ts... x) override {
    this->parent_->start_enrollment(this->finger_id_.value(x...), this->name_.value(x...));
  }
};

template<typename... Ts>
class CancelEnrollAction : public Action<Ts...>, public Parented<FingerprintDoorbell> {
 public:
  void play(Ts... x) override {
    this->parent_->cancel_enrollment();
  }
};

template<typename... Ts>
class DeleteAction : public Action<Ts...>, public Parented<FingerprintDoorbell> {
 public:
  TEMPLATABLE_VALUE(uint16_t, finger_id)

  void play(Ts... x) override {
    this->parent_->delete_fingerprint(this->finger_id_.value(x...));
  }
};

template<typename... Ts>
class DeleteAllAction : public Action<Ts...>, public Parented<FingerprintDoorbell> {
 public:
  void play(Ts... x) override {
    this->parent_->delete_all_fingerprints();
  }
};

template<typename... Ts>
class RenameAction : public Action<Ts...>, public Parented<FingerprintDoorbell> {
 public:
  TEMPLATABLE_VALUE(uint16_t, finger_id)
  TEMPLATABLE_VALUE(std::string, name)

  void play(Ts... x) override {
    this->parent_->rename_fingerprint(this->finger_id_.value(x...), this->name_.value(x...));
  }
};

}  // namespace fingerprint_doorbell
}  // namespace esphome
