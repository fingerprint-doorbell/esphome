# Fingerprint Doorbell - ESPHome Package

Transform your existing Fingerprint Doorbell project into a native ESPHome component with full Home Assistant integration!

## 🎯 What This Package Does

This ESPHome package converts your Arduino-based Fingerprint Doorbell into a native ESPHome component, providing:

- ✅ **Native Home Assistant Integration** - No MQTT needed, direct API integration
- ✅ **Auto-Discovery** - Sensors automatically appear in Home Assistant
- ✅ **Services** - Enroll, delete, and manage fingerprints from Home Assistant
- ✅ **OTA Updates** - Update firmware wirelessly from ESPHome dashboard
- ✅ **Web Interface** - Built-in web server for diagnostics
- ✅ **Full Feature Parity** - All original features preserved

## 📋 Prerequisites

- **ESPHome** installed (via Home Assistant add-on or standalone)
- **Home Assistant** (for full integration)
- **ESP32** board (esp32doit-devkit-v1 or compatible)
- **Grow R503** fingerprint sensor
- Basic knowledge of YAML configuration

## 🚀 Quick Start

### Method 1: Local Package (Recommended for Development)

1. **Copy the package to your ESPHome config directory:**
   ```bash
   cp -r esphome-package/components ~/.esphome/
   cp esphome-package/fingerprint-doorbell.yaml ~/.esphome/packages/
   ```

2. **Create your device configuration:**
   ```yaml
   # fingerprint-doorbell-front.yaml
   substitutions:
     name: fingerprint-doorbell-front
     friendly_name: "Front Door Fingerprint"

   packages:
     fingerprint_doorbell: !include packages/fingerprint-doorbell.yaml

   esphome:
     name: ${name}
     platform: ESP32
     board: esp32doit-devkit-v1

   wifi:
     ssid: !secret wifi_ssid
     password: !secret wifi_password

   api:
     encryption:
       key: !secret api_encryption_key

   ota:
     password: !secret ota_password
   ```

3. **Compile and upload:**
   ```bash
   esphome run fingerprint-doorbell-front.yaml
   ```

### Method 2: GitHub Package (Once Published)

```yaml
substitutions:
  name: fingerprint-doorbell

packages:
  fingerprint_doorbell: github://YOUR_USERNAME/fingerprint-doorbell-esphome/fingerprint-doorbell.yaml@main

esphome:
  name: ${name}
  platform: ESP32
  board: esp32doit-devkit-v1

# ... rest of config
```

## 🔌 Hardware Wiring

Same as the original project:

| ESP32 Pin | R503 Sensor Pin | Description |
|-----------|-----------------|-------------|
| GPIO16    | RX (Yellow)     | UART RX     |
| GPIO17    | TX (White)      | UART TX     |
| GPIO5     | Touch (Blue)    | Touch Ring  |
| 3.3V      | VCC (Red)       | Power       |
| GND       | GND (Black)     | Ground      |
| GPIO19    | -               | Doorbell Output (optional) |

**Important:** Ground the sensor housing to prevent ESD resets!

## 📊 Exposed Entities

### Sensors
- **Fingerprint Match ID** (`sensor.fingerprint_match_id`)
  - ID of matched fingerprint (1-200)
  - `-1` when no match

- **Fingerprint Confidence** (`sensor.fingerprint_confidence`)
  - Match confidence score (1-400, higher is better)
  - `0` when no match

### Text Sensors
- **Fingerprint Match Name** (`text_sensor.fingerprint_match_name`)
  - Name of matched fingerprint
  - Empty when no match

### Binary Sensors
- **Doorbell Ring** (`binary_sensor.doorbell_ring`)
  - `on` when unknown finger detected (doorbell event)
  - Stays `on` for 1 second

- **Finger Detected** (`binary_sensor.finger_detected`)
  - `on` when any finger is on the sensor
  - `off` when no finger present

## 🛠️ Services & Actions

This component provides two ways to manage fingerprints:

1. **Home Assistant Actions** - Use in automations with `action:` blocks
2. **REST API** - Direct HTTP calls for standalone use

### Home Assistant Actions

All actions are available in your ESPHome device's YAML or Home Assistant automations:

#### `fingerprint_doorbell.enroll`
Enroll a new fingerprint.

**Parameters:**
- `finger_id` (int): ID 1-200
- `name` (string): Name for this fingerprint

**Example in automation:**
```yaml
automation:
  - alias: "Enroll New Finger"
    trigger:
      - platform: event
        event_type: mobile_app_notification_action
        event_data:
          action: ENROLL_FINGER
    action:
      - action: fingerprint_doorbell.enroll
        data:
          finger_id: 1
          name: "John"
```

**Example in ESPHome button:**
```yaml
button:
  - platform: template
    name: "Enroll John"
    on_press:
      - fingerprint_doorbell.enroll:
          finger_id: 1
          name: "John"
```

