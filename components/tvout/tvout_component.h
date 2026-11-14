#pragma once
/*********************
* ESPHome face of the TvOut_FGF ESP32 engine.
*
* ESPHome #includes EVERY header of a component into its generated main.cpp.
* The engine's headers define macros (BLACK, WHITE, NTSC, ...) that collide
* with ESPHome's own names, so each one is wrapped in #ifdef TVOUT_ENGINE and
* only the engine's .cpp files, which #define it first, see their contents.
* This header is the only public one: it must not include an engine header.
*
* Threads: ESPHome's loop task calls everything here; the engine's app task
* (core 1) draws, the video task (core 1) generates NTSC, the web and sound
* tasks run on core 0.
**********************/
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include <string>

namespace esphome {
namespace tvout {

class TvOutComponent : public Component {
 public:
  void set_web_port(uint16_t port) { this->web_port_ = port; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /**Mode by name (intro show message dvd butterfly cube game birthday) or by
   * the old one-letter code (a i m d b c g 3). False if unknown.**/
  bool set_mode(const std::string &mode);
  std::string get_mode() const;
  std::string get_mode_letter() const;

  /**Text centred on screen; switches to mode message.**/
  void show_message(const std::string &text);
  std::string get_message() const;

  /**LittleFS image for intro/show/dvd. show_image() also switches to show.**/
  void set_image(const std::string &name);
  void show_image(const std::string &name);
  std::string get_image() const;

  /**dvd mode: true = leave a trail, false = clear every frame.**/
  void set_trail(bool on);
  bool get_trail() const;

  /**birthday | beep | startup | off. False if unknown.**/
  bool play_sound(const std::string &name);

  /**Images in LittleFS, sorted and comma separated, trimmed to fit Home
   * Assistant's 255-character state limit ("a,b,c +12 more"). Re-read only
   * after a file is saved or deleted, so it is cheap to poll.**/
  std::string get_image_list();
  int get_image_count();

  /**Measured video field rate (~60.0 when healthy), 0 until measured.**/
  float get_field_rate() const;

  /**Call from ota: on_begin -- puts "UPDATING..." on the TV and stops sound.**/
  void ota_begin();
  void ota_error();   // 2026-11-14: reanuda el video si la OTA falla

  void add_on_mode_callback(std::function<void(std::string)> &&cb) { this->mode_cb_.add(std::move(cb)); }

 protected:
  uint16_t web_port_{80};
  char last_mode_{0};
  bool rate_logged_{false};
  uint32_t list_gen_{0};          // imgstore_generation() the list was read at
  std::string image_list_;
  int image_count_{0};
  void refresh_image_list_();
  void wifi_portal_loop_();       // fallback hotspot: WiFi setup page <-> ESPHome wifi
  bool ap_was_active_{false};
  uint32_t last_scan_push_{0};
  CallbackManager<void(std::string)> mode_cb_;
};

/**on_mode: fires with the mode name whenever the displayed mode changes,
 * whoever changed it (Home Assistant, MQTT, the web page, intro ending).**/
class ModeTrigger : public Trigger<std::string> {
 public:
  explicit ModeTrigger(TvOutComponent *parent) {
    parent->add_on_mode_callback([this](std::string mode) { this->trigger(mode); });
  }
};

}  // namespace tvout
}  // namespace esphome
