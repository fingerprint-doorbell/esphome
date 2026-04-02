#include "fingerprint_doorbell.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/core/helpers.h"
#include "esphome/core/application.h"

namespace esphome {
namespace fingerprint_doorbell {

static const char *const TAG = "fingerprint_doorbell";

// Use Serial2 exactly like the original working code
#define mySerial Serial2

void FingerprintDoorbell::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Fingerprint Doorbell...");

  // Setup pins
  if (this->touch_pin_ != nullptr) {
    this->touch_pin_->setup();
    this->touch_pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLDOWN);
  }

  if (this->doorbell_pin_ != nullptr) {
    this->doorbell_pin_->setup();
    this->doorbell_pin_->pin_mode(gpio::FLAG_OUTPUT);
    this->doorbell_pin_->digital_write(false);
  }

  // Initialize serial pointer (finger_ object created in connect_sensor with correct password)
  this->hw_serial_ = &mySerial;
  
  ESP_LOGI(TAG, "Using Serial2 with default pins (RX=GPIO16, TX=GPIO17)");
  this->sensor_connected_ = false;
  this->mode_ = Mode::SCAN;
  
  // Setup keypad if configured
  if (this->keypad_enabled_) {
    this->setup_keypad();
    this->load_pin_codes();
  }
  
  // Setup REST API
  this->setup_web_server();
}

void FingerprintDoorbell::loop() {
  // Scan keypad if enabled (always, independent of fingerprint sensor)
  if (this->keypad_enabled_) {
    this->scan_keypad();
  }
  
  // Connect sensor if not connected (throttled to every 5 seconds)
  if (!this->sensor_connected_) {
    uint32_t now = millis();
    if (now - this->last_connect_attempt_ >= 5000) {
      this->last_connect_attempt_ = now;
      this->sensor_connected_ = this->connect_sensor();
      if (this->sensor_connected_) {
        ESP_LOGI(TAG, "Fingerprint sensor connected successfully");
        this->load_fingerprint_names();
        this->set_led_ring_ready();
        this->publish_last_action("Sensor connected");
      }
    }
    return;
  }

  // Handle different modes
  if (this->mode_ == Mode::ENROLL) {
    this->process_enrollment();
    return;
  }

  // Cooldown after no-match to keep LED visible
  if (this->last_ring_time_ > 0 && millis() - this->last_ring_time_ < 1000) {
    return;
  } else if (this->last_ring_time_ > 0) {
    // Cooldown just expired, return to ready state
    this->last_ring_time_ = 0;
    this->set_led_ring_ready();
  }
  
  // Cooldown after match to keep LED visible
  if (this->last_match_time_ > 0 && millis() - this->last_match_time_ < 1000) {
    return;
  } else if (this->last_match_time_ > 0) {
    // Cooldown just expired, return to ready state
    this->last_match_time_ = 0;
    this->set_led_ring_ready();
  }

  // Normal scan mode
  Match match = this->scan_fingerprint();

  // Handle match found
  if (match.scan_result == ScanResult::MATCH_FOUND) {
    ESP_LOGI(TAG, "Match: ID=%d, Name=%s, Confidence=%d", 
             match.match_id, match.match_name.c_str(), match.match_confidence);
    
    // Set match LED here (after scan completes) to ensure it's visible
    this->set_led_ring_match();
    
    if (this->match_id_sensor_ != nullptr)
      this->match_id_sensor_->publish_state(match.match_id);
    if (this->confidence_sensor_ != nullptr)
      this->confidence_sensor_->publish_state(match.match_confidence);
    if (this->match_name_sensor_ != nullptr)
      this->match_name_sensor_->publish_state(match.match_name);
    
    this->publish_last_action("Match: " + match.match_name);
    this->last_match_time_ = millis();
    this->last_match_id_ = match.match_id;
    
    // Clear after 3 seconds
    this->set_timeout(3000, [this]() {
      if (this->match_id_sensor_ != nullptr)
        this->match_id_sensor_->publish_state(-1);
      if (this->match_name_sensor_ != nullptr)
        this->match_name_sensor_->publish_state("");
      if (this->confidence_sensor_ != nullptr)
        this->confidence_sensor_->publish_state(0);
    });
  }
  // Handle no match (doorbell ring)
  else if (match.scan_result == ScanResult::NO_MATCH_FOUND) {
    ESP_LOGI(TAG, "No match - doorbell ring!");
    
    // Show no match LED
    this->set_led_ring_no_match();
    
    if (this->ring_sensor_ != nullptr)
      this->ring_sensor_->publish_state(true);
    if (this->doorbell_pin_ != nullptr)
      this->doorbell_pin_->digital_write(true);
    
    this->publish_last_action("Doorbell ring");
    this->last_ring_time_ = millis();
    
    // Clear doorbell output after 1 second
    this->set_timeout(1000, [this]() {
      if (this->ring_sensor_ != nullptr)
        this->ring_sensor_->publish_state(false);
      if (this->doorbell_pin_ != nullptr)
        this->doorbell_pin_->digital_write(false);
      // LED ready state is handled by cooldown logic in loop()
    });
  }

  // Update finger detection sensor
  if (this->finger_sensor_ != nullptr) {
    bool finger_present = (match.scan_result != ScanResult::NO_FINGER);
    this->finger_sensor_->publish_state(finger_present);
  }
}

void FingerprintDoorbell::dump_config() {
  ESP_LOGCONFIG(TAG, "Fingerprint Doorbell:");
  LOG_PIN("  Touch Pin: ", this->touch_pin_);
  LOG_PIN("  Doorbell Pin: ", this->doorbell_pin_);
  ESP_LOGCONFIG(TAG, "  Ignore Touch Ring: %s", YESNO(this->ignore_touch_ring_));
  ESP_LOGCONFIG(TAG, "  Sensor Connected: %s", YESNO(this->sensor_connected_));
  
  // LED configuration debug
  ESP_LOGCONFIG(TAG, "  LED Ready: color=%d, mode=%d, speed=%d", this->led_ready_.color, this->led_ready_.mode, this->led_ready_.speed);
  ESP_LOGCONFIG(TAG, "  LED Error: color=%d, mode=%d, speed=%d", this->led_error_.color, this->led_error_.mode, this->led_error_.speed);
  ESP_LOGCONFIG(TAG, "  LED Enroll: color=%d, mode=%d, speed=%d", this->led_enroll_.color, this->led_enroll_.mode, this->led_enroll_.speed);
  ESP_LOGCONFIG(TAG, "  LED Match: color=%d, mode=%d, speed=%d", this->led_match_.color, this->led_match_.mode, this->led_match_.speed);
  ESP_LOGCONFIG(TAG, "  LED Scanning: color=%d, mode=%d, speed=%d", this->led_scanning_.color, this->led_scanning_.mode, this->led_scanning_.speed);
  ESP_LOGCONFIG(TAG, "  LED No Match: color=%d, mode=%d, speed=%d", this->led_no_match_.color, this->led_no_match_.mode, this->led_no_match_.speed);
  
  if (this->sensor_connected_ && this->finger_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Sensor Capacity: %d", this->finger_->capacity);
    ESP_LOGCONFIG(TAG, "  Enrolled Templates: %d", this->finger_->templateCount);
  }
}

bool FingerprintDoorbell::connect_sensor() {
  ESP_LOGI(TAG, "Connecting to fingerprint sensor (attempt %d)...", this->connect_attempts_ + 1);

  // Only initialize serial and load password on first attempt
  if (this->connect_attempts_ == 0) {
    // Load stored password from preferences
    this->load_sensor_password();
    
    // Explicitly initialize Serial2 with pins for ESP-IDF framework
    // RX=GPIO16, TX=GPIO17 are the default Serial2 pins on ESP32
    mySerial.begin(57600, SERIAL_8N1, 16, 17);
    delay(100);  // Give serial time to initialize
    App.feed_wdt();
    
    // Create Adafruit_Fingerprint with the appropriate password
    uint32_t password = this->sensor_paired_ ? this->sensor_password_ : 0x00000000;
    if (this->finger_ != nullptr) {
      delete this->finger_;
    }
    this->finger_ = new Adafruit_Fingerprint(this->hw_serial_, password);
    
    if (this->sensor_paired_) {
      ESP_LOGI(TAG, "Using stored password for paired sensor");
    } else {
      ESP_LOGI(TAG, "Using default password (sensor unpaired)");
    }
    
    // Now tell Adafruit library what baud rate we're using
    this->finger_->begin(57600);
    App.feed_wdt();
  }
  
  this->connect_attempts_++;
  
  // Try to verify password (non-blocking - just one attempt per call)
  if (this->finger_->verifyPassword()) {
    ESP_LOGI(TAG, "Found fingerprint sensor!");
    
    // Startup LED signal
    this->finger_->LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_BLUE, 0);

    // Read sensor parameters
    ESP_LOGI(TAG, "Reading sensor parameters");
    this->finger_->getParameters();
    ESP_LOGI(TAG, "Status: 0x%02X, Capacity: %d, Security: %d", 
             this->finger_->status_reg, this->finger_->capacity, this->finger_->security_level);

    this->finger_->getTemplateCount();
    ESP_LOGI(TAG, "Sensor contains %d templates", this->finger_->templateCount);
    
    this->connect_attempts_ = 0;  // Reset for future reconnects
    return true;
  }
  
  // If we're paired but verification failed, it could be a different sensor
  if (this->sensor_paired_) {
    ESP_LOGW(TAG, "Password verification failed - sensor may have been swapped!");
  }
  
  // Allow up to 10 attempts (5 seconds interval * 10 = 50 seconds total)
  if (this->connect_attempts_ >= 10) {
    ESP_LOGE(TAG, "Did not find fingerprint sensor after %d attempts", this->connect_attempts_);
    this->connect_attempts_ = 0;  // Reset for future retries
  }
  
  return false;
}

