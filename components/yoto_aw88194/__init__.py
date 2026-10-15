import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, switch
from esphome.const import CONF_ID
from esphome.core import CORE, HexInt

DEPENDENCIES = ["i2c"]

CONF_RESET_SWITCH = "reset_switch"
CONF_FIRMWARE = "firmware"
CONF_DSP_CONFIG = "dsp_config"
CONF_FIRMWARE_DATA_ID = "firmware_data_id"
CONF_DSP_CONFIG_DATA_ID = "dsp_config_data_id"

# Exact sizes of the stock Yoto rev #04 containers (Open Yoto)
FIRMWARE_SIZE = 0x7F4
DSP_CONFIG_SIZE = 0x39C

yoto_aw88194_ns = cg.esphome_ns.namespace("yoto_aw88194")
AW88194Component = yoto_aw88194_ns.class_(
    "AW88194Component", cg.Component, i2c.I2CDevice
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AW88194Component),
            cv.Required(CONF_RESET_SWITCH): cv.use_id(switch.Switch),
            cv.Required(CONF_FIRMWARE): cv.file_,
            cv.Required(CONF_DSP_CONFIG): cv.file_,
            cv.GenerateID(CONF_FIRMWARE_DATA_ID): cv.declare_id(cg.uint8),
            cv.GenerateID(CONF_DSP_CONFIG_DATA_ID): cv.declare_id(cg.uint8),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x34))
)


def _read_blob(path, expected_size, what):
    full = CORE.relative_config_path(path)
    data = full.read_bytes()
    if len(data) != expected_size:
        raise cv.Invalid(
            f"{what} '{path}' is {len(data)} bytes; expected {expected_size} "
            f"(0x{expected_size:X}) for the Yoto rev #04 AW88194"
        )
    return data


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    sw = await cg.get_variable(config[CONF_RESET_SWITCH])
    cg.add(var.set_reset_switch(sw))

    fw = _read_blob(config[CONF_FIRMWARE], FIRMWARE_SIZE, "firmware")
    fw_arr = cg.progmem_array(
        config[CONF_FIRMWARE_DATA_ID], [HexInt(x) for x in fw]
    )
    cg.add(var.set_firmware(fw_arr, len(fw)))

    cfg = _read_blob(config[CONF_DSP_CONFIG], DSP_CONFIG_SIZE, "dsp_config")
    cfg_arr = cg.progmem_array(
        config[CONF_DSP_CONFIG_DATA_ID], [HexInt(x) for x in cfg]
    )
    cg.add(var.set_dsp_config(cfg_arr, len(cfg)))
