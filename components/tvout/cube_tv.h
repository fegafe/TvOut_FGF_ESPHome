#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
#pragma once
#include "TVout32.h"

void draw_cube(TVout32 &tv);
void rotate_axis(float q, char axis);
void printcube(TVout32 &tv);
void rotate_cube(TVout32 &tv);

#endif  // TVOUT_ENGINE