Match FingerprintDoorbell::scan_fingerprint() {
  Match match;
  match.scan_result = ScanResult::ERROR;

  if (!this->sensor_connected_)
    return match;

  // Check touch ring first (if not ignored)
  bool ring_touched = false;
  if (!this->ignore_touch_ring_) {
    if (this->is_ring_touched())
      ring_touched = true;
    
    if (ring_touched || this->last_touch_state_) {
      this->update_touch_state(true);
    } else {
      this->update_touch_state(false);
      match.scan_result = ScanResult::NO_FINGER;
      return match;
    }
  }

  // Multi-pass scanning (up to 5 attempts) - exactly like original
  bool do_another_scan = true;
  int scan_pass = 0;
  
  while (do_another_scan) {
    do_another_scan = false;
    scan_pass++;

    // STEP 1: Get Image
    bool do_imaging = true;
    int imaging_pass = 0;
    
    while (do_imaging) {
      do_imaging = false;
      imaging_pass++;
      
      match.return_code = this->finger_->getImage();
      
      switch (match.return_code) {
        case FINGERPRINT_OK:
          // Finger detected and image captured - show scanning LED
          this->set_led_ring_scanning();
          break;
          
        case FINGERPRINT_NOFINGER:
        case FINGERPRINT_PACKETRECIEVEERR:
          if (ring_touched) {
            this->update_touch_state(true);
            if (imaging_pass < 15) {
              do_imaging = true;
              break;
            } else {
              match.scan_result = ScanResult::NO_MATCH_FOUND;
              return match;
            }
          } else {
            if (this->ignore_touch_ring_ && scan_pass > 1) {
              match.scan_result = ScanResult::NO_MATCH_FOUND;
            } else {
              match.scan_result = ScanResult::NO_FINGER;
              this->update_touch_state(false);
            }
            return match;
          }
          
        case FINGERPRINT_IMAGEFAIL:
          ESP_LOGW(TAG, "Imaging error");
          this->update_touch_state(true);
          return match;
          
        default:
          ESP_LOGW(TAG, "Unknown error");
          return match;
      }
    }

    // STEP 2: Convert Image
    match.return_code = this->finger_->image2Tz();
    
    switch (match.return_code) {
      case FINGERPRINT_OK:
        this->update_touch_state(true);
        break;
      case FINGERPRINT_IMAGEMESS:
        ESP_LOGW(TAG, "Image too messy");
        return match;
      case FINGERPRINT_PACKETRECIEVEERR:
        ESP_LOGW(TAG, "Communication error");
        return match;
      case FINGERPRINT_FEATUREFAIL:
      case FINGERPRINT_INVALIDIMAGE:
        ESP_LOGW(TAG, "Could not find fingerprint features");
        return match;
      default:
        ESP_LOGW(TAG, "Unknown error");
        return match;
    }

    // STEP 3: Search DB
    match.return_code = this->finger_->fingerSearch();
    
    if (match.return_code == FINGERPRINT_OK) {
      // Match found - LED is set in loop() after scan returns
      match.scan_result = ScanResult::MATCH_FOUND;
      match.match_id = this->finger_->fingerID;
      match.match_confidence = this->finger_->confidence;
      
      auto it = this->fingerprint_names_.find(match.match_id);
      if (it != this->fingerprint_names_.end()) {
        match.match_name = it->second;
      } else {
        match.match_name = "unknown";
      }
      
    } else if (match.return_code == FINGERPRINT_PACKETRECIEVEERR) {
      ESP_LOGW(TAG, "Communication error");
      
    } else if (match.return_code == FINGERPRINT_NOTFOUND) {
      ESP_LOGD(TAG, "No match (Scan #%d of 5)", scan_pass);
      match.scan_result = ScanResult::NO_MATCH_FOUND;
      
      if (scan_pass < 5)
        do_another_scan = true;
      
    } else {
      ESP_LOGW(TAG, "Unknown error");
    }
  }

  return match;
}

// ==================== ENROLLMENT ====================

void FingerprintDoorbell::start_enrollment(uint16_t id, const std::string &name) {
  if (!this->sensor_connected_) {
    ESP_LOGW(TAG, "Cannot enroll: sensor not connected");
    this->publish_enroll_status("Error: Sensor not connected");
    this->publish_last_action("Enroll failed: no sensor");
    return;
  }
  
  if (id < 1 || id > 200) {
    ESP_LOGW(TAG, "Invalid ID %d (must be 1-200)", id);
    this->publish_enroll_status("Error: Invalid ID (1-200)");
    this->publish_last_action("Enroll failed: invalid ID");
    return;
  }
  
  ESP_LOGI(TAG, "Starting enrollment for ID %d, name: %s", id, name.c_str());
  
  this->mode_ = Mode::ENROLL;
  this->enroll_step_ = EnrollStep::WAITING_FOR_FINGER;
  this->enroll_id_ = id;
  this->enroll_name_ = name;
  this->enroll_sample_ = 1;
  this->enroll_timeout_ = millis() + 60000;  // 60 second timeout
  
  this->set_led_ring_enroll();
  this->publish_enroll_status("Place finger (1/5)");
  this->publish_last_action("Enrollment started for ID " + std::to_string(id));
}

void FingerprintDoorbell::cancel_enrollment() {
  if (this->mode_ != Mode::ENROLL) {
    return;
  }
  
  ESP_LOGI(TAG, "Enrollment cancelled");
  this->mode_ = Mode::SCAN;
  this->enroll_step_ = EnrollStep::IDLE;
  this->set_led_ring_ready();
  this->publish_enroll_status("Cancelled");
  this->publish_last_action("Enrollment cancelled");
}

void FingerprintDoorbell::process_enrollment() {
  // Check timeout
  if (millis() > this->enroll_timeout_) {
    ESP_LOGW(TAG, "Enrollment timeout");
    this->mode_ = Mode::SCAN;
    this->enroll_step_ = EnrollStep::IDLE;
    this->set_led_ring_ready();
    this->publish_enroll_status("Timeout");
    this->publish_last_action("Enrollment timeout");
    return;
  }

  uint8_t result;
  
  switch (this->enroll_step_) {
    case EnrollStep::WAITING_FOR_FINGER:
      result = this->finger_->getImage();
      if (result == FINGERPRINT_OK) {
        ESP_LOGI(TAG, "Image captured for sample %d", this->enroll_sample_);
        this->enroll_step_ = EnrollStep::CONVERTING;
      } else if (result != FINGERPRINT_NOFINGER) {
        ESP_LOGW(TAG, "Error capturing image: %d", result);
      }
      break;
      
    case EnrollStep::CONVERTING:
      result = this->finger_->image2Tz(this->enroll_sample_);
      if (result == FINGERPRINT_OK) {
        ESP_LOGI(TAG, "Image converted for sample %d", this->enroll_sample_);
        
        if (this->enroll_sample_ >= 5) {
          // All 5 samples collected, create model immediately
          // Don't send LED commands before createModel - it can corrupt buffer state
          this->publish_enroll_status("Creating model...");
          result = this->finger_->createModel();
          if (result == FINGERPRINT_OK) {
            ESP_LOGI(TAG, "Model created successfully");
            this->enroll_step_ = EnrollStep::STORING;
            this->publish_enroll_status("Storing...");
          } else if (result == FINGERPRINT_ENROLLMISMATCH) {
            ESP_LOGW(TAG, "Fingerprints did not match");
            this->mode_ = Mode::SCAN;
            this->enroll_step_ = EnrollStep::IDLE;
            this->set_led_ring_error();
            this->publish_enroll_status("Error: prints don't match");
            this->publish_last_action("Enrollment failed: mismatch");
            this->set_timeout(2000, [this]() { this->set_led_ring_ready(); });
          } else {
            ESP_LOGW(TAG, "Error creating model: %d", result);
            this->mode_ = Mode::SCAN;
            this->enroll_step_ = EnrollStep::IDLE;
            this->set_led_ring_error();
            this->publish_enroll_status("Error creating model");
            this->publish_last_action("Enrollment failed");
            this->set_timeout(2000, [this]() { this->set_led_ring_ready(); });
          }
        } else {
          // Need more samples - show LED feedback for successful scan
          this->set_led_ring_match();
          this->enroll_step_ = EnrollStep::WAITING_REMOVE;
          this->publish_enroll_status("Remove finger");
        }
      } else {
        ESP_LOGW(TAG, "Error converting image: %d", result);
        this->publish_enroll_status("Error, try again");
        this->enroll_step_ = EnrollStep::WAITING_FOR_FINGER;
        this->set_led_ring_enroll();
      }
      break;
      
    case EnrollStep::WAITING_REMOVE:
      result = this->finger_->getImage();
      if (result == FINGERPRINT_NOFINGER) {
        this->enroll_sample_++;
        ESP_LOGI(TAG, "Ready for sample %d", this->enroll_sample_);
        this->enroll_step_ = EnrollStep::WAITING_FOR_FINGER;
        this->set_led_ring_enroll();
        this->publish_enroll_status("Place finger (" + std::to_string(this->enroll_sample_) + "/5)");
      }
      break;
      
    case EnrollStep::STORING:
      result = this->finger_->storeModel(this->enroll_id_);
      if (result == FINGERPRINT_OK) {
        ESP_LOGI(TAG, "Fingerprint stored at ID %d", this->enroll_id_);
        this->save_fingerprint_name(this->enroll_id_, this->enroll_name_);
        this->finger_->getTemplateCount();
        
        // Show success LED and wait for finger to be removed
        this->enroll_step_ = EnrollStep::DONE;
        this->set_led_ring_match();  // Purple glow for success
        this->publish_enroll_status("Success! Remove finger");
        this->publish_last_action("Enrolled: " + this->enroll_name_ + " (ID " + std::to_string(this->enroll_id_) + ")");
      } else {
        ESP_LOGW(TAG, "Error storing model: %d", result);
        this->mode_ = Mode::SCAN;
        this->enroll_step_ = EnrollStep::IDLE;
        this->set_led_ring_error();
        this->publish_enroll_status("Error storing");
        this->publish_last_action("Enrollment failed: storage error");
        this->set_timeout(2000, [this]() { this->set_led_ring_ready(); });
      }
      break;
    
    case EnrollStep::DONE:
      // Wait for finger to be removed before returning to scan mode
      result = this->finger_->getImage();
      if (result == FINGERPRINT_NOFINGER) {
        ESP_LOGI(TAG, "Enrollment complete, finger removed");
        this->mode_ = Mode::SCAN;
        this->enroll_step_ = EnrollStep::IDLE;
        this->set_led_ring_ready();
        this->publish_enroll_status("Complete");
      }
      break;
      
    default:
      break;
  }
}

// ==================== DELETE / RENAME ====================

bool FingerprintDoorbell::delete_fingerprint(uint16_t id) {
  if (!this->sensor_connected_ || this->finger_ == nullptr) {
    this->publish_last_action("Delete failed: no sensor");
    return false;
  }
  
  if (this->finger_->deleteModel(id) == FINGERPRINT_OK) {
    std::string name = this->get_fingerprint_name(id);
    this->delete_fingerprint_name(id);
    this->finger_->getTemplateCount();
    ESP_LOGI(TAG, "Deleted fingerprint ID %d", id);
    this->publish_last_action("Deleted: " + name + " (ID " + std::to_string(id) + ")");
    return true;
  }
  this->publish_last_action("Delete failed for ID " + std::to_string(id));
  return false;
}

bool FingerprintDoorbell::delete_all_fingerprints() {
  if (!this->sensor_connected_ || this->finger_ == nullptr) {
    this->publish_last_action("Delete all failed: no sensor");
    return false;
  }
  
  if (this->finger_->emptyDatabase() == FINGERPRINT_OK) {
    // Collect IDs first to avoid modifying map while iterating
    std::vector<uint16_t> ids_to_delete;
    for (auto const& pair : this->fingerprint_names_) {
      ids_to_delete.push_back(pair.first);
    }
    // Now delete each name from preferences
    for (uint16_t id : ids_to_delete) {
      this->delete_fingerprint_name(id);
    }
    this->fingerprint_names_.clear();
    this->finger_->getTemplateCount();
    ESP_LOGI(TAG, "Deleted all fingerprints");
    this->publish_last_action("Deleted all fingerprints");
    return true;
  }
  this->publish_last_action("Delete all failed");
  return false;
}

bool FingerprintDoorbell::rename_fingerprint(uint16_t id, const std::string &new_name) {
  std::string old_name = this->get_fingerprint_name(id);
  this->save_fingerprint_name(id, new_name);
  ESP_LOGI(TAG, "Renamed ID %d from '%s' to '%s'", id, old_name.c_str(), new_name.c_str());
  this->publish_last_action("Renamed ID " + std::to_string(id) + " to " + new_name);
  return true;
}

// ==================== TEMPLATE TRANSFER ====================

