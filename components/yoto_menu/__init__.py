"""On-device menu for the Yoto Mini.

Header-only: ESPHome adds components/yoto_menu/yoto_menu.h to esphome.h, so the
global `yoto_menu::menu` object is available in YAML lambdas. The menu's hooks are
bound in the device YAML (esphome: on_boot).
"""

import esphome.config_validation as cv

DEPENDENCIES = ["display", "yoto_storage"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
