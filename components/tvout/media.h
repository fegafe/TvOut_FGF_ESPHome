#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
/*********************
* media - "give me the picture called X, right now", for stills and animations.
*
* Every mode draws through media_frame(), so any name can be a still (.tvb) or
* an animation (.tva) -- replace logo_princ with a GIF and modes 1/5/intro
* animate with no other change.
*
* Animations advance by wall clock against their own per-frame delays. One is
* open at a time: if the whole file fits in ANIM_RAM_MAX it is loaded once,
* otherwise its file stays open and frames are read in order as they come due
* (~8ms for a 240x240 frame). imgstore wraps every delete/rename of an image
* file in media_files_lock()/unlock(), which closes that handle first.
*
* Loop task ONLY, like imgstore_get().
**********************/
#pragma once
#include <Arduino.h>

#define ANIM_RAM_MAX  (96UL * 1024UL)

struct MediaFrame {
  int16_t  w, h;
  uint16_t stride;          // bytes per row
  const uint8_t *bits;      // first row, MSB = leftmost
  uint8_t  bpp;             // 1, 2 or 4 -- pass straight to TV.blit()
  bool     anim;            // true for a .tva
  bool     changed;         // a different frame/image than the last call returned
};

/**Current frame of `name`; false if no such still or animation.**/
bool media_frame(const char *name, MediaFrame &f);

/**Any task about to remove/rename an image file: lock (closes the player's
 * open file), change the files, unlock. The player reopens on demand.**/
void media_files_lock();
void media_files_unlock();

/**Call after clearing the screen: the next media_frame() reports changed, and
 * an open animation restarts from frame 0.**/
void media_rewind();
/**2026-11-14: suelta la animacion en RAM (hasta ~96 KB) al entrar a un modo sin imagenes.**/
void media_release();

#endif  // TVOUT_ENGINE