// Define FINGERPRINT_DOWNLOAD command (not in Arduino library)
#define FINGERPRINT_DOWNLOAD 0x09

bool FingerprintDoorbell::get_template(uint16_t id, std::vector<uint8_t> &template_data) {
  if (!this->sensor_connected_ || this->finger_ == nullptr) {
    ESP_LOGW(TAG, "Cannot get template: sensor not connected");
    return false;
  }
  
  // Temporarily pause scanning to avoid conflicts
  Mode previous_mode = this->mode_;
  this->mode_ = Mode::IDLE;
  delay(200);  // Give sensor time to settle
  
  // Flush any leftover data in serial buffer
  while (mySerial.available()) {
    mySerial.read();
  }
  
  // Load template from flash into character buffer
  uint8_t result = this->finger_->loadModel(id);
  if (result != FINGERPRINT_OK) {
    ESP_LOGW(TAG, "Failed to load template %d: error %d", id, result);
    this->mode_ = previous_mode;
    return false;
  }
  
  ESP_LOGI(TAG, "Template %d loaded, requesting data transfer...", id);
  
  // Request template data transfer using UPCHAR command (getModel)
  result = this->finger_->getModel();
  if (result != FINGERPRINT_OK) {
    ESP_LOGW(TAG, "Failed to start template transfer: error %d", result);
    this->mode_ = previous_mode;
    return false;
  }
  
  // R503 template = 1536 bytes, sent in packets
  // With 128-byte packet data: 1536 / 128 = 12 packets
  // Each packet: 2 (start) + 4 (addr) + 1 (type) + 2 (len) + 128 (data) + 2 (checksum) = 139 bytes
  const int PACKET_DATA_SIZE = 128;
  const int PACKET_OVERHEAD = 11;  // 2+4+1+2+2
  const int MAX_TEMPLATE_SIZE = 1536;
  const int MAX_PACKETS = MAX_TEMPLATE_SIZE / PACKET_DATA_SIZE;  // 12
  
  template_data.clear();
  template_data.reserve(MAX_TEMPLATE_SIZE);
  
  ESP_LOGI(TAG, "Reading template data (expecting up to %d packets)...", MAX_PACKETS);
  
  // Read packets by parsing raw serial data
  uint32_t start_time = millis();
  const uint32_t TIMEOUT_MS = 10000;
  int packets_read = 0;
  bool end_received = false;
  
  while (!end_received && packets_read < MAX_PACKETS && (millis() - start_time < TIMEOUT_MS)) {
    // Wait for packet start code 0xEF01
    bool found_start = false;
    uint32_t pkt_start_time = millis();
    
    while (!found_start && (millis() - pkt_start_time < 2000)) {
      if (mySerial.available() >= 2) {
        uint8_t b1 = mySerial.read();
        if (b1 == 0xEF) {
          uint8_t b2 = mySerial.peek();
          if (b2 == 0x01) {
            mySerial.read();  // consume 0x01
            found_start = true;
          }
        }
      }
    }
    
    if (!found_start) {
      ESP_LOGW(TAG, "Timeout waiting for packet %d start code", packets_read + 1);
      break;
    }
    
    // Read rest of header: 4 (addr) + 1 (type) + 2 (len) = 7 bytes
    uint8_t header[7];
    int header_read = 0;
    uint32_t header_start = millis();
    
    while (header_read < 7 && (millis() - header_start < 1000)) {
      if (mySerial.available()) {
        header[header_read++] = mySerial.read();
      }
    }
    
    if (header_read < 7) {
      ESP_LOGW(TAG, "Timeout reading packet %d header (got %d bytes)", packets_read + 1, header_read);
      break;
    }
    
    uint8_t pkt_type = header[4];
    uint16_t pkt_len = (header[5] << 8) | header[6];  // includes 2-byte checksum
    uint16_t data_len = (pkt_len > 2) ? (pkt_len - 2) : 0;
    
    if (data_len > 256) {
      ESP_LOGW(TAG, "Invalid packet %d data length: %d", packets_read + 1, data_len);
      break;
    }
    
    // Read data + checksum
    uint8_t payload[258];  // max 256 data + 2 checksum
    int payload_read = 0;
    uint32_t payload_start = millis();
    
    while (payload_read < pkt_len && (millis() - payload_start < 1000)) {
      if (mySerial.available()) {
        payload[payload_read++] = mySerial.read();
      }
    }
    
    if (payload_read < pkt_len) {
      ESP_LOGW(TAG, "Timeout reading packet %d payload (got %d/%d bytes)", 
               packets_read + 1, payload_read, pkt_len);
      break;
    }
    
    packets_read++;
    
    // Extract data bytes (exclude checksum)
    for (uint16_t i = 0; i < data_len; i++) {
      template_data.push_back(payload[i]);
    }
    
    ESP_LOGD(TAG, "Packet %d: type=0x%02X, data_len=%d, total=%d bytes", 
             packets_read, pkt_type, data_len, (int)template_data.size());
    
    // Check for end packet
    if (pkt_type == FINGERPRINT_ENDDATAPACKET) {
      end_received = true;
      ESP_LOGD(TAG, "Received end packet");
    }
  }
  
  ESP_LOGI(TAG, "Downloaded template %d: %d bytes in %d packets", 
           id, (int)template_data.size(), packets_read);
  
  if (template_data.size() < 512) {
    ESP_LOGW(TAG, "Template too small (%d bytes), expected at least 512", (int)template_data.size());
    this->mode_ = previous_mode;
    return false;
  }
  
  this->mode_ = previous_mode;
  return true;
}

bool FingerprintDoorbell::upload_template(uint16_t id, const std::string &name, const std::vector<uint8_t> &template_data) {
  if (!this->sensor_connected_ || this->finger_ == nullptr) {
    ESP_LOGW(TAG, "Cannot upload template: sensor not connected");
    return false;
  }
  
  // R503 templates are 1536 bytes, but we also accept 512 bytes (feature file) for compatibility
  if (template_data.size() != 512 && template_data.size() != 1536) {
    ESP_LOGW(TAG, "Invalid template size: %d bytes (expected 512 or 1536)", (int)template_data.size());
    return false;
  }
  
  // Temporarily pause scanning to avoid conflicts
  Mode previous_mode = this->mode_;
  this->mode_ = Mode::IDLE;
  delay(200);  // Give sensor time to settle
  
  // Flush any leftover data in serial buffer
  while (mySerial.available()) {
    mySerial.read();
  }
  
  // Use sensor's configured packet length
  uint16_t packet_len = this->finger_->packet_len;
  if (packet_len == 0 || packet_len > 256) {
    packet_len = 128;  // Safe default
  }
  
  ESP_LOGI(TAG, "Uploading template to ID %d (%d bytes, %d-byte packets)", 
           id, (int)template_data.size(), packet_len);
  

  
  // Send DownChar command to start receiving template into buffer 1
  uint8_t cmd_data[] = {FINGERPRINT_DOWNLOAD, 0x01};  // Buffer 1
  Adafruit_Fingerprint_Packet cmd_packet(FINGERPRINT_COMMANDPACKET, sizeof(cmd_data), cmd_data);
  this->finger_->writeStructuredPacket(cmd_packet);
  
  // Wait for acknowledgment
  Adafruit_Fingerprint_Packet ack_packet(FINGERPRINT_ACKPACKET, 0, nullptr);
  if (this->finger_->getStructuredPacket(&ack_packet, 2000) != FINGERPRINT_OK) {
    ESP_LOGW(TAG, "No acknowledgment for DOWNCHAR command");
    this->mode_ = previous_mode;
    return false;
  }
  
  if (ack_packet.data[0] != FINGERPRINT_OK) {
    ESP_LOGW(TAG, "DOWNCHAR failed: 0x%02X", ack_packet.data[0]);
    this->mode_ = previous_mode;
    return false;
  }
  
  ESP_LOGD(TAG, "Sensor ready to receive template data");
  delay(10);
  while (mySerial.available()) mySerial.read();
  
  // Send template data in packets
  const uint32_t addr = 0xFFFFFFFF;
  size_t total_size = template_data.size();
  size_t written = 0;
  int pkt_num = 0;
  
  while (written < total_size) {
    size_t remaining = total_size - written;
    size_t chunk_size = (remaining > packet_len) ? packet_len : remaining;
    bool is_last = (remaining <= packet_len);
    pkt_num++;
    
    // Packet type: 0x02 for data, 0x08 for final packet
    uint8_t pkt_type = is_last ? FINGERPRINT_ENDDATAPACKET : FINGERPRINT_DATAPACKET;
    
    // Length field: data bytes + 2 checksum bytes
    uint16_t total_len = chunk_size + 2;
    
    // Calculate checksum
    uint16_t checksum = (total_len >> 8) + (total_len & 0xFF) + pkt_type;
    for (size_t i = 0; i < chunk_size; i++) {
      checksum += template_data[written + i];
    }
    
    // Write packet header byte-by-byte
    mySerial.write((uint8_t)(0xEF01 >> 8));
    mySerial.write((uint8_t)(0xEF01 & 0xFF));
    mySerial.write((uint8_t)(addr >> 24));
    mySerial.write((uint8_t)(addr >> 16));
    mySerial.write((uint8_t)(addr >> 8));
    mySerial.write((uint8_t)(addr & 0xFF));
    mySerial.write(pkt_type);
    mySerial.write((uint8_t)(total_len >> 8));
    mySerial.write((uint8_t)(total_len & 0xFF));
    
    // Write payload data
    mySerial.write(template_data.data() + written, chunk_size);
    
    // Write checksum
    mySerial.write((uint8_t)(checksum >> 8));
    mySerial.write((uint8_t)(checksum & 0xFF));
    
    ESP_LOGD(TAG, "PKT%d: type=0x%02X, len=%d, total_written=%d", 
             pkt_num, pkt_type, (int)chunk_size, (int)(written + chunk_size));
    
    written += chunk_size;
    delay(1);  // Small yield between packets
  }
  
  ESP_LOGI(TAG, "Sent all %d packets (%d bytes total)", pkt_num, (int)written);
  
  delay(100);
  while (mySerial.available()) mySerial.read();
  
  // Delete any existing template at this ID
  this->finger_->deleteModel(id);

  delay(100);
  while (mySerial.available()) mySerial.read();
  
  // Store the template to flash
  uint8_t result = this->finger_->storeModel(id);

  
  if (result != FINGERPRINT_OK) {
    const char* error_desc = "unknown";
    switch (result) {
      case 0x01: error_desc = "PACKETRECIEVEERR"; break;
      case 0x0B: error_desc = "BADLOCATION"; break;
      case 0x0C: error_desc = "DBRANGEFAIL/invalid_template"; break;
      case 0x0D: error_desc = "UPLOADFEATUREFAIL"; break;
      case 0x0E: error_desc = "PACKETRESPONSEFAIL"; break;
      case 0x18: error_desc = "FLASHERR"; break;
    }
    ESP_LOGW(TAG, "Failed to store template at ID %d: error 0x%02X (%s)", id, result, error_desc);
    this->mode_ = previous_mode;
    return false;
  }
  
  // Save the name
  this->save_fingerprint_name(id, name);
  this->finger_->getTemplateCount();
  
  ESP_LOGI(TAG, "Template uploaded and stored at ID %d with name '%s'", id, name.c_str());
  this->publish_last_action("Imported: " + name + " (ID " + std::to_string(id) + ")");
  
  this->mode_ = previous_mode;
  return true;
}

