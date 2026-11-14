#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
#include <pgmspace.h>
#ifndef LOGOS_H
#define LOGOS_H

/**Only the butterfly frames stay in flash. Every other logo is a LittleFS
 * file (/img/<name>.tvb) -- originals in data/img, see tools/push_logos.py.**/

extern const unsigned char butterfly0[];
extern const unsigned char butterfly1[];
extern const unsigned char butterfly2[];
extern const unsigned char butterfly3[];
extern const unsigned char butterfly4[];
extern const unsigned char butterfly5[];
extern const unsigned char butterfly0_inv[];
extern const unsigned char butterfly1_inv[];
extern const unsigned char butterfly2_inv[];
extern const unsigned char butterfly3_inv[];
extern const unsigned char butterfly4_inv[];
extern const unsigned char butterfly5_inv[];

#endif
#endif  // TVOUT_ENGINE
