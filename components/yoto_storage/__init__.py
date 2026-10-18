import esphome.codegen as cg
import esphome.final_validate as fv
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.components import sensor, socket
from esphome.components.esp32 import (
    add_idf_sdkconfig_option,
    include_builtin_idf_component,
    require_vfs_dir,
)
from esphome.core import CORE
from esphome.const import (
    CONF_ID,
    CONF_PORT,
    CONF_TRIGGER_ID,
    DEVICE_CLASS_DATA_SIZE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)

DEPENDENCIES = ["esp32", "network"]
AUTO_LOAD = ["sensor", "socket"]
MULTI_CONF = False

CONF_CLK_PIN = "clk_pin"
CONF_CMD_PIN = "cmd_pin"
CONF_DATA0_PIN = "data0_pin"
CONF_DATA1_PIN = "data1_pin"
CONF_DATA2_PIN = "data2_pin"
CONF_DATA3_PIN = "data3_pin"
CONF_TOTAL_SPACE = "total_space"
CONF_FREE_SPACE = "free_space"
CONF_ON_PLAY_REQUEST = "on_play_request"
CONF_FREQUENCY_KHZ = "frequency_khz"
CONF_ADVERTISE_WEB_UI = "advertise_web_ui"
CONF_STREAM_PORT = "stream_port"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"

yoto_storage_ns = cg.esphome_ns.namespace("yoto_storage")
YotoStorage = yoto_storage_ns.class_("YotoStorage", cg.PollingComponent)
PlayRequestTrigger = yoto_storage_ns.class_(
    "PlayRequestTrigger", automation.Trigger.template(cg.std_string)
)

_SIZE_SENSOR = sensor.sensor_schema(
    unit_of_measurement="MB",
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_DATA_SIZE,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    icon="mdi:micro-sd",
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(YotoStorage),
            cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_CMD_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_DATA0_PIN): pins.internal_gpio_pin_number,
            cv.Required(CONF_DATA1_PIN): pins.internal_gpio_pin_number,
            cv.Required(CONF_DATA2_PIN): pins.internal_gpio_pin_number,
            cv.Required(CONF_DATA3_PIN): pins.internal_gpio_pin_number,
            cv.Optional(CONF_FREQUENCY_KHZ, default=20000): cv.int_range(400, 40000),
            cv.Optional(CONF_PORT, default=8080): cv.port,
            # Separate server for audio streaming to the player, so a long
            # stream never blocks the web page, uploads or cover art
            cv.Optional(CONF_STREAM_PORT, default=8081): cv.port,
            # Login for the admin page / file manager on `port` (both empty = no login)
            cv.Optional(CONF_USERNAME, default=""): cv.string,
            cv.Optional(CONF_PASSWORD, default=""): cv.string,
            # Report the file manager as the device's web UI, so Home Assistant's
            # "Visit" link opens it. The mDNS slot it needs is reserved below.
            cv.Optional(CONF_ADVERTISE_WEB_UI, default=False): cv.boolean,
            cv.Optional(CONF_TOTAL_SPACE): _SIZE_SENSOR,
            cv.Optional(CONF_FREE_SPACE): _SIZE_SENSOR,
            cv.Optional(CONF_ON_PLAY_REQUEST): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PlayRequestTrigger)}
            ),
        }
    ).extend(cv.polling_component_schema("60s")),
    cv.only_on_esp32,
    # File server: 1 listening socket + up to 3 open connections; the media
    # source streams files over a loopback HTTP connection (1 more).
    socket.consume_sockets(1, "yoto_storage_http", socket.SocketType.TCP_LISTEN),
    socket.consume_sockets(4, "yoto_storage_http"),
    socket.consume_sockets(1, "yoto_storage_stream", socket.SocketType.TCP_LISTEN),
    socket.consume_sockets(3, "yoto_storage_stream"),
)


def _reserve_mdns_web_slot(config):
    """With advertise_web_ui, ESPHome's mDNS publishes an _http service for the
    "web server" (USE_WEBSERVER) but only reserves slots for components it knows
    (api, sendspin, web_server, ...). The unreserved service then overwrote the
    last slot - Sendspin - so Music Assistant lost the player. Counting this
    component as one with an mDNS service reserves the extra slot."""
    if config.get(CONF_ADVERTISE_WEB_UI) and "web_server" not in fv.full_config.get():
        from esphome.components import mdns

        if "yoto_storage" not in mdns.COMPONENTS_WITH_MDNS_SERVICES:
            mdns.COMPONENTS_WITH_MDNS_SERVICES = (*mdns.COMPONENTS_WITH_MDNS_SERVICES, "yoto_storage")
    return config


FINAL_VALIDATE_SCHEMA = _reserve_mdns_web_slot


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_pins(
        config[CONF_CLK_PIN], config[CONF_CMD_PIN], config[CONF_DATA0_PIN],
        config[CONF_DATA1_PIN], config[CONF_DATA2_PIN], config[CONF_DATA3_PIN],
    ))
    cg.add(var.set_frequency_khz(config[CONF_FREQUENCY_KHZ]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_stream_port(config[CONF_STREAM_PORT]))
    cg.add(var.set_auth(config[CONF_USERNAME], config[CONF_PASSWORD]))

    if conf := config.get(CONF_TOTAL_SPACE):
        cg.add(var.set_total_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_FREE_SPACE):
        cg.add(var.set_free_sensor(await sensor.new_sensor(conf)))

    for conf in config.get(CONF_ON_PLAY_REQUEST, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_play_request_trigger(trigger))
        await automation.build_automation(trigger, [(cg.std_string, "x")], conf)

    if config[CONF_ADVERTISE_WEB_UI] and "web_server" not in CORE.config:
        cg.add_define("USE_WEBSERVER")
        cg.add_define("USE_WEBSERVER_PORT", config[CONF_PORT])

    # opendir/readdir/mkdir/rmdir: ESPHome disables VFS directory support unless asked
    require_vfs_dir()
    # ESP-IDF pieces ESPHome leaves out by default
    for comp in ("fatfs", "sdmmc", "esp_driver_sdmmc", "wear_levelling", "esp_http_server"):
        include_builtin_idf_component(comp)
    # Long, UTF-8 file names (book and chapter titles)
    add_idf_sdkconfig_option("CONFIG_FATFS_LFN_HEAP", True)
    add_idf_sdkconfig_option("CONFIG_FATFS_MAX_LFN", 255)
    add_idf_sdkconfig_option("CONFIG_FATFS_API_ENCODING_UTF_8", True)
    # The media source talks to the file server over 127.0.0.1
    add_idf_sdkconfig_option("CONFIG_LWIP_NETIF_LOOPBACK", True)
