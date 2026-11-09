import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_GAS,
    DEVICE_CLASS_MONETARY,
    DEVICE_CLASS_POWER,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CUBIC_METER,
    UNIT_KILOWATT_HOURS,
    UNIT_WATT,
)

from . import CONF_SEEZERO_ID, SeeZero

GBP = "GBP"


def _energy(today=True):
    # today/week/month reset to zero; total_increasing handles that as a new cycle
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_KILOWATT_HOURS,
        accuracy_decimals=3,
        device_class=DEVICE_CLASS_ENERGY,
        state_class=STATE_CLASS_TOTAL_INCREASING if today else cv.UNDEFINED,
    )


def _cost():
    # plain values (no state_class): they reset daily/weekly/monthly
    return sensor.sensor_schema(
        unit_of_measurement=GBP, accuracy_decimals=2, device_class=DEVICE_CLASS_MONETARY
    )


def _rate(unit=cv.UNDEFINED, icon="mdi:currency-gbp", decimals=5):
    return sensor.sensor_schema(unit_of_measurement=unit, accuracy_decimals=decimals, icon=icon)


# Order must match enum SensorKey in seezero.h
SENSOR_KEYS = {
    "electricity_power": sensor.sensor_schema(
        unit_of_measurement=UNIT_WATT,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    "electricity_import": _energy(),
    "electricity_export": _energy(),
    "electricity_today": _energy(),
    "electricity_yesterday": _energy(today=False),
    "electricity_this_week": _energy(),
    "electricity_this_month": _energy(),
    "electricity_cost_today": _cost(),
    "electricity_cost_yesterday": _cost(),
    "electricity_cost_this_week": _cost(),
    "electricity_cost_this_month": _cost(),
    "electricity_unit_rate": _rate("GBP/kWh"),
    "electricity_standing_charge": _rate("GBP/day"),
    "gas_meter": sensor.sensor_schema(
        unit_of_measurement=UNIT_CUBIC_METER,
        accuracy_decimals=3,
        device_class=DEVICE_CLASS_GAS,
        state_class=STATE_CLASS_TOTAL_INCREASING,
    ),
    "gas_today": _energy(),
    "gas_yesterday": _energy(today=False),
    "gas_this_week": _energy(),
    "gas_this_month": _energy(),
    "gas_cost_today": _cost(),
    "gas_cost_yesterday": _cost(),
    "gas_cost_this_week": _cost(),
    "gas_cost_this_month": _cost(),
    "gas_unit_rate": _rate("GBP/kWh"),
    "gas_unit_rate_m3": _rate("GBP/m³"),
    "gas_standing_charge": _rate("GBP/day"),
    "gas_calorific_value": _rate("MJ/m³", icon="mdi:fire", decimals=1),
    "gas_conversion_factor": _rate(icon="mdi:swap-horizontal", decimals=5),
    "frames_ok": sensor.sensor_schema(
        accuracy_decimals=0,
        icon="mdi:counter",
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "crc_errors": sensor.sensor_schema(
        accuracy_decimals=0,
        icon="mdi:alert-circle-outline",
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_SEEZERO_ID): cv.use_id(SeeZero)}
    | {cv.Optional(key): schema for key, schema in SENSOR_KEYS.items()}
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_SEEZERO_ID])
    for index, key in enumerate(SENSOR_KEYS):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(hub.set_sensor(index, sens))
