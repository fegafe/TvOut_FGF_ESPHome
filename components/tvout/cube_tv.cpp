#define TVOUT_ENGINE
#include <Arduino.h>
#include "cube_tv.h"

/*** Variables for Cube ***/
float zOff  = 150;
float xOff  = 0;
float yOff  = 0;
float cSize = 50;
int view_plane = 170;         // was 64 on the 120x96 AVR screen; scaled for 320x240
float angle    = PI / 60;

float cube3d[8][3] = {
  {xOff - cSize, yOff + cSize, zOff - cSize},
  {xOff + cSize, yOff + cSize, zOff - cSize},
  {xOff - cSize, yOff - cSize, zOff - cSize},
  {xOff + cSize, yOff - cSize, zOff - cSize},
  {xOff - cSize, yOff + cSize, zOff + cSize},
  {xOff + cSize, yOff + cSize, zOff + cSize},
  {xOff - cSize, yOff - cSize, zOff + cSize},
  {xOff + cSize, yOff - cSize, zOff + cSize}
};
/**int16_t rather than the AVR's unsigned char: the projection can land off
 * screen and TVout32 clips, whereas a byte would wrap around.**/
int16_t cube2d[8][2];

void rotate_cube(TVout32 &tv) {
  int rsteps = random(10, 60);
  char axis[4] = "xyz";
  int r_ang = random(2);
  char r_axis = axis[random(3)];
  for (int i = 0; i < rsteps; i++) {
    rotate_axis(((r_ang) * angle + (1 - r_ang) * (2 * PI - angle)), r_axis);
    printcube(tv);
  }
}

void printcube(TVout32 &tv) {
  for (byte i = 0; i < 8; i++) {
    cube2d[i][0] = (int16_t)((cube3d[i][0] * view_plane / cube3d[i][2]) + (tv.hres() / 2));
    cube2d[i][1] = (int16_t)((cube3d[i][1] * view_plane / cube3d[i][2]) + (tv.vres() / 2));
  }
  tv.delay_frame(1);
  tv.clear_screen();
  draw_cube(tv);
}

void rotate_axis(float q, char axis) {
  float tx, ty, tz, temp_x = 0, temp_y = 0, temp_z = 0;
  for (byte i = 0; i < 8; i++) {
    tx = cube3d[i][0] - xOff;
    ty = cube3d[i][1] - yOff;
    tz = cube3d[i][2] - zOff;
    if (axis == 'z') {
      temp_x = tx * cos(q) - ty * sin(q);
      temp_y = tx * sin(q) + ty * cos(q);
      temp_z = tz;
    }
    if (axis == 'x') {
      temp_y = ty * cos(q) - tz * sin(q);
      temp_z = ty * sin(q) + tz * cos(q);
      temp_x = tx;
    }
    if (axis == 'y') {
      temp_z = tz * cos(q) - tx * sin(q);
      temp_x = tz * sin(q) + tx * cos(q);
      temp_y = ty;
    }
    cube3d[i][0] = temp_x + xOff;
    cube3d[i][1] = temp_y + yOff;
    cube3d[i][2] = temp_z + zOff;
  }
}

void draw_cube(TVout32 &tv) {
  tv.draw_line(cube2d[0][0], cube2d[0][1], cube2d[1][0], cube2d[1][1], WHITE);
  tv.draw_line(cube2d[0][0], cube2d[0][1], cube2d[2][0], cube2d[2][1], WHITE);
  tv.draw_line(cube2d[0][0], cube2d[0][1], cube2d[4][0], cube2d[4][1], WHITE);
  tv.draw_line(cube2d[1][0], cube2d[1][1], cube2d[5][0], cube2d[5][1], WHITE);
  tv.draw_line(cube2d[1][0], cube2d[1][1], cube2d[3][0], cube2d[3][1], WHITE);
  tv.draw_line(cube2d[2][0], cube2d[2][1], cube2d[6][0], cube2d[6][1], WHITE);
  tv.draw_line(cube2d[2][0], cube2d[2][1], cube2d[3][0], cube2d[3][1], WHITE);
  tv.draw_line(cube2d[4][0], cube2d[4][1], cube2d[6][0], cube2d[6][1], WHITE);
  tv.draw_line(cube2d[4][0], cube2d[4][1], cube2d[5][0], cube2d[5][1], WHITE);
  tv.draw_line(cube2d[7][0], cube2d[7][1], cube2d[6][0], cube2d[6][1], WHITE);
  tv.draw_line(cube2d[7][0], cube2d[7][1], cube2d[3][0], cube2d[3][1], WHITE);
  tv.draw_line(cube2d[7][0], cube2d[7][1], cube2d[5][0], cube2d[5][1], WHITE);
}
