#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
/*********************
* TVout32 - the drawing half of the AVR TVout library, retargeted at the ESP32
* CompositeNTSC framebuffer.
*
* Signal generation is gone (that was all AVR assembly); the primitives, the
* bitmap format and the method names are kept so TvOut_FGF sketch code ports
* across essentially unchanged.
*
* The framebuffer is 4bpp grey; 1bpp art (logos.cpp, .tvb) draws as black and
* white, .tva files can carry 2bpp or 4bpp grey.
*
* Coordinates are 1:1 with the framebuffer: 320x240, the practical ceiling for
* NTSC. 320 pixels across the 48us active window puts an alternating pattern at
* 3.3MHz, right at the 4.2MHz luma limit of the standard -- going wider would
* only blur.
*
* BITMAP FORMAT
*   classic   { w, h, rows... }              w,h <= 255, as logos.cpp uses today
*   extended  { 0, wLo, wHi, hLo, hHi, rows... }   for anything wider than 255
*
* A width of 0 is meaningless, so a leading 0 byte is an unambiguous marker and
* both forms can sit side by side. Rows are packed 1bpp, ceil(w/8) bytes each,
* MSB = leftmost pixel.
**********************/
#pragma once
#include <Arduino.h>
#include "CompositeNTSC.h"

#define ZOOM    1
#define LOG_W   FB_W       // 320
#define LOG_H   FB_H       // 240

/**TVout colour constants**/
#define BLACK   0
#define WHITE   1
#define INVERT  2

/**Accepted for source compatibility; only NTSC is generated.**/
#define NTSC    0
#define PAL     1

/**The C64 8x8 set extracted from the ESP8266 project.**/
#define FONT_W  8
#define FONT_H  8

class TVout32 : public Print {
  public:
    struct BmpInfo { int16_t w, h; uint16_t dataOff; };

    /**Read a bitmap's header, whichever of the two forms it uses.**/
    static BmpInfo info(const unsigned char *bmp);

    bool begin(uint8_t mode = NTSC, int16_t w = LOG_W, int16_t h = LOG_H);

    int16_t hres()      { return LOG_W; }
    int16_t vres()      { return LOG_H; }
    int16_t char_line() { return LOG_W / (FONT_W * tsize); }

    void clear_screen(uint8_t colour = BLACK);
    void set_pixel(int16_t x, int16_t y, uint8_t colour);     // BLACK/WHITE/INVERT
    uint8_t get_pixel(int16_t x, int16_t y);                  // WHITE if grey >= 8

    /**Grey levels, 0 = black ... 15 = white.**/
    void set_grey(int16_t x, int16_t y, uint8_t level);
    uint8_t get_grey(int16_t x, int16_t y);

    void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t colour);
    void draw_row(int16_t line, int16_t x0, int16_t x1, uint8_t colour);
    void draw_column(int16_t col, int16_t y0, int16_t y1, uint8_t colour);
    void draw_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                   uint8_t colour, int8_t fill = -1);
    void draw_circle(int16_t x0, int16_t y0, int16_t r,
                     uint8_t colour, int8_t fill = -1);

    /**Full bitmap, or a window of one. `offset` is an absolute byte index into
     * bmp (header included), `wCrop`/`lines` limit how much is drawn -- the
     * signature intro() relies on for its scroll-in. 0 means "everything".**/
    void bitmap(int16_t x, int16_t y, const unsigned char *bmp,
                uint16_t offset = 0, int16_t wCrop = 0, int16_t lines = 0);

    /**Raw rows (no header) at 1, 2 or 4 bpp, `stride` bytes apart, MSB-first,
     * drawn opaque in a w x h box. 1bpp is black/white, 2bpp 4 greys, 4bpp
     * 16 greys. Table-driven, so a full-screen frame takes well under a
     * millisecond -- fast enough for the vblank window.**/
    void blit(int16_t x, int16_t y, int16_t w, int16_t h,
              const uint8_t *bits, uint16_t stride, uint8_t bpp = 1);

    /**Text. At 320x240 the 8x8 glyphs give 40x30 at size 1, which is past what
     * a CRT resolves comfortably -- size 2 (20x15) is the default for that
     * reason, matching the apparent size of the old AVR build.**/
    void select_font(const unsigned char *f) { (void)f; }   // one font on ESP32
    void set_text_size(uint8_t n) { tsize = n ? n : 1; }
    uint8_t text_size() const { return tsize; }
    void set_cursor(int16_t x, int16_t y) { cx = x; cy = y; }
    virtual size_t write(uint8_t c);

    /**Timing. delay() yields to FreeRTOS instead of spinning.**/
    void delay(uint16_t ms)     { vTaskDelay(pdMS_TO_TICKS(ms)); }
    void delay_frame(uint8_t n) { Composite.waitFrames(n); }

  private:
    int16_t cx = 0, cy = 0;
    uint8_t tsize = 2;
    void drawChar(int16_t x, int16_t y, uint8_t c);
    void scroll();
};

extern TVout32 TV;

#endif  // TVOUT_ENGINE
