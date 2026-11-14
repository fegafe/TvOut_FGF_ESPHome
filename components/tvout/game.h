#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
/*********************
* game - a Wolfenstein/Doom-style raycaster on the composite TV (mode 'g').
*
* Renders a 3D view into a 160x112 grey buffer (16 levels), doubled to
* 320x224 on screen, plus a 16-line HUD drawn with TVout32's text. The copy
* to the screen starts right after a field ends and outruns the beam, so it
* does not tear without needing a second 38KB framebuffer. The game's own
* ~30KB (view + textures) is allocated only while it runs.
*
*   - textured walls with distance shading, dithered floor/ceiling gradients
*   - enemies that chase, shoot back, take damage and die
*   - health and ammo pickups, an exit wall, several levels
*   - controls from the /game web page (touch D-pad or keyboard)
*
* Textures and sprites are generated at start-up -- no assets to upload.
* Levels: two built in. /lvl/<n>.txt in LittleFS overrides level n or adds
* more (same ASCII format as the built-in ones, see game.cpp).
*
* Loop task only: game_frame() renders one frame per call.
**********************/
#pragma once
#include <Arduino.h>

/**Key bits, as sent by the web page in /key?k=<mask>.**/
#define GK_FWD      0x01
#define GK_BACK     0x02
#define GK_TURN_L   0x04
#define GK_TURN_R   0x08
#define GK_STRAFE_L 0x10
#define GK_STRAFE_R 0x20
#define GK_FIRE     0x40
#define GK_USE      0x80    // restart after death / continue

/**Held keys, written by the web task. Released automatically if the page
 * stops refreshing them (lost packet, closed tab).**/
void game_keys(uint16_t mask);

/**One frame: input, simulation, render, present. Starts the game on the
 * first call after game_end().**/
void game_frame();

/**Leave the game: back to single-buffered drawing.**/
void game_end();

/**"hp,ammo,kills,enemies,level,fps" for the web page.**/
String game_status();

#endif  // TVOUT_ENGINE
