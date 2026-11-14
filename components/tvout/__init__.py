"""TvOut FGF -- NTSC composite video out on the classic ESP32's DAC (GPIO25).

The engine is the TvOut_FGF_ESP32 Arduino sketch, unchanged apart from
networking: ESPHome now owns WiFi, OTA, the API and MQTT. See README.md.
"""
from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32, wifi
from esphome.components.esp32 import get_esp32_variant
from esphome.components.esp32.const import VARIANT_ESP32
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID

DEPENDENCIES = ["wifi"]

CONF_WEB_PORT = "web_port"
CONF_ON_MODE = "on_mode"

tvout_ns = cg.esphome_ns.namespace("tvout")
TvOutComponent = tvout_ns.class_("TvOutComponent", cg.Component)
ModeTrigger = tvout_ns.class_(
    "ModeTrigger", automation.Trigger.template(cg.std_string)
)


def _classic_esp32_only(config):
    if get_esp32_variant() != VARIANT_ESP32:
        raise cv.Invalid(
            "tvout needs the classic ESP32: the video comes out of its built-in "
            "DAC on GPIO25, which the S3/C3/C6 do not have"
        )
    # LittleFS needs opendir/mkdir/rmdir, which ESPHome otherwise compiles out.
    # Must be flagged at validation: esp32's to_code reads it before ours runs.
    if hasattr(esp32, "require_vfs_dir"):
        esp32.require_vfs_dir()
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TvOutComponent),
            cv.Optional(CONF_WEB_PORT, default=80): cv.port,
            cv.Optional(CONF_ON_MODE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ModeTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_arduino,
    _classic_esp32_only,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_web_port(config[CONF_WEB_PORT]))

    for conf in config.get(CONF_ON_MODE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "x")], conf)

    # The WiFi setup page lists every network the scan saw, not just ours.
    wifi.request_wifi_scan_results()

    # Arduino core libraries the engine uses (image store + upload page)
    for lib in ("FS", "LittleFS", "Network", "WebServer"):
        cg.add_library(lib, None)
