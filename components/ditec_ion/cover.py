import esphome.codegen as cg
from esphome.components import binary_sensor, ble_client, cover, sensor, text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_NOTIFY, ENTITY_CATEGORY_DIAGNOSTIC, UNIT_PERCENT

from . import ditec_ion_ns

DEPENDENCIES = ["esp32", "ble_client"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]

CONF_OPERATION_COUNTER = "operation_counter"
CONF_PARTIAL_OPEN_POSITION = "partial_open_position"
CONF_AUTO_CLOSE_TIMER = "auto_close_timer"
CONF_AUTO_CLOSE = "auto_close"
CONF_BOARD_INFO = "board_info"

DitecIonCover = ditec_ion_ns.class_(
    "DitecIonCover", cover.Cover, cg.Component, ble_client.BLEClientNode
)

CONFIG_SCHEMA = (
    cover.cover_schema(DitecIonCover, device_class="gate")
    .extend(
        {
            # Also subscribe to notifications on the gate's characteristic (if it has the Notify
            # property) and feed them to the same frame parser. Off by default: the original
            # project receives frames only via the ESP32's own GATT server (esp32_ble_server).
            cv.Optional(CONF_NOTIFY, default=False): cv.boolean,
            cv.Optional(CONF_OPERATION_COUNTER): sensor.sensor_schema(
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PARTIAL_OPEN_POSITION): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_AUTO_CLOSE_TIMER): sensor.sensor_schema(
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_AUTO_CLOSE): binary_sensor.binary_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BOARD_INFO): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_notify(config[CONF_NOTIFY]))

    if conf := config.get(CONF_OPERATION_COUNTER):
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_operation_counter_sensor(sens))
    if conf := config.get(CONF_PARTIAL_OPEN_POSITION):
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_partial_open_position_sensor(sens))
    if conf := config.get(CONF_AUTO_CLOSE_TIMER):
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_auto_close_timer_sensor(sens))
    if conf := config.get(CONF_AUTO_CLOSE):
        sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(var.set_auto_close_binary_sensor(sens))
    if conf := config.get(CONF_BOARD_INFO):
        sens = await text_sensor.new_text_sensor(conf)
        cg.add(var.set_board_info_text_sensor(sens))
