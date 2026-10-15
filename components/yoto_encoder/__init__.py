import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = []
MULTI_CONF = True

CONF_PIN_A = "pin_a"
CONF_PIN_B = "pin_b"
CONF_INVERT = "invert"
CONF_GLITCH_FILTER = "glitch_filter"
CONF_COUNTS_PER_DETENT = "counts_per_detent"
CONF_MAX_STEPS_PER_LOOP = "max_steps_per_loop"
CONF_ON_CLOCKWISE = "on_clockwise"
CONF_ON_ANTICLOCKWISE = "on_anticlockwise"

yoto_encoder_ns = cg.esphome_ns.namespace("yoto_encoder")
YotoEncoder = yoto_encoder_ns.class_("YotoEncoder", cg.Component)
StepTrigger = yoto_encoder_ns.class_("StepTrigger", automation.Trigger.template())

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(YotoEncoder),
            cv.Required(CONF_PIN_A): pins.internal_gpio_input_pin_number,
            cv.Required(CONF_PIN_B): pins.internal_gpio_input_pin_number,
            # Swap the turn direction without swapping wires/pins
            cv.Optional(CONF_INVERT, default=False): cv.boolean,
            # Hardware filter: pulses shorter than this are ignored (max ~12.7 us)
            cv.Optional(CONF_GLITCH_FILTER, default="10us"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(max=cv.TimePeriod(microseconds=12)),
            ),
            # Quadrature edges per click (Yoto knobs: 4)
            cv.Optional(CONF_COUNTS_PER_DETENT, default=4): cv.int_range(min=1, max=8),
            # Fast spins: at most this many step events per main-loop pass
            cv.Optional(CONF_MAX_STEPS_PER_LOOP, default=3): cv.int_range(min=1, max=20),
            cv.Optional(CONF_ON_CLOCKWISE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StepTrigger)}
            ),
            cv.Optional(CONF_ON_ANTICLOCKWISE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StepTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pins(config[CONF_PIN_A], config[CONF_PIN_B]))
    cg.add(var.set_invert(config[CONF_INVERT]))
    cg.add(var.set_glitch_filter_ns(config[CONF_GLITCH_FILTER].total_microseconds * 1000))
    cg.add(var.set_counts_per_detent(config[CONF_COUNTS_PER_DETENT]))
    cg.add(var.set_max_steps_per_loop(config[CONF_MAX_STEPS_PER_LOOP]))

    for conf in config.get(CONF_ON_CLOCKWISE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_clockwise_trigger(trigger))
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_ANTICLOCKWISE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_anticlockwise_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    include_builtin_idf_component("esp_driver_pcnt")
