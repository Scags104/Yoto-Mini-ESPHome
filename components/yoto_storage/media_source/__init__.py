import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import audio, media_source, psram
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID, CONF_TASK_STACK_IN_PSRAM
from esphome.types import ConfigType

from .. import YotoStorage, yoto_storage_ns

AUTO_LOAD = ["audio"]
DEPENDENCIES = ["yoto_storage"]

CONF_STORAGE_ID = "storage_id"
CONF_HANDLE_HTTP = "handle_http"

StorageMediaSource = yoto_storage_ns.class_(
    "StorageMediaSource", cg.Component, media_source.MediaSource
)


def _request_codecs(config: ConfigType) -> ConfigType:
    audio.request_micro_decoder_support()
    audio.request_mp3_support()
    audio.request_flac_support()
    audio.request_opus_support()
    audio.request_wav_support()
    return config


CONFIG_SCHEMA = cv.All(
    media_source.media_source_schema(StorageMediaSource)
    .extend(
        {
            cv.GenerateID(CONF_STORAGE_ID): cv.use_id(YotoStorage),
            # Also play plain http(s):// URLs (e.g. media sent from Home Assistant)
            cv.Optional(CONF_HANDLE_HTTP, default=True): cv.boolean,
            cv.Optional(CONF_TASK_STACK_IN_PSRAM): psram.validate_task_stack_in_psram,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _request_codecs,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)

    storage = await cg.get_variable(config[CONF_STORAGE_ID])
    cg.add(var.set_storage(storage))
    cg.add(var.set_handle_http(config[CONF_HANDLE_HTTP]))

    if config.get(CONF_TASK_STACK_IN_PSRAM):
        cg.add(var.set_task_stack_in_psram(True))
        psram.request_external_task_stack()

    # micro-decoder streams over HTTP (here: the device's own file server)
    for comp in ("esp_http_client", "tcp_transport", "esp-tls"):
        include_builtin_idf_component(comp)