**Process:**
1. Call the action
2. Place finger on sensor when LED flashes purple
3. Remove finger when LED turns solid purple
4. Repeat 5 times total
5. Enrollment complete!

#### `fingerprint_doorbell.cancel_enroll`
Cancel an in-progress enrollment.

**Example:**
```yaml
button:
  - platform: template
    name: "Cancel Enrollment"
    on_press:
      - fingerprint_doorbell.cancel_enroll
```

#### `fingerprint_doorbell.delete`
Delete a single fingerprint.

**Parameters:**
- `finger_id` (int): ID to delete

**Example:**
```yaml
button:
  - platform: template
    name: "Delete Finger 1"
    on_press:
      - fingerprint_doorbell.delete:
          finger_id: 1
```

#### `fingerprint_doorbell.delete_all`
Delete all enrolled fingerprints.

**Example:**
```yaml
button:
  - platform: template
    name: "Delete All"
    on_press:
      - fingerprint_doorbell.delete_all
```

#### `fingerprint_doorbell.rename`
Rename an existing fingerprint.

**Parameters:**
- `finger_id` (int): ID to rename
- `name` (string): New name

**Example:**
```yaml
button:
  - platform: template
    name: "Rename Finger 1"
    on_press:
      - fingerprint_doorbell.rename:
          finger_id: 1
          name: "John Smith"
```

---

### REST API Endpoints

The component exposes a REST API at `/fingerprint/*` for standalone device management without Home Assistant.

**Base URL:** `http://<device-ip>/fingerprint/`

#### `GET /fingerprint/list`
Get list of all enrolled fingerprints.

**Example:**
```bash
curl -H "Authorization: Bearer your-secret-token-here" http://192.168.1.100/fingerprint/list
```

**Response:**
```json
[
  {"id": 1, "name": "John"},
  {"id": 2, "name": "Jane"}
]
```

#### `GET /fingerprint/status`
Get current sensor status.

**Example:**
```bash
curl -H "Authorization: Bearer your-secret-token-here" http://192.168.1.100/fingerprint/status
```

**Response:**
```json
{
  "connected": true,
  "enrolling": false,
  "count": 2
}
```

#### `POST /fingerprint/enroll?id=X&name=Y`
Start fingerprint enrollment.

**Parameters:**
- `id` (int): Fingerprint ID (1-200)
- `name` (string): Name for fingerprint

**Example:**
```bash
curl -H "Authorization: Bearer your-secret-token-here" -X POST "http://192.168.1.100/fingerprint/enroll?id=3&name=Alice"
```

**Response:**
```json
{"status": "enrollment_started", "id": 3, "name": "Alice"}
```

After calling, place finger on sensor 5 times (LED guides you).

#### `POST /fingerprint/cancel`
Cancel in-progress enrollment.

**Example:**
```bash
curl -H "Authorization: Bearer your-secret-token-here" -X POST http://192.168.1.100/fingerprint/cancel
```

**Response:**
```json
{"status": "cancelled"}
```

#### `POST /fingerprint/delete?id=X`
Delete a fingerprint.

**Parameters:**
- `id` (int): Fingerprint ID to delete

**Example:**
```bash
curl -H "Authorization: Bearer your-secret-token-here" -X POST "http://192.168.1.100/fingerprint/delete?id=1"
```

**Response:**
```json
{"status": "deleted", "id": 1}
```

#### `POST /fingerprint/delete_all`
Delete all fingerprints.

**Example:**
```bash
curl -H "Authorization: Bearer your-secret-token-here" -X POST http://192.168.1.100/fingerprint/delete_all
```

**Response:**
```json
{"status": "all_deleted"}
```

#### `POST /fingerprint/rename?id=X&name=Y`
Rename a fingerprint.

**Parameters:**
- `id` (int): Fingerprint ID
- `name` (string): New name

**Example:**
```bash
curl -H "Authorization: Bearer your-secret-token-here" -X POST "http://192.168.1.100/fingerprint/rename?id=1&name=John%20Smith"
```

**Response:**
```json
{"status": "renamed", "id": 1, "name": "John Smith"}
```

#### REST API Authentication

The REST API can be protected with a Bearer token. Configure `api_token` in your component:

```yaml
fingerprint_doorbell:
  id: fp_doorbell
  api_token: "your-secret-token-here"
```

**Tip:** Reuse your ESPHome API encryption key for convenience:

```yaml
fingerprint_doorbell:
  id: fp_doorbell
  api_token: !secret api_encryption_key
```

| Configuration | Behavior |
|---------------|----------|
| No `api_token` set | API is open (backward compatible) |
| `api_token` configured | All endpoints require `Authorization: Bearer <token>` header |

