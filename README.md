# TvOut_FGF_ESPHome

ESPHome build of [`TvOut_FGF_ESP32`](../TvOut_FGF_ESP32). The video engine is
the same code. ESPHome now takes care of WiFi, **OTA updates**, Home Assistant
and MQTT, so after the first USB flash you update the board over WiFi.

Hardware and wiring have not changed: classic ESP32 only. Video comes out on
**GPIO25** (180 Ω to the RCA centre pin) and sound on **GPIO32**. See the
Arduino project's README for the circuit and calibration notes.

## Layout

```
TvOut_FGF_ESPHome/
  tvout.yaml              the ESPHome config: WiFi, OTA, API, MQTT, entities
  secrets.yaml            your credentials (copied from the .ino; keep it private)
  secrets.yaml.example    template for a fresh checkout
  partitions.csv          2 OTA app slots + LittleFS
  components/tvout/       external component
    __init__.py             YAML schema (web_port, on_mode)
    tvout_component.*       ESPHome glue: entities call into the engine
    app.cpp                 the old .ino's setup/loop, run as a core-1 task
    tv_state.h              shared mode/message/image state (was mqtt_ctrl.h)
    CompositeNTSC.* TVout32.* imgstore.* media.* webui.* sound.* game.*
    cube_tv.* logos.* font8x8_c64.h      engine, copied from TvOut_FGF_ESP32
  data/img/*.tvb          stock logos
  tools/push_logos.py     puts the stock logos on the board
```

## What changed from the Arduino build

| | Arduino (`TvOut_FGF_ESP32`) | ESPHome (this folder) |
|---|---|---|
| WiFi / fixed IP | `#define`s in the `.ino` | `wifi:` in `tvout.yaml`, plus a fallback hotspot |
| Firmware update | USB only (partition scheme "No OTA") | **ESPHome OTA**, password protected, with `safe_mode` |
| Control | MQTT `tvout/*` | Home Assistant entities **and** the same MQTT topics |
| `loop()` | Arduino loop task | `tvapp` FreeRTOS task on core 1 (`app.cpp`) |
| Web page, uploads, game | port 80 | unchanged, port 80 (`web_port:`) |
| LittleFS | ~1.9 MB | 1.19 MB (OTA needs two app slots) |

Only the networking was replaced. `mqtt_ctrl.cpp` is gone, and the globals it
held (`prog`, `msgbuf`, `img_name`, ...) are now in `app.cpp`/`tv_state.h`.
Apart from that, the engine files differ from the Arduino ones in three ways:

- Each header is wrapped in `#ifdef TVOUT_ENGINE` and each `.cpp` defines it
  first. ESPHome includes **every** component header in its generated
  `main.cpp`, and the engine's `BLACK`/`WHITE`/`NTSC` macros would otherwise
  break ESPHome's own code (for example `Color::BLACK`).
- `webui_begin()` takes the port.
- `webui.cpp` includes `tv_state.h` instead of `mqtt_ctrl.h`.

## First flash (USB, once)

The partition table is different, so the first flash has to be over USB. The
Arduino build has no OTA, so it could not take this firmware over the air anyway.

```
pip install -U esphome                    # 2025.7 or newer (needs Arduino 3.x)
cd TvOut_FGF_ESPHome
esphome run tvout.yaml --device COM6      # your serial port
```

Run it from **PowerShell or cmd, not Git Bash**: ESP-IDF's tool installer
refuses to run under MSYS. The build goes to `C:/esphome-build/tvout-fgf`
(`build_path:` in the YAML) because OneDrive locks files in a synced
`.esphome/` folder and the build then fails with `PermissionError`.

**LittleFS is reformatted.** The filesystem moved, so images you uploaded to
the Arduino build are lost. Afterwards, put the stock logos back with:

```
python tools/push_logos.py 192.168.1.81
```

Then upload your own images again from `http://192.168.1.81/`.

## Updating over the air

```
esphome run tvout.yaml --device 192.168.1.81
```

Or press **Install → Wirelessly** in the ESPHome dashboard or the Home Assistant
add-on. To use the add-on, copy `tvout.yaml`, `partitions.csv` and
`components/` into its `/config/esphome/` folder, and put the secrets in its
`secrets.yaml`.

