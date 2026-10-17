import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, i2c, sensor, text_sensor
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_BATTERY_VOLTAGE,
    CONF_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_PLUG,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
    UNIT_VOLT,
)

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor"]

CONF_CHARGER_ADDRESS = "charger_address"
CONF_CONFIGURE_CHARGER = "configure_charger"
CONF_BATTERY_PROFILE = "battery_profile"
CONF_CHARGE_STATE = "charge_state"
CONF_CHARGING = "charging"
CONF_EXTERNAL_POWER = "external_power"

PROFILE_SIZE = 80

yoto_battery_ns = cg.esphome_ns.namespace("yoto_battery")
YotoBattery = yoto_battery_ns.class_(
    "YotoBattery", cg.PollingComponent, i2c.I2CDevice
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(YotoBattery),
            cv.Optional(CONF_CHARGER_ADDRESS, default=0x1A): cv.i2c_address,
            # Apply the stock rev #04 charger setup (from Open Yoto) whenever
            # USB power appears, and enable charging. False = status only.
            cv.Optional(CONF_CONFIGURE_CHARGER, default=False): cv.boolean,
            # Optional 80-byte CW2215B battery profile. Without it the component
            # never writes the gauge and relies on the profile the stock
            # firmware already loaded (the gauge keeps it while the battery is
            # connected).
            cv.Optional(CONF_BATTERY_PROFILE): cv.All(
                cv.ensure_list(cv.hex_uint8_t), cv.Length(min=PROFILE_SIZE, max=PROFILE_SIZE)
            ),
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_CHARGE_STATE): text_sensor.text_sensor_schema(
                icon="mdi:battery-charging",
            ),
            cv.Optional(CONF_CHARGING): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_BATTERY_CHARGING,
            ),
            cv.Optional(CONF_EXTERNAL_POWER): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PLUG,
            ),
        }
    )
    .extend(cv.polling_component_schema("30s"))
    .extend(i2c.i2c_device_schema(0x64))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_charger_address(config[CONF_CHARGER_ADDRESS]))
    cg.add(var.set_configure_charger(config[CONF_CONFIGURE_CHARGER]))

    if profile := config.get(CONF_BATTERY_PROFILE):
        cg.add(var.set_profile(profile))

    if conf := config.get(CONF_BATTERY_VOLTAGE):
        cg.add(var.set_voltage_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_BATTERY_LEVEL):
        cg.add(var.set_level_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_CHARGE_STATE):
        cg.add(var.set_charge_state_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_CHARGING):
        cg.add(var.set_charging_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_EXTERNAL_POWER):
        cg.add(var.set_external_power_sensor(await binary_sensor.new_binary_sensor(conf)))
