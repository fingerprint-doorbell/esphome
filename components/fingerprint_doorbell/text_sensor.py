import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID, ICON_FINGERPRINT
from . import FingerprintDoorbell, CONF_FINGERPRINT_DOORBELL_ID, fingerprint_doorbell_ns

DEPENDENCIES = ["fingerprint_doorbell"]

CONF_MATCH_NAME = "match_name"
CONF_ENROLL_STATUS = "enroll_status"
CONF_LAST_ACTION = "last_action"
CONF_PIN_MATCH_NAME = "pin_match_name"
CONF_INVALID_ACTION = "invalid_action"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FINGERPRINT_DOORBELL_ID): cv.use_id(FingerprintDoorbell),
        cv.Optional("match_name"): text_sensor.text_sensor_schema(
            icon=ICON_FINGERPRINT,
        ),
        cv.Optional("enroll_status"): text_sensor.text_sensor_schema(
            icon="mdi:account-plus",
        ),
        cv.Optional("last_action"): text_sensor.text_sensor_schema(
            icon="mdi:history",
        ),
        cv.Optional("pin_match_name"): text_sensor.text_sensor_schema(
            icon="mdi:dialpad",
        ),
        cv.Optional("invalid_action"): text_sensor.text_sensor_schema(
            icon="mdi:alert-circle",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FINGERPRINT_DOORBELL_ID])

    if CONF_MATCH_NAME in config:
        sens = await text_sensor.new_text_sensor(config[CONF_MATCH_NAME])
        cg.add(parent.set_match_name_sensor(sens))

    if CONF_ENROLL_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_ENROLL_STATUS])
        cg.add(parent.set_enroll_status_sensor(sens))

    if CONF_LAST_ACTION in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LAST_ACTION])
        cg.add(parent.set_last_action_sensor(sens))

    if CONF_PIN_MATCH_NAME in config:
        sens = await text_sensor.new_text_sensor(config[CONF_PIN_MATCH_NAME])
        cg.add(parent.set_pin_match_name_sensor(sens))

    if CONF_INVALID_ACTION in config:
        sens = await text_sensor.new_text_sensor(config[CONF_INVALID_ACTION])
        cg.add(parent.set_invalid_action_sensor(sens))