// ==================== UTILITIES ====================

uint16_t FingerprintDoorbell::get_enrolled_count() {
  return this->finger_ != nullptr ? this->finger_->templateCount : 0;
}

std::string FingerprintDoorbell::get_fingerprint_name(uint16_t id) {
  auto it = this->fingerprint_names_.find(id);
  return (it != this->fingerprint_names_.end()) ? it->second : "unknown";
}

std::string FingerprintDoorbell::get_fingerprint_list_json() {
  std::string json = "[";
  bool first = true;
  
  if (this->finger_ == nullptr || !this->sensor_connected_) {
    return "[]";
  }
  
  // Get list of enrolled IDs from sensor using ReadIndexTable command (0x1F)
  // This is much faster than checking each slot individually
  std::vector<uint16_t> enrolled_ids;
  
  // ReadIndexTable command - page 0 covers slots 0-255 (enough for most sensors)
  uint8_t packet[] = {
    0xEF, 0x01,             // Header
    0xFF, 0xFF, 0xFF, 0xFF, // Address
    0x01,                   // Command packet
    0x00, 0x04,             // Length (3 bytes + checksum)
    0x1F,                   // ReadIndexTable command
    0x00,                   // Page 0 (slots 0-255)
    0x00, 0x24              // Checksum: 0x01 + 0x00 + 0x04 + 0x1F + 0x00 = 0x24
  };
  
  // Clear buffer and send command
  while (this->hw_serial_->available()) this->hw_serial_->read();
  this->hw_serial_->write(packet, sizeof(packet));
  this->hw_serial_->flush();
  
  // Wait for response (12 byte header + 32 byte index table = 44 bytes minimum)
  uint32_t start = millis();
  while (this->hw_serial_->available() < 44 && millis() - start < 1000) {
    delay(10);
  }
  
  // Read response
  uint8_t response[64];
  int count = 0;
  while (this->hw_serial_->available() && count < 64) {
    response[count++] = this->hw_serial_->read();
  }
  
  // Validate response: header, packet type 0x07 (ACK), and status 0x00 (OK)
  if (count >= 44 && response[0] == 0xEF && response[1] == 0x01 && 
      response[6] == 0x07 && response[9] == 0x00) {
    // Parse index table (32 bytes starting at response[10])
    // Each bit represents a slot: 1 = occupied, 0 = empty
    for (int byteIdx = 0; byteIdx < 32; byteIdx++) {
      uint8_t indexByte = response[10 + byteIdx];
      for (int bitIdx = 0; bitIdx < 8; bitIdx++) {
        if (indexByte & (1 << bitIdx)) {
          uint16_t id = (byteIdx * 8) + bitIdx;
          enrolled_ids.push_back(id);
        }
      }
    }
    ESP_LOGD(TAG, "Found %d enrolled fingerprints on sensor", enrolled_ids.size());
  } else {
    ESP_LOGW(TAG, "Failed to read index table (count=%d, status=0x%02X), falling back to cached names", 
             count, count >= 10 ? response[9] : 0xFF);
    // Fallback: use cached names only
    for (const auto& pair : this->fingerprint_names_) {
      if (!first) json += ",";
      json += "{\"id\":" + std::to_string(pair.first) + ",\"name\":\"" + pair.second + "\"}";
      first = false;
    }
    json += "]";
    return json;
  }
  
  // Build JSON with all enrolled IDs, using cached name or ID as fallback
  for (uint16_t id : enrolled_ids) {
    if (!first) json += ",";
    
    auto it = this->fingerprint_names_.find(id);
    if (it != this->fingerprint_names_.end() && !it->second.empty()) {
      // Use stored name
      json += "{\"id\":" + std::to_string(id) + ",\"name\":\"" + it->second + "\"}";
    } else {
      // No name stored, use ID as name
      json += "{\"id\":" + std::to_string(id) + ",\"name\":\"#" + std::to_string(id) + "\"}";
    }
    first = false;
  }
  
  json += "]";
  return json;
}

void FingerprintDoorbell::update_touch_state(bool touched) {
  if ((touched != this->last_touch_state_) || (this->ignore_touch_ring_ != this->last_ignore_touch_ring_)) {
    if (touched) {
      this->set_led_ring_scanning();
    } else {
      this->set_led_ring_ready();
    }
  }
  this->last_touch_state_ = touched;
  this->last_ignore_touch_ring_ = this->ignore_touch_ring_;
}

bool FingerprintDoorbell::is_ring_touched() {
  if (this->touch_pin_ == nullptr)
    return false;
  // LOW = touched (capacitive sensor)
  return !this->touch_pin_->digital_read();
}

void FingerprintDoorbell::set_led_ring_ready() {
  if (this->finger_ == nullptr || !this->sensor_connected_)
    return;
  
  if (this->ignore_touch_ring_) {
    // When touch ring is ignored, use solid "on" mode instead of breathing
    this->finger_->LEDcontrol(FINGERPRINT_LED_ON, 0, this->led_ready_.color, 0);
  } else {
    this->finger_->LEDcontrol(this->led_ready_.mode, this->led_ready_.speed, this->led_ready_.color, 0);
  }
}

void FingerprintDoorbell::set_led_ring_error() {
  if (this->finger_ != nullptr)
    this->finger_->LEDcontrol(this->led_error_.mode, this->led_error_.speed, this->led_error_.color, 0);
}

void FingerprintDoorbell::set_led_ring_enroll() {
  if (this->finger_ != nullptr)
    this->finger_->LEDcontrol(this->led_enroll_.mode, this->led_enroll_.speed, this->led_enroll_.color, 0);
}

void FingerprintDoorbell::set_led_ring_match() {
  ESP_LOGD(TAG, "LED match: color=%d, mode=%d, speed=%d", this->led_match_.color, this->led_match_.mode, this->led_match_.speed);
  if (this->finger_ != nullptr)
    this->finger_->LEDcontrol(this->led_match_.mode, this->led_match_.speed, this->led_match_.color, 0);
}

void FingerprintDoorbell::set_led_ring_scanning() {
  ESP_LOGD(TAG, "LED scanning: color=%d, mode=%d, speed=%d", this->led_scanning_.color, this->led_scanning_.mode, this->led_scanning_.speed);
  if (this->finger_ != nullptr)
    this->finger_->LEDcontrol(this->led_scanning_.mode, this->led_scanning_.speed, this->led_scanning_.color, 0);
}

void FingerprintDoorbell::set_led_ring_no_match() {
  ESP_LOGD(TAG, "LED no_match: color=%d, mode=%d, speed=%d", this->led_no_match_.color, this->led_no_match_.mode, this->led_no_match_.speed);
  if (this->finger_ != nullptr)
    this->finger_->LEDcontrol(this->led_no_match_.mode, this->led_no_match_.speed, this->led_no_match_.color, 0);
}

void FingerprintDoorbell::load_fingerprint_names() {
  this->fingerprint_names_.clear();
  
  for (uint16_t i = 1; i <= 200; i++) {
    std::string key = "fp_" + std::to_string(i);
    ESPPreferenceObject pref = global_preferences->make_preference<std::array<char, 32>>(fnv1_hash(key.c_str()));
    
    std::array<char, 32> name_array;
    if (pref.load(&name_array)) {
      std::string name(name_array.data());
      if (!name.empty() && name[0] != '\0' && name != "@empty") {
        this->fingerprint_names_[i] = name;
      }
    }
  }
  
  ESP_LOGI(TAG, "%d fingerprint names loaded", this->fingerprint_names_.size());
}

void FingerprintDoorbell::save_fingerprint_name(uint16_t id, const std::string &name) {
  std::string key = "fp_" + std::to_string(id);
  ESPPreferenceObject pref = global_preferences->make_preference<std::array<char, 32>>(fnv1_hash(key.c_str()));
  
  std::array<char, 32> name_array = {};
  strncpy(name_array.data(), name.c_str(), 31);
  name_array[31] = '\0';
  pref.save(&name_array);
  
  this->fingerprint_names_[id] = name;
}

void FingerprintDoorbell::delete_fingerprint_name(uint16_t id) {
  std::string key = "fp_" + std::to_string(id);
  ESPPreferenceObject pref = global_preferences->make_preference<std::array<char, 32>>(fnv1_hash(key.c_str()));
  
  std::array<char, 32> empty_array = {};
  pref.save(&empty_array);
  
  this->fingerprint_names_.erase(id);
}

void FingerprintDoorbell::publish_enroll_status(const std::string &status) {
  ESP_LOGI(TAG, "Enroll status: %s", status.c_str());
  if (this->enroll_status_sensor_ != nullptr) {
    this->enroll_status_sensor_->publish_state(status);
  }
}

void FingerprintDoorbell::publish_last_action(const std::string &action) {
  ESP_LOGI(TAG, "Action: %s", action.c_str());
  if (this->last_action_sensor_ != nullptr) {
    this->last_action_sensor_->publish_state(action);
  }
}

// ==================== SENSOR PAIRING ====================

void FingerprintDoorbell::load_sensor_password() {
  ESPPreferenceObject pref = global_preferences->make_preference<uint32_t>(fnv1_hash("sensor_pwd"));
  uint32_t stored_password = 0;
  if (pref.load(&stored_password)) {
    // Check for special "unpaired" marker (all 1s is unlikely as a real password)
    if (stored_password != 0xFFFFFFFF) {
      this->sensor_password_ = stored_password;
      this->sensor_paired_ = true;
      ESP_LOGI(TAG, "Loaded sensor password from preferences");
    } else {
      this->sensor_paired_ = false;
      ESP_LOGI(TAG, "Sensor is unpaired (marker found)");
    }
  } else {
    // No password stored - sensor is unpaired
    this->sensor_paired_ = false;
    ESP_LOGI(TAG, "No sensor password in preferences - unpaired");
  }
}

void FingerprintDoorbell::save_sensor_password() {
  ESPPreferenceObject pref = global_preferences->make_preference<uint32_t>(fnv1_hash("sensor_pwd"));
  pref.save(&this->sensor_password_);
  ESP_LOGI(TAG, "Saved sensor password to preferences");
}

bool FingerprintDoorbell::reset_sensor_to_default() {
  ESP_LOGI(TAG, "Resetting sensor to default password...");
  
  // CRITICAL: Reinitialize serial to ensure clean state
  // This is necessary because the Adafruit library can corrupt UART communication
  mySerial.end();
  delay(50);
  mySerial.begin(57600, SERIAL_8N1, 16, 17);
  delay(100);
  
  // Clear any garbage in the serial buffer
  while (this->hw_serial_->available()) this->hw_serial_->read();
  
  // Build list of passwords to try - include common passwords
  std::vector<uint32_t> passwords_to_try;
  if (this->sensor_password_ != 0xFFFFFFFF && this->sensor_password_ != 0x00000000) {
    passwords_to_try.push_back(this->sensor_password_);  // Stored password first
  }
  passwords_to_try.push_back(0x00000000);  // Default password
  passwords_to_try.push_back(0x12345678);  // Common password
  passwords_to_try.push_back(0xFFFFFFFF);  // Another common one
  
  // Try to connect with one of the passwords using raw protocol
  // (more reliable than the library which has issues after begin())
  for (uint32_t try_pw : passwords_to_try) {
    ESP_LOGD(TAG, "Trying password 0x%08X...", try_pw);
    
    if (this->raw_verify_and_reset_password(try_pw)) {
      // Success! Sensor is now at default password
      if (this->finger_ != nullptr) {
        delete this->finger_;
      }
      this->finger_ = new Adafruit_Fingerprint(this->hw_serial_, 0x00000000);
      this->finger_->begin(57600);
      delay(100);
      
      ESP_LOGI(TAG, "Sensor reset to default password (was 0x%08X)", try_pw);
      return true;
    }
  }
  
  ESP_LOGW(TAG, "Cannot connect to sensor with any known password");
  return false;
}