**Response when unauthorized (HTTP 401):**
```json
{"error": "Unauthorized"}
```

---

### Additional Text Sensors

To monitor enrollment progress, add these text sensors:

```yaml
text_sensor:
  - platform: fingerprint_doorbell
    enroll_status:
      name: "Enrollment Status"
    last_action:
      name: "Last Action"
```

**Enrollment Status Values:**
- `Place finger (1/5)` through `Place finger (5/5)`
- `Remove finger`
- `Creating model...`
- `Storing...`
- `Success!`
- `Error: ...` (various error messages)
- `Timeout`
- `Cancelled`

## 🎨 LED Ring Indicators

| Color | Pattern | Meaning |
|-------|---------|---------|
| Blue | Breathing | Ready (touch ring enabled) |
| Blue | Solid | Ready (touch ring disabled) |
| Red | Flashing | Finger detected, scanning |
| Purple | Flashing | Enrollment mode, waiting for finger |
| Purple | Solid | Match found / enrollment pass complete |
| Red | Solid | Error state |

## 🏡 Home Assistant Integration Examples

### Auto-Unlock Door on Match
```yaml
automation:
  - alias: "Front Door Auto Unlock"
    trigger:
      - platform: state
        entity_id: sensor.fingerprint_match_id
    condition:
      - condition: numeric_state
        entity_id: sensor.fingerprint_match_id
        above: 0
      - condition: numeric_state
        entity_id: sensor.fingerprint_confidence
        above: 150
    action:
      - service: lock.unlock
        target:
          entity_id: lock.front_door
      - service: notify.mobile_app
        data:
          message: "Door unlocked for {{ states('text_sensor.fingerprint_match_name') }}"
```

### Doorbell Notification
```yaml
automation:
  - alias: "Doorbell Ring Notification"
    trigger:
      - platform: state
        entity_id: binary_sensor.doorbell_ring
        to: "on"
    action:
      - service: notify.mobile_app
        data:
          message: "Someone is at the front door"
          title: "Doorbell Ring"
```

### Rain Protection
```yaml
automation:
  - alias: "Fingerprint Rain Protection"
    trigger:
      - platform: state
        entity_id: binary_sensor.rain_sensor
    action:
      - service: esphome.fingerprint_doorbell_front_set_ignore_touch_ring
        data:
          state: "{{ is_state('binary_sensor.rain_sensor', 'on') }}"
```

## ⚙️ Configuration Options

### Override Default Pins
```yaml
fingerprint_doorbell:
  touch_pin: GPIO5       # Default
  doorbell_pin: GPIO19   # Default
  ignore_touch_ring: false  # Set true for rain-exposed sensors
```

### Customize UART Pins
```yaml
fingerprint_doorbell:
  sensor_rx_pin: 16  # ESP32 RX <- Sensor TX (default GPIO16)
  sensor_tx_pin: 17  # ESP32 TX -> Sensor RX (default GPIO17)
```

### Customize Sensor Names
```yaml
sensor:
  - platform: fingerprint_doorbell
    match_id:
      name: "Custom Match ID Name"
    confidence:
      name: "Custom Confidence Name"
```

## 🔧 Troubleshooting

### Sensor Not Found
- Check wiring (RX/TX may need to be swapped - sensor TX goes to ESP32 RX)
- Verify 3.3V power supply
- Ensure sensor housing is grounded
- Check ESPHome logs: `esphome logs fingerprint-doorbell-front.yaml`

### False Doorbell Rings in Rain
- Set `ignore_touch_ring: true` in config, OR
- Use Home Assistant automation to dynamically enable rain protection

### Enrollment Fails
- Ensure finger is clean and dry
- Place finger consistently in the same position
- Don't press too hard or too soft
- Wait for LED to turn solid purple before removing finger

### Compilation Errors
- Ensure you have the Adafruit Fingerprint library:
  ```yaml
  esphome:
    libraries:
      - "adafruit/Adafruit Fingerprint Sensor Library@^2.1.0"
  ```

## 📦 What's Included

```
esphome-package/
├── components/
│   └── fingerprint_doorbell/
│       ├── __init__.py              # Component registration
│       ├── fingerprint_doorbell.h   # C++ header
│       ├── fingerprint_doorbell.cpp # Core implementation
│       ├── sensor.py                # Sensor platform
│       ├── text_sensor.py           # Text sensor platform
│       └── binary_sensor.py         # Binary sensor platform
├── fingerprint-doorbell.yaml        # Main package config
├── example-config.yaml              # User config example
├── example-secrets.yaml             # Secrets template
└── home-assistant-examples.yaml     # HA automation examples
```

## 🆚 Comparison: Original vs ESPHome

