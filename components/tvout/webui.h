#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
/*********************
* webui - HTTP upload page on port 80.
*
* The browser does all the image work: scale to fit 320x240 (then down by
* the size slider), greyscale,
* Floyd-Steinberg dither (or plain threshold), pack to 1bpp and base64 the
* result. The ESP32 only decodes base64 and writes the bytes to LittleFS, so
* there is no JPEG/PNG decoder on the MCU.
*
* Runs in its own task on core 0, next to WiFi. Core 1 stays on video. ESPHome
* owns WiFi, OTA and the API; this page is kept for image uploads and the game.
*
* Endpoints:
*   GET  /                  the page
*   GET  /list              JSON of stored images
*   POST /upload?name=X     body is base64 of {w,h,rows...}; an existing name
*                           is overwritten -- that is how "replace" works
*   POST /anim?name=X[&pos=P] .tva upload (animations, grey images), one piece
*                           per request: no pos = header, pos=P = <=9600 bytes
*                           of frame data at offset P (base64 bodies)
*   GET  /del?name=X
*   GET  /show?name=X       display it now
*   GET  /dvd?name=X        bounce it like a DVD logo (mode d)
*   GET  /trail[?on=0|1]    DVD trail flag: read it, or set it (0 = clear each frame)
*   GET  /mode?p=C[&name=X] mode letter (a i m d b c); name = image for a/i/d
*   GET  /msg?t=TEXT        show TEXT centred (mode m)
*   GET  /game              the game controller page (starts mode g)
*   GET  /key?k=MASK        held game keys (game.h GK_*), returns game status
*   GET  /shot.pgm          screenshot of the TV picture (320x240, 16 greys)
*   GET|POST /level?n=N[&del=1]  game level file /lvl/N.txt: read, store (body), delete
*   GET  /gstat             "hp,ammo,kills,enemies,level,fps,state"
*   GET  /state             {"p","img","msg","trail"} -- what the TV is doing
*   GET  /snd?name=X        birthday | beep | startup | off
*   GET  /wifi              WiFi setup page (SSID + password)
*   GET  /wifiscan          JSON of networks seen by the last scan
*   POST /wifisave          ssid=..&psk=.. -- saved by ESPHome, then it connects
*
* While the fallback hotspot is up this also answers every DNS query with the
* board's IP and redirects unknown URLs (and /) to /wifi, so phones pop the
* setup page up as a captive portal.
**********************/
#pragma once
#include <string>

/**Start the page on `port` (80 unless the YAML says otherwise).**/
void webui_begin(uint16_t port = 80);

/**Called from ESPHome's loop (tvout_component.cpp).**/
void webui_set_ap(bool active, const char *ip);         // hotspot up/down
void webui_set_networks(const std::string &json);       // [{"ssid","rssi","lock"}]
bool webui_take_wifi(char ssid[33], char psk[65]);      // true once per page submit

#endif  // TVOUT_ENGINE