bool FingerprintDoorbell::pair_sensor(uint32_t password) {
  ESP_LOGI(TAG, "Pairing sensor with password 0x%08X...", password);
  
  // Step 1: Try to reset sensor to default password first
  // This ensures we start from a known state
  bool reset_ok = this->reset_sensor_to_default();
  
  if (!reset_ok) {
    ESP_LOGW(TAG, "Could not reset sensor to default password");
    this->publish_last_action("Pairing failed - cannot reset sensor");
    return false;
  }
  
  // Step 2: Now sensor is at default password, set the new password
  // Try library first, then raw protocol as fallback
  delay(50);
  uint8_t result = this->finger_->setPassword(password);
  if (result != FINGERPRINT_OK) {
    ESP_LOGW(TAG, "Library setPassword failed: error %d, trying raw protocol...", result);
    
    if (!this->raw_set_password(password)) {
      ESP_LOGE(TAG, "Raw setPassword also failed");
      this->publish_last_action("Pairing failed - setPassword error");
      return false;
    }
  }
  
  // Step 3: Update state and recreate finger object
  this->sensor_password_ = password;
  this->sensor_paired_ = true;
  this->sensor_connected_ = true;
  this->save_sensor_password();
  
  delete this->finger_;
  this->finger_ = new Adafruit_Fingerprint(this->hw_serial_, password);
  this->finger_->begin(57600);
  
  ESP_LOGI(TAG, "Sensor paired successfully");
  this->publish_last_action("Sensor paired");
  return true;
}

bool FingerprintDoorbell::unpair_sensor() {
  ESP_LOGI(TAG, "Unpairing sensor (resetting to default password)...");
  
  // Use reset_sensor_to_default which has all the recovery logic
  // (tries stored password, raw protocol fallback, etc.)
  bool reset_ok = this->reset_sensor_to_default();
  
  if (!reset_ok) {
    ESP_LOGW(TAG, "Failed to reset sensor to default password");
    this->publish_last_action("Unpair failed - cannot reset sensor");
    return false;
  }
  
  // Update our state
  this->sensor_password_ = 0xFFFFFFFF;  // Marker for "unpaired"
  this->sensor_paired_ = false;
  this->sensor_connected_ = true;  // We're now connected with default password
  this->save_sensor_password();
  
  ESP_LOGI(TAG, "Sensor unpaired successfully");
  this->publish_last_action("Sensor unpaired");
  return true;
}

bool FingerprintDoorbell::factory_reset_sensor(uint32_t old_password) {
  ESP_LOGI(TAG, "Factory resetting sensor...");
  
  // Build list of passwords to try
  std::vector<uint32_t> passwords_to_try;
  if (old_password != 0xFFFFFFFF) {
    passwords_to_try.push_back(old_password);  // User-provided password first
  }
  if (this->sensor_password_ != 0xFFFFFFFF) {
    passwords_to_try.push_back(this->sensor_password_);  // Stored password
  }
  passwords_to_try.push_back(0x00000000);  // Default password last
  
  // Try to connect with one of the passwords using library
  bool connected = false;
  uint8_t result;
  uint32_t connected_password = 0;
  
  for (uint32_t try_pw : passwords_to_try) {
    if (this->finger_ != nullptr) {
      delete this->finger_;
    }
    this->finger_ = new Adafruit_Fingerprint(this->hw_serial_, try_pw);
    this->finger_->begin(57600);
    delay(100);
    
    result = this->finger_->verifyPassword();
    if (result == FINGERPRINT_OK) {
      // Verify with getParameters
      result = this->finger_->getParameters();
      if (result == FINGERPRINT_OK) {
        ESP_LOGI(TAG, "Connected with password 0x%08X", try_pw);
        connected = true;
        connected_password = try_pw;
        break;
      }
    }
  }
  
  // If library connection failed, try raw protocol with user-provided password
  if (!connected && old_password != 0xFFFFFFFF) {
    ESP_LOGI(TAG, "Library connection failed, trying raw protocol with provided password...");
    connected = this->raw_verify_and_reset_password(old_password);
    if (connected) {
      // Raw reset succeeded - sensor is now at default password
      connected_password = 0x00000000;
      
      // Recreate finger object with default password
      if (this->finger_ != nullptr) {
        delete this->finger_;
      }
      this->finger_ = new Adafruit_Fingerprint(this->hw_serial_, 0x00000000);
      this->finger_->begin(57600);
      delay(100);
    }
  }
  
  if (!connected) {
    ESP_LOGW(TAG, "Cannot connect to sensor with any known password");
    this->publish_last_action("Factory reset failed - cannot connect");
    return false;
  }
  
  // Delete all fingerprints from sensor
  delay(50);
  result = this->finger_->emptyDatabase();
  if (result != FINGERPRINT_OK) {
    ESP_LOGW(TAG, "Failed to empty fingerprint database: error %d", result);
    // Continue anyway
  } else {
    ESP_LOGI(TAG, "Fingerprint database cleared");
  }
  
  // Reset password to default if not already
  if (connected_password != 0x00000000) {
    delay(50);
    result = this->finger_->setPassword(0x00000000);
    if (result != FINGERPRINT_OK) {
      ESP_LOGW(TAG, "Failed to reset sensor password: error %d", result);
      this->publish_last_action("Factory reset failed - password reset error");
      return false;
    }
    ESP_LOGI(TAG, "Sensor password reset to default");
    
    // Recreate finger object with default password
    delete this->finger_;
    this->finger_ = new Adafruit_Fingerprint(this->hw_serial_, 0x00000000);
    this->finger_->begin(57600);
  }
  
  // Update our state
  this->sensor_password_ = 0xFFFFFFFF;
  this->sensor_paired_ = false;
  this->sensor_connected_ = true;
  this->save_sensor_password();
  
  // Clear stored fingerprint names
  for (uint16_t i = 1; i <= 200; i++) {
    std::string key = "fp_" + std::to_string(i);
    ESPPreferenceObject pref = global_preferences->make_preference<std::array<char, 32>>(fnv1_hash(key.c_str()));
    std::array<char, 32> empty = {0};
    pref.save(&empty);
  }
  this->fingerprint_names_.clear();
  
  ESP_LOGI(TAG, "Factory reset complete");
  this->publish_last_action("Factory reset complete");
  return true;
}

// Raw protocol helper for factory reset when password is unknown to library
bool FingerprintDoorbell::raw_verify_and_reset_password(uint32_t old_password) {
  ESP_LOGI(TAG, "Attempting raw protocol reset with password 0x%08X", old_password);
  
  // Clear serial buffer thoroughly
  delay(100);
  while (this->hw_serial_->available()) this->hw_serial_->read();
  delay(50);
  
  // Build verifyPassword packet (command 0x13)
  uint8_t p0 = (old_password >> 24) & 0xFF;
  uint8_t p1 = (old_password >> 16) & 0xFF;
  uint8_t p2 = (old_password >> 8) & 0xFF;
  uint8_t p3 = old_password & 0xFF;
  uint16_t checksum = 0x01 + 0x00 + 0x07 + 0x13 + p0 + p1 + p2 + p3;
  
  uint8_t verify_packet[] = {
    0xEF, 0x01,             // Header
    0xFF, 0xFF, 0xFF, 0xFF, // Address
    0x01,                   // Command packet
    0x00, 0x07,             // Length
    0x13,                   // verifyPassword command
    p0, p1, p2, p3,         // Password
    (uint8_t)(checksum >> 8), (uint8_t)(checksum & 0xFF)
  };
  
  this->hw_serial_->write(verify_packet, sizeof(verify_packet));
  this->hw_serial_->flush();
  
  // Wait for response with timeout
  uint32_t start = millis();
  while (this->hw_serial_->available() < 12 && millis() - start < 500) {
    delay(10);
  }
  
  // Read response
  uint8_t response[20];
  int count = 0;
  while (this->hw_serial_->available() && count < 20) {
    response[count++] = this->hw_serial_->read();
  }
  
  // Log full response for debugging
  if (count > 0) {
    char hex[64];
    int pos = 0;
    for (int i = 0; i < count && pos < 60; i++) {
      pos += snprintf(hex + pos, 64 - pos, "%02X ", response[i]);
    }
    ESP_LOGD(TAG, "Raw verify response (%d bytes): %s", count, hex);
  }
  
  // Check response: header EF 01, then address, then 0x07 (ack), length, confirmation code
  if (count < 12) {
    ESP_LOGW(TAG, "Raw verifyPassword: insufficient response (%d bytes)", count);
    return false;
  }
  
  // Verify header
  if (response[0] != 0xEF || response[1] != 0x01) {
    ESP_LOGW(TAG, "Raw verifyPassword: invalid header");
    return false;
  }
  
  // Byte 6 should be 0x07 (ACK packet)
  if (response[6] != 0x07) {
    ESP_LOGW(TAG, "Raw verifyPassword: not an ACK packet (got 0x%02X)", response[6]);
    return false;
  }
  
  // Byte 9 is confirmation code (0x00 = OK)
  if (response[9] != 0x00) {
    ESP_LOGW(TAG, "Raw verifyPassword failed: confirmation code 0x%02X", response[9]);
    return false;
  }
  ESP_LOGI(TAG, "Raw verifyPassword OK");
  
  // Now send setPassword to reset to 0x00000000
  delay(50);
  while (this->hw_serial_->available()) this->hw_serial_->read();
  
  // Checksum for setPassword: 0x01 + 0x00 + 0x07 + 0x12 + 0 + 0 + 0 + 0 = 0x1A
  uint8_t set_pass_packet[] = {
    0xEF, 0x01,             // Header
    0xFF, 0xFF, 0xFF, 0xFF, // Address
    0x01,                   // Command packet
    0x00, 0x07,             // Length
    0x12,                   // setPassword command
    0x00, 0x00, 0x00, 0x00, // New password (0x00000000)
    0x00, 0x1A              // Checksum
  };
  
  this->hw_serial_->write(set_pass_packet, sizeof(set_pass_packet));
  this->hw_serial_->flush();
  
  // Wait for response
  start = millis();
  while (this->hw_serial_->available() < 12 && millis() - start < 500) {
    delay(10);
  }
  
  count = 0;
  while (this->hw_serial_->available() && count < 20) {
    response[count++] = this->hw_serial_->read();
  }
  
  // Log response
  if (count > 0) {
    char hex[64];
    int pos = 0;
    for (int i = 0; i < count && pos < 60; i++) {
      pos += snprintf(hex + pos, 64 - pos, "%02X ", response[i]);
    }
    ESP_LOGD(TAG, "Raw setPassword response (%d bytes): %s", count, hex);
  }
  
  if (count >= 12 && response[0] == 0xEF && response[1] == 0x01 && 
      response[6] == 0x07 && response[9] == 0x00) {
    ESP_LOGI(TAG, "Raw setPassword OK - password reset to default");
    return true;
  }
  
  ESP_LOGW(TAG, "Raw setPassword failed (count=%d, code=0x%02X)", count, count >= 10 ? response[9] : 0xFF);
  return false;
}

