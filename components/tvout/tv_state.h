#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
/*********************
* tv_state - what the TV should be doing. Replaces mqtt_ctrl.h of the Arduino
* build: ESPHome now owns WiFi/MQTT/API, and its entities (tvout_component),
* the web UI and the app task all meet here.
*
* Writers: ESPHome's loop task (entities, MQTT on_message) and the web task.
* Reader:  the app task (app.cpp), which owns all drawing.
**********************/
#pragma once
#include <Arduino.h>

/**Program selector letter: a i m d b c g 3 (see app.cpp).**/
extern volatile char prog;

/**Text for mode 'm'.**/
extern char msgbuf[64];
extern volatile bool msg_dirty;

/**LittleFS image for modes a (intro), i (show) and d (dvd). Defaults to
 * logo_princ.**/
extern char img_name[32];
extern volatile bool img_dirty;

/**asdvd(): true = leave a trail, false = clear the screen every frame.**/
extern volatile bool dvd_trail;

/**Measured video field rate, set once ~2s after boot. Should read ~60.0;
 * anything else means the line period is off and the TV will roll.**/
extern volatile float field_rate;

/**Start the app task (boot intro, then the mode loop) on core 1.**/
void app_start();

#endif  // TVOUT_ENGINE
