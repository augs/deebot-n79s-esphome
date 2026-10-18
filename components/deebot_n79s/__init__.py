"""Deebot N79S mainboard UART protocol."""

import esphome.codegen as cg
from esphome.components import sensor, text_sensor, time as time_, uart
import esphome.config_validation as cv
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_ID,
    CONF_STATUS,
    CONF_TIME_ID,
    DEVICE_CLASS_BATTERY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor"]

CONF_BRUSH = "brush"
CONF_SIDE_BRUSH = "side_brush"
CONF_DUST_CASE = "dust_case"
CONF_ERROR = "error"
CONF_FAN_SPEED = "fan_speed"
CONF_CLEAN_MODE = "clean_mode"
CONF_CHARGE_STATE = "charge_state"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_SCHEDULES = "schedules"

deebot_ns = cg.esphome_ns.namespace("deebot_n79s")
DeebotN79S = deebot_ns.class_("DeebotN79S", cg.PollingComponent, uart.UARTDevice)


def _lifespan_schema(icon):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        icon=icon,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


def _text_schema(icon, diagnostic=False):
    return text_sensor.text_sensor_schema(
        icon=icon,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC if diagnostic else cv.UNDEFINED,
    )


_SENSORS = {
    CONF_BATTERY_LEVEL: sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_BATTERY,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_BRUSH: _lifespan_schema("mdi:brush"),
    CONF_SIDE_BRUSH: _lifespan_schema("mdi:broom"),
    CONF_DUST_CASE: _lifespan_schema("mdi:air-filter"),
}

_TEXT_SENSORS = {
    CONF_STATUS: _text_schema("mdi:robot-vacuum"),
    CONF_ERROR: _text_schema("mdi:alert-circle-outline"),
    CONF_FAN_SPEED: _text_schema("mdi:fan"),
    CONF_CLEAN_MODE: _text_schema("mdi:map-marker-path"),
    CONF_CHARGE_STATE: _text_schema("mdi:battery-charging", diagnostic=True),
    CONF_FIRMWARE_VERSION: _text_schema("mdi:chip", diagnostic=True),
    CONF_SCHEDULES: _text_schema("mdi:calendar-clock"),
}

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DeebotN79S),
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            **{cv.Optional(key): schema for key, schema in _SENSORS.items()},
            **{cv.Optional(key): schema for key, schema in _TEXT_SENSORS.items()},
        }
    )
    # Status changes arrive as reports; polling only catches missed ones.
    .extend(cv.polling_component_schema("300s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "deebot_n79s", require_tx=True, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if CONF_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(clock))

    for key in _SENSORS:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(sens))

    for key in _TEXT_SENSORS:
        if key in config:
            sens = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_text_sensor")(sens))