// Raw protocol helper to set password (more reliable than library)
bool FingerprintDoorbell::raw_set_password(uint32_t new_password) {
  ESP_LOGI(TAG, "Setting password via raw protocol to 0x%08X", new_password);
  
  // Clear serial buffer
  delay(50);
  while (this->hw_serial_->available()) this->hw_serial_->read();
  
  // Build setPassword packet (command 0x12)
  uint8_t p0 = (new_password >> 24) & 0xFF;
  uint8_t p1 = (new_password >> 16) & 0xFF;
  uint8_t p2 = (new_password >> 8) & 0xFF;
  uint8_t p3 = new_password & 0xFF;
  uint16_t checksum = 0x01 + 0x00 + 0x07 + 0x12 + p0 + p1 + p2 + p3;
  
  uint8_t packet[] = {
    0xEF, 0x01,             // Header
    0xFF, 0xFF, 0xFF, 0xFF, // Address
    0x01,                   // Command packet
    0x00, 0x07,             // Length
    0x12,                   // setPassword command
    p0, p1, p2, p3,         // New password
    (uint8_t)(checksum >> 8), (uint8_t)(checksum & 0xFF)
  };
  
  this->hw_serial_->write(packet, sizeof(packet));
  this->hw_serial_->flush();
  
  // Wait for response
  uint32_t start = millis();
  while (this->hw_serial_->available() < 12 && millis() - start < 500) {
    delay(10);
  }
  
  uint8_t response[20];
  int count = 0;
  while (this->hw_serial_->available() && count < 20) {
    response[count++] = this->hw_serial_->read();
  }
  
  // Log response
  if (count > 0) {
    char hex[64];
    int pos = 0;
    for (int i = 0; i < count && pos < 60; i++) {
      pos += snprintf(hex + pos, 64 - pos, "%02X ", response[i]);
    }
    ESP_LOGD(TAG, "Raw setPassword response (%d bytes): %s", count, hex);
  }
  
  if (count >= 12 && response[0] == 0xEF && response[1] == 0x01 && 
      response[6] == 0x07 && response[9] == 0x00) {
    ESP_LOGI(TAG, "Raw setPassword OK");
    return true;
  }
  
  ESP_LOGW(TAG, "Raw setPassword failed (count=%d, code=0x%02X)", count, count >= 10 ? response[9] : 0xFF);
  return false;
}

// ==================== REST API ====================

class FingerprintRequestHandler : public AsyncWebHandler {
 public:
  FingerprintRequestHandler(FingerprintDoorbell *parent) : parent_(parent) {}
  
  bool canHandle(AsyncWebServerRequest *request) const override {
    std::string url = request->url();
    return url.rfind("/fingerprint/", 0) == 0 || url.rfind("/pincode/", 0) == 0;
  }
  
  bool isRequestHandlerTrivial() const override { return false; }
  
  bool check_auth(AsyncWebServerRequest *request) const {
    std::string token = this->parent_->get_api_token();
    if (token.empty()) {
      return true;  // No token configured, allow access
    }
    
    // Check Authorization header: "Bearer <token>"
    auto auth_header = request->get_header("Authorization");
    if (!auth_header.has_value()) {
      return false;
    }
    
    std::string expected = "Bearer " + token;
    return auth_header.value() == expected;
  }
  
  void send_cors_response(AsyncWebServerRequest *request, int code, const char *content_type, const std::string &body) {
    AsyncWebServerResponse *response = request->beginResponse(code, content_type, body.c_str());
    // Note: Access-Control-Allow-Origin is added by ESPHome's web_server component
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    request->send(response);
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    // Handle CORS preflight
    if (request->method() == HTTP_OPTIONS) {
      auto *response = request->beginResponse(200, "text/plain", "");
      response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
      response->addHeader("Access-Control-Max-Age", "86400");
      request->send(response);
      return;
    }

    // Check authorization for all endpoints
    if (!this->check_auth(request)) {
      this->send_cors_response(request, 401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    std::string url = request->url();
    ESP_LOGD(TAG, "Request: %s %s", request->method() == HTTP_GET ? "GET" : (request->method() == HTTP_POST ? "POST" : "OTHER"), url.c_str());
    
    // GET /fingerprint/list - Get list of enrolled fingerprints
    if (url == "/fingerprint/list" && request->method() == HTTP_GET) {
      std::string json = this->parent_->get_fingerprint_list_json();
      this->send_cors_response(request, 200, "application/json", json);
      return;
    }
    
    // GET /fingerprint/status - Get current status
    if (url == "/fingerprint/status" && request->method() == HTTP_GET) {
      std::string json = "{";
      json += "\"connected\":" + std::string(this->parent_->is_sensor_connected() ? "true" : "false");
      json += ",\"paired\":" + std::string(this->parent_->is_sensor_paired() ? "true" : "false");
      json += ",\"enrolling\":" + std::string(this->parent_->is_enrolling() ? "true" : "false");
      json += ",\"count\":" + std::to_string(this->parent_->get_enrolled_count());
      json += "}";
      this->send_cors_response(request, 200, "application/json", json);
      return;
    }
    
    // POST /fingerprint/pair?password=XXXXXXXX - Pair sensor with password (hex string)
    if (url == "/fingerprint/pair" && request->method() == HTTP_POST) {
      if (!request->hasParam("password")) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing password parameter\"}");
        return;
      }
      std::string password_str = request->getParam("password")->value();
      
      // Parse hex string to uint32_t
      uint32_t password = 0;
      char *endptr;
      password = strtoul(password_str.c_str(), &endptr, 16);
      if (*endptr != '\0' || password_str.empty()) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Invalid password format (use hex, e.g. 12345678)\"}");
        return;
      }
      