| Feature | Original (PlatformIO) | ESPHome Package |
|---------|----------------------|-----------------|
| Home Assistant Integration | MQTT | Native API |
| Configuration | Web UI | YAML + HA Services |
| Updates | Manual/Web OTA | ESPHome Dashboard |
| Fingerprint Management | Web UI | HA Services |
| Logging | Web UI | ESPHome Logs |
| Sensor Data | MQTT Topics | HA Entities |
| Setup Complexity | Medium | Easy |
| Customization | C++ Code | YAML Config |

## 🔄 Migration from Original

If you're migrating from the original PlatformIO project:

1. **Fingerprints are NOT preserved** - You'll need to re-enroll
2. **Settings reset** - Reconfigure via ESPHome YAML
3. **MQTT not needed** - Uses Home Assistant API
4. **Web UI replaced** - Use Home Assistant UI + Services

## 📚 Additional Resources

- [ESPHome Documentation](https://esphome.io)
- [Original Project README](../README.md)
- [Grow R503 Datasheet](https://cdn.shopify.com/s/files/1/0551/3656/5159/files/R503_fingerprint_module_user_manual.pdf)
- [Home Assistant ESPHome Integration](https://www.home-assistant.io/integrations/esphome/)

## 🤝 Contributing

Found a bug or have a feature request? Please open an issue or submit a pull request!

## 📄 License

Same license as the original FingerprintDoorbell project.

## 🙏 Credits

- Original FingerprintDoorbell by frickelzeugs
- ESPHome package conversion by [Your Name]
- Inspired by Everything Smart Home's Everything Presence One

---


**Matrix Keypad**

| Matrix-Pad | Function | ESP32-GPIO | Cable Color |
|------------|----------|------------|-------------|
| PAD 4      | ROW1     | IO32       | Blue        |
| PAD 2      | ROW2     | IO27       | Yellow      |
| PAD 6      | ROW3     | IO18       | Orange      |
| PAD 5      | ROW4     | IO04       | Purple      |
| PAD 3      | COL1     | IO25       | Green       |
| PAD 8      | COL2     | IO22       | Red         |
| PAD 7      | COL3     | IO33       | White       |

### Keypad Layout
```
  COL1  COL2  COL3
   |     |     |
ROW1--[1]--[2]--[3]
ROW2--[4]--[5]--[6]
ROW3--[7]--[8]--[9]
ROW4--[*]--[0]--[#]
```

| Key | Function |
|-----|----------|
| `0-9` | Enter PIN digits (4-10 digits) |
| `*` | Confirm/submit PIN → unlock |
| `#` | Trigger lock action |

---

## 🔐 PIN Code Management

### PIN Code Sensors

| Sensor | Description |
|--------|-------------|
| `pin_match_name` | Name of successfully matched PIN |
| `pin_invalid` | Pulses `on` for 500ms on invalid PIN attempt |
| `lock_action` | Pulses `on` for 500ms when `#` is pressed |

**Note:** On successful PIN match, `match_name` sensor also shows `"PIN: <name>"` for unified automation handling.

### PIN Code REST API

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/pincode/list` | List all PIN codes (codes hidden) |
| GET | `/pincode/status` | Keypad status (enabled, count) |
| POST | `/pincode/add?id=X&code=1234&name=Y` | Add PIN (4-10 digits) |
| POST | `/pincode/delete?id=X` | Delete PIN |
| POST | `/pincode/delete_all` | Delete all PINs |
| POST | `/pincode/rename?id=X&name=Y` | Rename PIN |
| POST | `/pincode/update?id=X&code=5678` | Change PIN code |

**Example - Add PIN:**
```bash
curl -H "Authorization: Bearer your-token" \
  -X POST "http://192.168.1.100/pincode/add?id=1&code=1234&name=John"
```

### Home Assistant Automations for PIN

**Unlock on valid PIN:**
```yaml
automation:
  - alias: "PIN Code Unlock"
    trigger:
      - platform: state
        entity_id: text_sensor.pin_match_name
    condition:
      - condition: template
        value_template: "{{ trigger.to_state.state != '' }}"
    action:
      - service: lock.unlock
        target:
          entity_id: lock.front_door
      - service: notify.mobile_app
        data:
          message: "Door unlocked by PIN: {{ trigger.to_state.state }}"
```

**Lock on # key press:**
```yaml
automation:
  - alias: "Lock Door via Keypad"
    trigger:
      - platform: state
        entity_id: binary_sensor.lock_action
        to: "on"
    action:
      - service: lock.lock
        target:
          entity_id: lock.front_door
```

**Alert on invalid PIN:**
```yaml
automation:
  - alias: "Invalid PIN Alert"
    trigger:
      - platform: state
        entity_id: binary_sensor.pin_invalid
        to: "on"
    action:
      - service: notify.mobile_app
        data:
          message: "Invalid PIN attempt at front door!"
          title: "Security Alert"
```    

