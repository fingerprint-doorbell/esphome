import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_OCCUPANCY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_LOCK,
)
from . import FingerprintDoorbell, CONF_FINGERPRINT_DOORBELL_ID, fingerprint_doorbell_ns

DEPENDENCIES = ["fingerprint_doorbell"]

CONF_RING = "ring"
CONF_FINGER = "finger"
CONF_PIN_INVALID = "pin_invalid"
CONF_LOCK_ACTION = "lock_action"
CONF_UNLOCK_ACTION = "unlock_action"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FINGERPRINT_DOORBELL_ID): cv.use_id(FingerprintDoorbell),
        cv.Optional(CONF_RING): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_OCCUPANCY,
        ),
        cv.Optional(CONF_FINGER): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_OCCUPANCY,
        ),
        cv.Optional(CONF_PIN_INVALID): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PROBLEM,
            icon="mdi:alert-circle",
        ),
        cv.Optional(CONF_LOCK_ACTION): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_LOCK,
            icon="mdi:lock",
        ),
        cv.Optional(CONF_UNLOCK_ACTION): binary_sensor.binary_sensor_schema(
            icon="mdi:lock-open",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FINGERPRINT_DOORBELL_ID])

    if CONF_RING in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_RING])
        cg.add(parent.set_ring_sensor(sens))

    if CONF_FINGER in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_FINGER])
        cg.add(parent.set_finger_sensor(sens))

    if CONF_PIN_INVALID in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_PIN_INVALID])
        cg.add(parent.set_pin_invalid_sensor(sens))

    if CONF_LOCK_ACTION in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_LOCK_ACTION])
        cg.add(parent.set_lock_action_sensor(sens))

    if CONF_UNLOCK_ACTION in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_UNLOCK_ACTION])
        cg.add(parent.set_unlock_action_sensor(sens))
