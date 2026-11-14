#include "tvout_component.h"
#include "esphome/core/log.h"
#include "esphome/components/wifi/wifi_component.h"
#include <algorithm>

/**ESPHome headers first: the engine's macros must not reach them.**/
#define TVOUT_ENGINE
#include "TVout32.h"
#include "tv_state.h"
#include "imgstore.h"
#include "sound.h"
#include "webui.h"

void tvout_ota_pause();   // CompositeNTSC.cpp (2026-11-14)
void tvout_ota_resume();

namespace esphome {
namespace tvout {

static const char *const TAG = "tvout";

struct ModeName {
  char letter;
  const char *name;
};
static const ModeName MODES[] = {
    {'a', "intro"},     {'i', "show"}, {'m', "message"}, {'d', "dvd"},
    {'b', "butterfly"}, {'c', "cube"}, {'g', "game"},    {'3', "birthday"}, {'w', "bano"},  // 2026-11-14
};

static const char *name_of(char letter) {
  for (const auto &m : MODES)
    if (m.letter == letter) return m.name;
  return "dvd";  // the app loop turns anything unknown into dvd
}

void TvOutComponent::setup() {
  if (!TV.begin(NTSC)) {
    ESP_LOGE(TAG, "Video init failed (I2S0 / DAC on GPIO25)");
    this->mark_failed();
    return;
  }
  randomSeed(esp_random());
  sound_begin();  // LEDC tone player, core 0
  if (!imgstore_begin()) ESP_LOGE(TAG, "LittleFS mount failed - check the partition table");
  webui_begin(this->web_port_);  // upload page + game, core 0
  app_start();                   // boot intro, then the mode loop, core 1
}

/**Fallback hotspot: tell the web task when the AP is up (it then answers DNS
 * and redirects to /wifi), feed it the scanned networks, and apply the SSID +
 * password the page posted. save_wifi_sta() writes flash and starts
 * connecting, so it must run here on ESPHome's loop, not on the web task.**/
void TvOutComponent::wifi_portal_loop_() {
  auto *w = wifi::global_wifi_component;
  if (w == nullptr) return;

  const bool ap = w->is_ap_active();
  if (ap != this->ap_was_active_) {
    this->ap_was_active_ = ap;
    char ip[network::IP_ADDRESS_BUFFER_SIZE] = "192.168.4.1";
    if (ap) w->wifi_soft_ap_ip().str_to(ip);
    webui_set_ap(ap, ip);
    if (ap) {
      ESP_LOGI(TAG, "Fallback hotspot up: WiFi setup page at http://%s/wifi", ip);
    }
  }

  const uint32_t now = millis();
  if (now - this->last_scan_push_ > 3000) {
    this->last_scan_push_ = now;
    std::string json = "[";
    for (auto &scan : w->get_scan_result()) {
      if (scan.get_is_hidden()) continue;
      if (json.size() > 1) json += ',';
      json += "{\"ssid\":\"";
      for (const char *c = scan.get_ssid().c_str(); *c; c++) {
        if (*c == '"' || *c == '\\') json += '\\';
        if ((unsigned char) *c >= 0x20) json += *c;
      }
      char tail[32];
      snprintf(tail, sizeof(tail), "\",\"rssi\":%d,\"lock\":%d}", scan.get_rssi(), scan.get_with_auth() ? 1 : 0);
      json += tail;
    }
    json += ']';
    webui_set_networks(json);
  }

  char ssid[33], psk[65];
  if (webui_take_wifi(ssid, psk)) {
    ESP_LOGI(TAG, "WiFi setup page: new network '%s'", ssid);
    w->save_wifi_sta(ssid, psk);
  }
}

void TvOutComponent::loop() {
  this->wifi_portal_loop_();
  const char p = prog;
  if (p != this->last_mode_) {
    this->last_mode_ = p;
    this->mode_cb_.call(name_of(p));
  }
  if (!this->rate_logged_ && field_rate > 0) {
    this->rate_logged_ = true;
    if (field_rate > 59.5f && field_rate < 60.5f)
      ESP_LOGI(TAG, "Video: %.2f fields/s", field_rate);
    else
      ESP_LOGW(TAG, "Video: %.2f fields/s - expected 60.0, the TV may roll", field_rate);
  }
}

void TvOutComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "TvOut FGF:");
  ESP_LOGCONFIG(TAG, "  Video: NTSC 240p, %dx%d, 16 greys, GPIO25 (DAC1)", FB_W, FB_H);
  ESP_LOGCONFIG(TAG, "  Sound: GPIO%d", SND_PIN);
  ESP_LOGCONFIG(TAG, "  Web UI port: %u", this->web_port_);
  ESP_LOGCONFIG(TAG, "  LittleFS: %u / %u bytes used", (unsigned) imgstore_used(), (unsigned) imgstore_total());
  if (this->is_failed()) ESP_LOGE(TAG, "  Video init FAILED");
}