While an update runs, the TV shows **UPDATING...**. Flash writes stall both
cores, so the picture may roll or blank for a moment. If the update fails, the
board goes back to `dvd` mode. The OTA password is `ota_password` in
`secrets.yaml`.

If a bad build makes the board boot-loop, `safe_mode:` brings it up with only
WiFi and OTA, so you can always flash a fixed build over the air.

## Home Assistant

Add the device through the ESPHome integration, using `api_key` from
`secrets.yaml` as the encryption key.

| Entity | Type | Does |
|---|---|---|
| Mode | select | `intro show message dvd butterfly cube game birthday` |
| Message | text | shows the text centred (switches to `message`) |
| Image | text | LittleFS image name, shown centred (`show`) |
| DVD trail | switch | trail on/off for `dvd` |
| Play birthday / Beep / Sound off | buttons | sound on GPIO32 |
| Images | text sensor | images in LittleFS, sorted, comma separated; updates within 5 s of an upload/delete |
| Image count | sensor | how many images are stored |
| Video field rate | sensor | should read ~60.00 Hz; anything else and the TV rolls |
| WiFi signal, Uptime, IP address, Restart | diagnostics | |

Home Assistant states max out at 255 characters. If the names don't fit, **Images**
lists as many whole names as fit and ends with `+N more`; **Image count** is always
the full total.

**Mode** follows the TV whatever changed it: Home Assistant, MQTT, the web page,
or the boot intro ending in `dvd`.

## MQTT (optional)

The `mqtt:` block keeps the Arduino build's topics, so existing automations
keep working:

| Topic | Direction | Payload |
|---|---|---|
| `tvout/cmd` | in | mode letter (`a i m d b c g 3`) or mode name |
| `tvout/msg` | in | text, shown by mode `m` |
| `tvout/img` | in | LittleFS image name, shown (mode `i`) |
| `tvout/snd` | in | `birthday` \| `beep` \| `startup` \| `off` |
| `tvout/state` | out, retained | the mode letter currently running |

ESPHome's own topics go under `tvout/esphome/...`. Discovery is off, because
Home Assistant already gets the entities through the API. If you don't use
MQTT, delete the `mqtt:` block and the `mqtt.publish` step under `on_mode:`.

## Web page

`http://<board>/` works as before: image and GIF upload, modes, sound, `/game`,
`/shot.pgm`, `/level`. It is the engine's own server, not ESPHome's
`web_server`. If you add ESPHome's `web_server:`, give it another port
(`port: 8080`) or move this one with `tvout: web_port:`.

## Using it from YAML

`id(tv)` exposes:

```cpp
id(tv).set_mode("cube");          // or "c"
id(tv).show_message("FELIZ CUMPLE");
id(tv).show_image("guitar");
id(tv).set_image("guitar");       // for intro/show/dvd, without switching mode
id(tv).set_trail(false);
id(tv).play_sound("birthday");
id(tv).get_mode();  id(tv).get_mode_letter();  id(tv).get_image();
id(tv).get_message();  id(tv).get_trail();  id(tv).get_field_rate();
id(tv).get_image_list();  id(tv).get_image_count();
```

`on_mode:` fires with the mode name (`x`) each time it changes.

## Threads

| Task | Core | Prio | Job |
|---|---|---|---|
| `ntsc` | 1 | max | NTSC generator, blocks on I2S DMA |
| `tvapp` | 1 | 1 | the old `loop()`: all drawing |
| ESPHome loop | 1 | 1 | entities, API, MQTT, OTA |
| `web` | 0 | 1 | upload page / game controls |
| `snd` | 0 | 2 | melody player |
| WiFi / lwIP | 0 | | |

`tvapp` and ESPHome's loop have the same priority. A mode that never sleeps
(the cube) shares core 1 with ESPHome instead of starving it, and the video
task outranks both.

## Flash budget

| Partition | Size |
|---|---|
| app0 / app1 (OTA) | 1.375 MB each; the firmware is 1.17 MB (81%) |
| LittleFS (`spiffs`) | 1.19 MB, about 120 full-screen 1-bit images |

To get more image space, shrink both app slots by the same amount in
`partitions.csv` and grow `spiffs` by the difference. Keep enough headroom over
the firmware size that the build prints. If a future build outgrows the slots,
make them bigger and LittleFS smaller. A partition change needs a USB flash
and wipes LittleFS again.
