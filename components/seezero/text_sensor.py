import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_SEEZERO_ID, SeeZero

CONF_SUPPLIER_MESSAGE = "supplier_message"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SEEZERO_ID): cv.use_id(SeeZero),
        cv.Optional(CONF_SUPPLIER_MESSAGE): text_sensor.text_sensor_schema(
            icon="mdi:message-text-outline"
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_SEEZERO_ID])
    if CONF_SUPPLIER_MESSAGE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SUPPLIER_MESSAGE])
        cg.add(hub.set_message_sensor(sens))