bool TvOutComponent::set_mode(const std::string &mode) {
  for (const auto &m : MODES) {
    if (mode == m.name || (mode.size() == 1 && mode[0] == m.letter)) {
      prog = m.letter;
      return true;
    }
  }
  ESP_LOGW(TAG, "Unknown mode '%s'", mode.c_str());
  return false;
}

std::string TvOutComponent::get_mode() const { return name_of(prog); }
std::string TvOutComponent::get_mode_letter() const { return std::string(1, (char) prog); }

void TvOutComponent::show_message(const std::string &text) {
  strlcpy(msgbuf, text.c_str(), sizeof(msgbuf));
  msg_dirty = true;
  prog = 'm';
}
std::string TvOutComponent::get_message() const { return msgbuf; }

void TvOutComponent::set_image(const std::string &name) {
  if (name.empty() || name == img_name) return;
  strlcpy(img_name, name.c_str(), sizeof(img_name));
  img_dirty = true;
}
void TvOutComponent::show_image(const std::string &name) {
  this->set_image(name);
  prog = 'i';
}
std::string TvOutComponent::get_image() const { return img_name; }

void TvOutComponent::set_trail(bool on) { dvd_trail = on; }
bool TvOutComponent::get_trail() const { return dvd_trail; }

bool TvOutComponent::play_sound(const std::string &name) {
  if (sound_by_name(name.c_str())) return true;
  ESP_LOGW(TAG, "Unknown sound '%s'", name.c_str());
  return false;
}

void TvOutComponent::refresh_image_list_() {
  const uint32_t gen = imgstore_generation();
  if (gen == this->list_gen_) return;
  this->list_gen_ = gen;
  const std::string all = imgstore_names().c_str();
  this->image_count_ = all.empty() ? 0 : 1 + std::count(all.begin(), all.end(), ',');
  static const size_t MAX_STATE = 255;   // Home Assistant's state length limit
  if (all.size() <= MAX_STATE) {
    this->image_list_ = all;
    return;
  }
  // Keep whole names only, then say how many did not fit
  std::string out;
  int shown = 0;
  size_t start = 0;
  while (start < all.size()) {
    size_t end = all.find(',', start);
    if (end == std::string::npos) end = all.size();
    const std::string name = all.substr(start, end - start);
    const std::string tail = " +" + std::to_string(this->image_count_ - shown - 1) + " more";
    if (out.size() + (out.empty() ? 0 : 1) + name.size() + tail.size() > MAX_STATE) break;
    if (!out.empty()) out += ',';
    out += name;
    shown++;
    start = end + 1;
  }
  this->image_list_ = out + " +" + std::to_string(this->image_count_ - shown) + " more";
}

std::string TvOutComponent::get_image_list() {
  this->refresh_image_list_();
  return this->image_list_;
}

int TvOutComponent::get_image_count() {
  this->refresh_image_list_();
  return this->image_count_;
}

float TvOutComponent::get_field_rate() const { return field_rate; }

void TvOutComponent::ota_begin() {
  sound_stop();
  ESP_LOGW(TAG, "OTA: pausing video, modes, web page and sound");
  tvout_ota_pause();
}
void TvOutComponent::ota_error() {
  ESP_LOGW(TAG, "OTA failed: resuming video");
  tvout_ota_resume();
}

}  // namespace tvout
}  // namespace esphome