      if (this->parent_->pair_sensor(password)) {
        this->send_cors_response(request, 200, "application/json", "{\"status\":\"paired\"}");
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Failed to pair sensor\"}");
      }
      return;
    }
    
    // POST /fingerprint/unpair - Unpair sensor (reset to default password)
    if (url == "/fingerprint/unpair" && request->method() == HTTP_POST) {
      if (this->parent_->unpair_sensor()) {
        this->send_cors_response(request, 200, "application/json", "{\"status\":\"unpaired\"}");
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Failed to unpair sensor\"}");
      }
      return;
    }
    
    // POST /fingerprint/factory_reset?password=XXXXXXXX - Factory reset sensor (delete all fingerprints and reset password)
    // Password is optional - if provided, will try to connect with it first
    if (url == "/fingerprint/factory_reset" && request->method() == HTTP_POST) {
      uint32_t old_password = 0xFFFFFFFF;  // Default: no password provided
      
      if (request->hasParam("password")) {
        std::string password_str = request->getParam("password")->value();
        char *endptr;
        old_password = strtoul(password_str.c_str(), &endptr, 16);
        if (*endptr != '\0' || password_str.empty()) {
          this->send_cors_response(request, 400, "application/json", "{\"error\":\"Invalid password format (use hex, e.g. 12345678)\"}");
          return;
        }
      }
      
      if (this->parent_->factory_reset_sensor(old_password)) {
        this->send_cors_response(request, 200, "application/json", "{\"status\":\"factory_reset_complete\"}");
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Factory reset failed - check password or sensor connection\"}");
      }
      return;
    }
    
    // POST /fingerprint/enroll?id=X&name=Y - Start enrollment
    if (url == "/fingerprint/enroll" && request->method() == HTTP_POST) {
      if (!request->hasParam("id") || !request->hasParam("name")) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing id or name parameter\"}");
        return;
      }
      std::string id_str = request->getParam("id")->value();
      std::string name = request->getParam("name")->value();
      uint16_t id = std::atoi(id_str.c_str());
      
      if (id < 1 || id > 200) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"ID must be 1-200\"}");
        return;
      }
      
      this->parent_->start_enrollment(id, name);
      std::string response = "{\"status\":\"enrollment_started\",\"id\":" + std::to_string(id) + ",\"name\":\"" + name + "\"}";
      this->send_cors_response(request, 200, "application/json", response);
      return;
    }
    
    // POST /fingerprint/cancel - Cancel enrollment
    if (url == "/fingerprint/cancel" && request->method() == HTTP_POST) {
      this->parent_->cancel_enrollment();
      this->send_cors_response(request, 200, "application/json", "{\"status\":\"cancelled\"}");
      return;
    }
    
    // POST /fingerprint/delete?id=X - Delete fingerprint
    if (url == "/fingerprint/delete" && request->method() == HTTP_POST) {
      if (!request->hasParam("id")) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing id parameter\"}");
        return;
      }
      std::string id_str = request->getParam("id")->value();
      uint16_t id = std::atoi(id_str.c_str());
      
      if (this->parent_->delete_fingerprint(id)) {
        std::string response = "{\"status\":\"deleted\",\"id\":" + std::to_string(id) + "}";
        this->send_cors_response(request, 200, "application/json", response);
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Delete failed\"}");
      }
      return;
    }
    
    // POST /fingerprint/delete_all - Delete all fingerprints
    if (url == "/fingerprint/delete_all" && request->method() == HTTP_POST) {
      if (this->parent_->delete_all_fingerprints()) {
        this->send_cors_response(request, 200, "application/json", "{\"status\":\"all_deleted\"}");
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Delete all failed\"}");
      }
      return;
    }
    
    // POST /fingerprint/rename?id=X&name=Y - Rename fingerprint
    if (url == "/fingerprint/rename" && request->method() == HTTP_POST) {
      if (!request->hasParam("id") || !request->hasParam("name")) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing id or name parameter\"}");
        return;
      }
      std::string id_str = request->getParam("id")->value();
      std::string name = request->getParam("name")->value();
      uint16_t id = std::atoi(id_str.c_str());
      
      if (this->parent_->rename_fingerprint(id, name)) {
        std::string response = "{\"status\":\"renamed\",\"id\":" + std::to_string(id) + ",\"name\":\"" + name + "\"}";
        this->send_cors_response(request, 200, "application/json", response);
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Rename failed\"}");
      }
      return;
    }
    
    // GET /fingerprint/template?id=X - Export fingerprint template as base64
    if (url == "/fingerprint/template" && request->method() == HTTP_GET) {
      if (!request->hasParam("id")) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing id parameter\"}");
        return;
      }
      std::string id_str = request->getParam("id")->value();
      uint16_t id = std::atoi(id_str.c_str());
      
      std::vector<uint8_t> template_data;
      if (this->parent_->get_template(id, template_data)) {
        // Base64 encode the template data
        std::string base64 = this->base64_encode(template_data);
        std::string name = this->parent_->get_fingerprint_name(id);
        std::string response = "{\"id\":" + std::to_string(id) + ",\"name\":\"" + name + "\",\"template\":\"" + base64 + "\"}";
        this->send_cors_response(request, 200, "application/json", response);
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Failed to export template\"}");
      }
      return;
    }
    
    // POST /fingerprint/template/chunk - Import fingerprint template in chunks
    // Query params: id, chunk (0-based index), total (total chunks), data (base64 chunk)
    // First chunk also includes: name
    if (url == "/fingerprint/template/chunk" && request->method() == HTTP_POST) {
      if (!request->hasParam("id") || !request->hasParam("chunk") || 
          !request->hasParam("total") || !request->hasParam("data")) {
        ESP_LOGW(TAG, "Chunk request missing params");
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing chunk parameters\"}");
        return;
      }
      
      uint16_t id = std::atoi(request->getParam("id")->value().c_str());
      int chunk_idx = std::atoi(request->getParam("chunk")->value().c_str());
      int total_chunks = std::atoi(request->getParam("total")->value().c_str());
      std::string data = request->getParam("data")->value();
      
      ESP_LOGD(TAG, "Chunk %d/%d for id=%d, data_len=%d", chunk_idx + 1, total_chunks, id, data.length());
      
      // First chunk - initialize buffer and store name
      if (chunk_idx == 0) {
        if (!request->hasParam("name")) {
          this->send_cors_response(request, 400, "application/json", "{\"error\":\"First chunk must include name\"}");
          return;
        }
        import_id_ = id;
        import_name_ = request->getParam("name")->value();
        import_buffer_.clear();
        import_buffer_.reserve(total_chunks * 500);  // Approximate size
        ESP_LOGI(TAG, "Starting chunked import: id=%d name='%s' chunks=%d", id, import_name_.c_str(), total_chunks);
      }
      
      // Verify this chunk is for the current import
      if (id != import_id_) {
        ESP_LOGW(TAG, "Chunk id mismatch: expected %d, got %d", import_id_, id);
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Chunk id mismatch\"}");
        return;
      }
      
      // Append chunk data
      import_buffer_ += data;
      
      // If not last chunk, just acknowledge
      if (chunk_idx < total_chunks - 1) {
        this->send_cors_response(request, 200, "application/json", "{\"status\":\"chunk_received\"}");
        return;
      }
      
      // Last chunk - process the complete template
      std::vector<uint8_t> template_data = this->base64_decode(import_buffer_);
      ESP_LOGI(TAG, "Received %d chunks, decoded %d bytes", total_chunks, (int)template_data.size());
      import_buffer_.clear();
      
      if (template_data.empty()) {
        ESP_LOGW(TAG, "Failed to decode base64 template");
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Invalid base64 template data\"}");
        return;
      }
      
      if (this->parent_->upload_template(import_id_, import_name_, template_data)) {
        std::string response = "{\"status\":\"imported\",\"id\":" + std::to_string(import_id_) + ",\"name\":\"" + import_name_ + "\"}";
        this->send_cors_response(request, 200, "application/json", response);
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Failed to import template\"}");
      }
      return;
    }
    
    // ==================== PIN CODE ENDPOINTS ====================
    
    // GET /pincode/list - List all PIN codes (without revealing the actual codes)
    if (url == "/pincode/list" && request->method() == HTTP_GET) {
      this->send_cors_response(request, 200, "application/json", this->parent_->get_pin_code_list_json());
      return;
    }
    
    // GET /pincode/status - Get keypad status
    if (url == "/pincode/status" && request->method() == HTTP_GET) {
      std::string json = "{\"enabled\":" + std::string(this->parent_->is_keypad_enabled() ? "true" : "false");
      json += ",\"count\":" + std::to_string(this->parent_->get_pin_code_count()) + "}";
      this->send_cors_response(request, 200, "application/json", json);
      return;
    }
    
    // POST /pincode/add?id=X&code=XXXX&name=Y - Add a new PIN code
    if (url == "/pincode/add" && request->method() == HTTP_POST) {
      if (!request->hasParam("id") || !request->hasParam("code") || !request->hasParam("name")) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing id, code, or name parameter\"}");
        return;
      }
      std::string id_str = request->getParam("id")->value();
      std::string code = request->getParam("code")->value();
      std::string name = request->getParam("name")->value();
      uint16_t id = std::atoi(id_str.c_str());
      
      if (id < 1 || id > 100) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"ID must be 1-100\"}");
        return;
      }
      if (code.length() < 4 || code.length() > 10) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Code must be 4-10 digits\"}");
        return;
      }
      for (char c : code) {
        if (c < '0' || c > '9') {
          this->send_cors_response(request, 400, "application/json", "{\"error\":\"Code must contain only digits 0-9\"}");
          return;
        }
      }
      
      if (this->parent_->add_pin_code(id, code, name)) {
        std::string response = "{\"status\":\"added\",\"id\":" + std::to_string(id) + ",\"name\":\"" + name + "\"}";
        this->send_cors_response(request, 200, "application/json", response);
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Failed to add PIN code (ID may already exist)\"}");
      }
      return;
    }
    
    // POST /pincode/delete?id=X - Delete a PIN code
    if (url == "/pincode/delete" && request->method() == HTTP_POST) {
      if (!request->hasParam("id")) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing id parameter\"}");
        return;
      }
      std::string id_str = request->getParam("id")->value();
      uint16_t id = std::atoi(id_str.c_str());
      
      if (this->parent_->delete_pin_code(id)) {
        std::string response = "{\"status\":\"deleted\",\"id\":" + std::to_string(id) + "}";
        this->send_cors_response(request, 200, "application/json", response);
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Delete failed\"}");
      }
      return;
    }
    
    // POST /pincode/delete_all - Delete all PIN codes
    if (url == "/pincode/delete_all" && request->method() == HTTP_POST) {
      if (this->parent_->delete_all_pin_codes()) {
        this->send_cors_response(request, 200, "application/json", "{\"status\":\"all_deleted\"}");
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Delete all failed\"}");
      }
      return;
    }
    
    // POST /pincode/rename?id=X&name=Y - Rename a PIN code
    if (url == "/pincode/rename" && request->method() == HTTP_POST) {
      if (!request->hasParam("id") || !request->hasParam("name")) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing id or name parameter\"}");
        return;
      }
      std::string id_str = request->getParam("id")->value();
      std::string name = request->getParam("name")->value();
      uint16_t id = std::atoi(id_str.c_str());
      
      if (this->parent_->rename_pin_code(id, name)) {
        std::string response = "{\"status\":\"renamed\",\"id\":" + std::to_string(id) + ",\"name\":\"" + name + "\"}";
        this->send_cors_response(request, 200, "application/json", response);
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Rename failed\"}");
      }
      return;
    }
    
    // POST /pincode/update?id=X&code=XXXX - Update PIN code value
    if (url == "/pincode/update" && request->method() == HTTP_POST) {
      if (!request->hasParam("id") || !request->hasParam("code")) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Missing id or code parameter\"}");
        return;
      }
      std::string id_str = request->getParam("id")->value();
      std::string code = request->getParam("code")->value();
      uint16_t id = std::atoi(id_str.c_str());
      
      if (code.length() < 4 || code.length() > 10) {
        this->send_cors_response(request, 400, "application/json", "{\"error\":\"Code must be 4-10 digits\"}");
        return;
      }
      for (char c : code) {
        if (c < '0' || c > '9') {
          this->send_cors_response(request, 400, "application/json", "{\"error\":\"Code must contain only digits 0-9\"}");
          return;
        }
      }
      
      if (this->parent_->update_pin_code(id, code)) {
        std::string response = "{\"status\":\"updated\",\"id\":" + std::to_string(id) + "}";
        this->send_cors_response(request, 200, "application/json", response);
      } else {
        this->send_cors_response(request, 500, "application/json", "{\"error\":\"Update failed\"}");
      }
      return;
    }
    
    this->send_cors_response(request, 404, "application/json", "{\"error\":\"Unknown endpoint\"}");
  }
  
  // Base64 encoding
  std::string base64_encode(const std::vector<uint8_t> &data) const {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((data.size() + 2) / 3 * 4);
    
    for (size_t i = 0; i < data.size(); i += 3) {
      uint32_t n = static_cast<uint32_t>(data[i]) << 16;
      if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
      if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);
      
      result += chars[(n >> 18) & 0x3F];
      result += chars[(n >> 12) & 0x3F];
      result += (i + 1 < data.size()) ? chars[(n >> 6) & 0x3F] : '=';
      result += (i + 2 < data.size()) ? chars[n & 0x3F] : '=';
    }
    return result;
  }
  
  // Base64 decoding
  std::vector<uint8_t> base64_decode(const std::string &input) const {
    static const int decode_table[256] = {
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
      52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
      -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
      15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
      -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
      41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    
    std::vector<uint8_t> result;
    result.reserve(input.size() * 3 / 4);
    
    uint32_t val = 0;
    int bits = 0;
    
    for (char c : input) {
      if (c == '=') break;
      // URL encoding converts '+' to space, so treat space as '+'
      if (c == ' ') c = '+';
      int d = decode_table[static_cast<uint8_t>(c)];
      if (d < 0) continue;
      
      val = (val << 6) | d;
      bits += 6;
      
      if (bits >= 8) {
        bits -= 8;
        result.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
      }
    }
    return result;
  }
  
 protected:
  FingerprintDoorbell *parent_;
  
  // Chunked import state
  uint16_t import_id_{0};
  std::string import_name_;
  std::string import_buffer_;
};

void FingerprintDoorbell::setup_web_server() {
  auto *base = web_server_base::global_web_server_base;
  if (base == nullptr) {
    ESP_LOGW(TAG, "WebServerBase not found, REST API disabled");
    return;
  }
  
  base->init();
  base->add_handler(new FingerprintRequestHandler(this));
  ESP_LOGI(TAG, "REST API registered at /fingerprint/* and /pincode/*");
}

// ==================== KEYPAD FUNCTIONS ====================

void FingerprintDoorbell::setup_keypad() {
  ESP_LOGD(TAG, "Setting up keypad with %d rows and %d cols", 
           this->keypad_row_pins_.size(), this->keypad_col_pins_.size());
  
  // Configure all pins as INPUT_PULLUP initially
  // Row pins will be switched to OUTPUT/LOW during scanning
  for (auto *pin : this->keypad_row_pins_) {
    pin->setup();
    pin->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
  }
  
  // Configure column pins as INPUT with PULLUP
  for (auto *pin : this->keypad_col_pins_) {
    pin->setup();
    pin->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
  }
  
  this->keypad_buffer_.clear();
  this->keypad_last_key_time_ = 0;
  this->keypad_last_key_ = 0;
  this->keypad_last_scan_time_ = 0;
}

void FingerprintDoorbell::scan_keypad() {
  // Throttle scanning to every 20ms
  uint32_t now = millis();
  if (now - this->keypad_last_scan_time_ < 20) {
    return;
  }
  this->keypad_last_scan_time_ = now;
  
  // Clear buffer if no input for 10 seconds
  if (this->keypad_buffer_.length() > 0 && 
      now - this->keypad_last_key_time_ > 10000) {
    ESP_LOGD(TAG, "Keypad buffer timeout, clearing");
    this->keypad_buffer_.clear();
  }
  
  char key = this->get_pressed_key();
  
  // Debounce: only process if key changed and debounce period passed
  if (key != this->keypad_last_key_) {
    // Key released or new key pressed
    if (key != 0 && (now - this->keypad_last_key_time_ > 150)) {
      // New key pressed after debounce
      this->process_keypad_input(key);
      this->keypad_last_key_time_ = now;
    }
    this->keypad_last_key_ = key;
  }
}

char FingerprintDoorbell::get_pressed_key() {
  // Standard 4x3 matrix keypad layout
  // Rows drive LOW, columns read with internal pullup
  // Layout:
  //        COL1  COL2  COL3
  // ROW1:   1     2     3
  // ROW2:   4     5     6
  // ROW3:   7     8     9
  // ROW4:   *     0     #
  static const char keys[4][3] = {
    {'1', '2', '3'},
    {'4', '5', '6'},
    {'7', '8', '9'},
    {'*', '0', '#'}
  };
  
  // Ensure column pins are INPUT_PULLUP before each scan
  for (auto *pin : this->keypad_col_pins_) {
    pin->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
  }
  delayMicroseconds(10);
  
  // Scan each row
  for (size_t row = 0; row < this->keypad_row_pins_.size() && row < 4; row++) {
    // Set current row to OUTPUT LOW
    this->keypad_row_pins_[row]->pin_mode(gpio::FLAG_OUTPUT);
    this->keypad_row_pins_[row]->digital_write(false);
    delayMicroseconds(50);
    
    for (size_t col = 0; col < this->keypad_col_pins_.size() && col < 3; col++) {
      bool col_state = this->keypad_col_pins_[col]->digital_read();
      if (!col_state) {
        // Key pressed - reset row and return
        this->keypad_row_pins_[row]->digital_write(true);
        this->keypad_row_pins_[row]->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
        ESP_LOGD(TAG, "Keypad: row=%d col=%d -> key='%c'", row, col, keys[row][col]);
        return keys[row][col];
      }
    }
    
    // Reset row to INPUT (high-impedance)
    this->keypad_row_pins_[row]->digital_write(true);
    this->keypad_row_pins_[row]->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
  }
  
  return 0;  // No key pressed
}

void FingerprintDoorbell::process_keypad_input(char key) {
  ESP_LOGD(TAG, "Keypad key pressed: %c", key);
  
  if (key == '*') {
    // Confirm/submit PIN
    if (this->keypad_buffer_.length() >= 4) {
      this->verify_pin_code();
    } else if (this->keypad_buffer_.length() > 0) {
      ESP_LOGD(TAG, "PIN too short (min 4 digits)");
      // Publish invalid attempt for too short PIN
      if (this->pin_invalid_sensor_ != nullptr) {
        this->pin_invalid_sensor_->publish_state(true);
        this->set_timeout(500, [this]() {
          this->pin_invalid_sensor_->publish_state(false);
        });
      }
      this->publish_last_action("PIN too short");
    }
    this->keypad_buffer_.clear();
  } else if (key == '#') {
    // Lock action
    this->trigger_lock_action();
    this->keypad_buffer_.clear();
  } else if (key >= '0' && key <= '9') {
    // Digit - add to buffer (max 10 digits)
    if (this->keypad_buffer_.length() < 10) {
      this->keypad_buffer_ += key;
      ESP_LOGD(TAG, "Keypad buffer: %d digits", this->keypad_buffer_.length());
    }
  }
}

void FingerprintDoorbell::verify_pin_code() {
  ESP_LOGI(TAG, "Verifying PIN code (%d digits)", this->keypad_buffer_.length());
  
  // Search for matching PIN code
  for (const auto &pair : this->pin_codes_) {
    if (pair.second.code == this->keypad_buffer_) {
      // Match found!
      ESP_LOGI(TAG, "PIN match: ID=%d, Name=%s", pair.first, pair.second.name.c_str());
      
      // Publish match to PIN-specific sensor only (not fingerprint match sensor)
      if (this->pin_match_name_sensor_ != nullptr) {
        this->pin_match_name_sensor_->publish_state(pair.second.name);
        // Reset PIN match name after 3 seconds
        this->set_timeout(3000, [this]() {
          if (this->pin_match_name_sensor_ != nullptr) {
            this->pin_match_name_sensor_->publish_state("");
          }
        });
      }
      
      this->publish_last_action("PIN unlock: " + pair.second.name);
      
      // Set LED to match state (same as fingerprint match)
      this->set_led_ring_match();
      this->last_match_time_ = millis();
      
      return;
    }
  }
  
  // No match - invalid PIN
  ESP_LOGW(TAG, "Invalid PIN code entered");
  
  if (this->pin_invalid_sensor_ != nullptr) {
    this->pin_invalid_sensor_->publish_state(true);
    this->set_timeout(500, [this]() {
      this->pin_invalid_sensor_->publish_state(false);
    });
  }
  
  this->publish_last_action("Invalid PIN");
  
  // Set LED to no-match state
  this->set_led_ring_no_match();
  this->last_ring_time_ = millis();
}

void FingerprintDoorbell::trigger_lock_action() {
  ESP_LOGI(TAG, "Lock action triggered via keypad");
  
  if (this->lock_action_sensor_ != nullptr) {
    this->lock_action_sensor_->publish_state(true);
    this->set_timeout(500, [this]() {
      this->lock_action_sensor_->publish_state(false);
    });
  }
  
  this->publish_last_action("Lock triggered");
}

// ==================== PIN CODE STORAGE ====================

void FingerprintDoorbell::load_pin_codes() {
  this->pin_codes_.clear();
  
  for (uint16_t i = 1; i <= 100; i++) {
    // Load code
    std::string code_key = "pin_code_" + std::to_string(i);
    ESPPreferenceObject code_pref = global_preferences->make_preference<std::array<char, 16>>(fnv1_hash(code_key.c_str()));
    
    std::array<char, 16> code_array;
    if (code_pref.load(&code_array)) {
      std::string code(code_array.data());
      if (!code.empty() && code[0] != '\0') {
        // Load name
        std::string name_key = "pin_name_" + std::to_string(i);
        ESPPreferenceObject name_pref = global_preferences->make_preference<std::array<char, 32>>(fnv1_hash(name_key.c_str()));
        
        std::array<char, 32> name_array;
        std::string name = "Unknown";
        if (name_pref.load(&name_array)) {
          name = std::string(name_array.data());
        }
        
        PinCode pc;
        pc.id = i;
        pc.code = code;
        pc.name = name;
        this->pin_codes_[i] = pc;
      }
    }
  }
  
  ESP_LOGI(TAG, "%d PIN codes loaded", this->pin_codes_.size());
}

void FingerprintDoorbell::save_pin_code(uint16_t id, const std::string &code, const std::string &name) {
  // Save code
  std::string code_key = "pin_code_" + std::to_string(id);
  ESPPreferenceObject code_pref = global_preferences->make_preference<std::array<char, 16>>(fnv1_hash(code_key.c_str()));
  
  std::array<char, 16> code_array = {};
  strncpy(code_array.data(), code.c_str(), 15);
  code_array[15] = '\0';
  code_pref.save(&code_array);
  
  // Save name
  std::string name_key = "pin_name_" + std::to_string(id);
  ESPPreferenceObject name_pref = global_preferences->make_preference<std::array<char, 32>>(fnv1_hash(name_key.c_str()));
  
  std::array<char, 32> name_array = {};
  strncpy(name_array.data(), name.c_str(), 31);
  name_array[31] = '\0';
  name_pref.save(&name_array);
  
  // Update in-memory map
  PinCode pc;
  pc.id = id;
  pc.code = code;
  pc.name = name;
  this->pin_codes_[id] = pc;
}

void FingerprintDoorbell::delete_pin_code_storage(uint16_t id) {
  // Clear code
  std::string code_key = "pin_code_" + std::to_string(id);
  ESPPreferenceObject code_pref = global_preferences->make_preference<std::array<char, 16>>(fnv1_hash(code_key.c_str()));
  
  std::array<char, 16> empty_code = {};
  code_pref.save(&empty_code);
  
  // Clear name
  std::string name_key = "pin_name_" + std::to_string(id);
  ESPPreferenceObject name_pref = global_preferences->make_preference<std::array<char, 32>>(fnv1_hash(name_key.c_str()));
  
  std::array<char, 32> empty_name = {};
  name_pref.save(&empty_name);
  
  // Remove from in-memory map
  this->pin_codes_.erase(id);
}

// ==================== PIN CODE MANAGEMENT (PUBLIC) ====================

bool FingerprintDoorbell::add_pin_code(uint16_t id, const std::string &code, const std::string &name) {
  // Check if ID already exists
  if (this->pin_codes_.find(id) != this->pin_codes_.end()) {
    ESP_LOGW(TAG, "PIN code ID %d already exists", id);
    return false;
  }
  
  // Check if code already exists (prevent duplicate codes)
  for (const auto &pair : this->pin_codes_) {
    if (pair.second.code == code) {
      ESP_LOGW(TAG, "PIN code already in use by ID %d", pair.first);
      return false;
    }
  }
  
  this->save_pin_code(id, code, name);
  ESP_LOGI(TAG, "Added PIN code: ID=%d, Name=%s", id, name.c_str());
  this->publish_last_action("PIN added: " + name);
  return true;
}

bool FingerprintDoorbell::delete_pin_code(uint16_t id) {
  if (this->pin_codes_.find(id) == this->pin_codes_.end()) {
    ESP_LOGW(TAG, "PIN code ID %d not found", id);
    return false;
  }
  
  std::string name = this->pin_codes_[id].name;
  this->delete_pin_code_storage(id);
  ESP_LOGI(TAG, "Deleted PIN code: ID=%d", id);
  this->publish_last_action("PIN deleted: " + name);
  return true;
}

bool FingerprintDoorbell::delete_all_pin_codes() {
  for (auto &pair : this->pin_codes_) {
    this->delete_pin_code_storage(pair.first);
  }
  this->pin_codes_.clear();
  ESP_LOGI(TAG, "All PIN codes deleted");
  this->publish_last_action("All PINs deleted");
  return true;
}

bool FingerprintDoorbell::rename_pin_code(uint16_t id, const std::string &new_name) {
  if (this->pin_codes_.find(id) == this->pin_codes_.end()) {
    ESP_LOGW(TAG, "PIN code ID %d not found", id);
    return false;
  }
  
  std::string code = this->pin_codes_[id].code;
  this->save_pin_code(id, code, new_name);
  ESP_LOGI(TAG, "Renamed PIN code: ID=%d to %s", id, new_name.c_str());
  this->publish_last_action("PIN renamed: " + new_name);
  return true;
}

bool FingerprintDoorbell::update_pin_code(uint16_t id, const std::string &new_code) {
  if (this->pin_codes_.find(id) == this->pin_codes_.end()) {
    ESP_LOGW(TAG, "PIN code ID %d not found", id);
    return false;
  }
  
  // Check if new code already exists
  for (const auto &pair : this->pin_codes_) {
    if (pair.first != id && pair.second.code == new_code) {
      ESP_LOGW(TAG, "PIN code already in use by ID %d", pair.first);
      return false;
    }
  }
  
  std::string name = this->pin_codes_[id].name;
  this->save_pin_code(id, new_code, name);
  ESP_LOGI(TAG, "Updated PIN code: ID=%d", id);
  this->publish_last_action("PIN updated: " + name);
  return true;
}

std::string FingerprintDoorbell::get_pin_code_list_json() {
  std::string json = "[";
  bool first = true;
  
  for (const auto &pair : this->pin_codes_) {
    if (!first) json += ",";
    first = false;
    // Note: We don't return the actual code for security
    json += "{\"id\":" + std::to_string(pair.first);
    json += ",\"name\":\"" + pair.second.name + "\"}";
  }
  
  json += "]";
  return json;
}

uint16_t FingerprintDoorbell::get_pin_code_count() {
  return this->pin_codes_.size();
}

}  // namespace fingerprint_doorbell
}  // namespace esphome
