#define TVOUT_ENGINE
#include "TVout32.h"
#include "font8x8_c64.h"

TVout32 TV;

bool TVout32::begin(uint8_t mode, int16_t w, int16_t h) {
  (void)mode; (void)w; (void)h;         // geometry is fixed by CompositeNTSC
  return Composite.begin();
}

/**A leading 0 byte marks the extended (16-bit) header.**/
TVout32::BmpInfo TVout32::info(const unsigned char *bmp) {
  BmpInfo b;
  if (pgm_read_byte(bmp) == 0) {
    b.w = (int16_t)(pgm_read_byte(bmp + 1) | (pgm_read_byte(bmp + 2) << 8));
    b.h = (int16_t)(pgm_read_byte(bmp + 3) | (pgm_read_byte(bmp + 4) << 8));
    b.dataOff = 5;
  } else {
    b.w = pgm_read_byte(bmp);
    b.h = pgm_read_byte(bmp + 1);
    b.dataOff = 2;
  }
  return b;
}

/**TVout colour -> grey level. BLACK/WHITE keep their TVout meaning.**/
static inline uint8_t levelOf(uint8_t colour) { return colour == WHITE ? 15 : 0; }

void TVout32::set_grey(int16_t x, int16_t y, uint8_t level) {
  if ((uint16_t)x >= LOG_W || (uint16_t)y >= LOG_H) return;
  uint8_t *p = Composite.frame() + y * FB_STRIDE + (x >> 1);
  if (x & 1) *p = (*p & 0xF0) | (level & 15);
  else       *p = (*p & 0x0F) | (uint8_t)(level << 4);
}

uint8_t TVout32::get_grey(int16_t x, int16_t y) {
  if ((uint16_t)x >= LOG_W || (uint16_t)y >= LOG_H) return 0;
  const uint8_t b = Composite.frame()[y * FB_STRIDE + (x >> 1)];
  return (x & 1) ? (b & 15) : (b >> 4);
}

void TVout32::set_pixel(int16_t x, int16_t y, uint8_t colour) {
  if (colour == INVERT) set_grey(x, y, 15 - get_grey(x, y));
  else                  set_grey(x, y, levelOf(colour));
}

uint8_t TVout32::get_pixel(int16_t x, int16_t y) {
  return get_grey(x, y) >= 8 ? WHITE : BLACK;
}

void TVout32::clear_screen(uint8_t colour) {
  memset(Composite.frame(), colour == WHITE ? 0xFF : 0x00, FB_SIZE);
  cx = cy = 0;
}

void TVout32::draw_row(int16_t line, int16_t x0, int16_t x1, uint8_t colour) {
  if (x0 > x1) { int16_t t = x0; x0 = x1; x1 = t; }
  for (int16_t x = x0; x <= x1; x++) set_pixel(x, line, colour);
}

void TVout32::draw_column(int16_t col, int16_t y0, int16_t y1, uint8_t colour) {
  if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; }
  for (int16_t y = y0; y <= y1; y++) set_pixel(col, y, colour);
}

void TVout32::draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t colour) {
  int16_t dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int16_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;
  for (;;) {
    set_pixel(x0, y0, colour);
    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void TVout32::draw_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                        uint8_t colour, int8_t fill) {
  if (fill >= 0) for (int16_t i = 0; i <= h; i++) draw_row(y + i, x, x + w, (uint8_t)fill);
  draw_row(y, x, x + w, colour);
  draw_row(y + h, x, x + w, colour);
  draw_column(x, y, y + h, colour);
  draw_column(x + w, y, y + h, colour);
}

void TVout32::draw_circle(int16_t x0, int16_t y0, int16_t r,
                          uint8_t colour, int8_t fill) {
  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
  if (fill >= 0) draw_column(x0, y0 - r, y0 + r, (uint8_t)fill);
  set_pixel(x0, y0 + r, colour);
  set_pixel(x0, y0 - r, colour);
  set_pixel(x0 + r, y0, colour);
  set_pixel(x0 - r, y0, colour);
  while (x < y) {
    if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
    x++; ddF_x += 2; f += ddF_x;
    if (fill >= 0) {
      draw_column(x0 + x, y0 - y, y0 + y, (uint8_t)fill);
      draw_column(x0 - x, y0 - y, y0 + y, (uint8_t)fill);
      draw_column(x0 + y, y0 - x, y0 + x, (uint8_t)fill);
      draw_column(x0 - y, y0 - x, y0 + x, (uint8_t)fill);
    }
    set_pixel(x0 + x, y0 + y, colour); set_pixel(x0 - x, y0 + y, colour);
    set_pixel(x0 + x, y0 - y, colour); set_pixel(x0 - x, y0 - y, colour);
    set_pixel(x0 + y, y0 + x, colour); set_pixel(x0 - y, y0 + x, colour);
    set_pixel(x0 + y, y0 - x, colour); set_pixel(x0 - y, y0 - x, colour);
  }
}

/**Blits a whole bitmap or a window of one.**/
void TVout32::bitmap(int16_t x, int16_t y, const unsigned char *bmp,
                     uint16_t offset, int16_t wCrop, int16_t lines) {
  BmpInfo bi = info(bmp);
  if (wCrop <= 0 || wCrop > bi.w) wCrop = bi.w;
  if (lines <= 0 || lines > bi.h)  lines = bi.h;
  const uint16_t wb = (bi.w + 7) / 8;
  blit(x, y, wCrop, lines, bmp + (offset ? offset : bi.dataOff), wb, 1);
}

/**Source pixel k of a row at 1, 2 or 4 bpp, as a 0..15 grey level.**/
static inline uint8_t srcLevel(const uint8_t *row, int16_t k, uint8_t bpp) {
  switch (bpp) {
    case 1:  return ((row[k >> 3] >> (7 - (k & 7))) & 1) ? 15 : 0;
    case 2:  return ((row[k >> 2] >> (6 - 2 * (k & 3))) & 3) * 5;
    default: return (k & 1) ? (row[k >> 1] & 15) : (row[k >> 1] >> 4);
  }
}

/**Byte -> framebuffer bytes, so a source row expands with table lookups.**/
static uint8_t lut1[256][4];      // 8 x 1bpp -> 4 bytes of 2 grey pixels
static uint8_t lut2[256][2];      // 4 x 2bpp -> 2 bytes
static bool    lutsReady = false;

static void buildLuts() {
  for (int b = 0; b < 256; b++) {
    for (int i = 0; i < 4; i++)
      lut1[b][i] = (((b >> (7 - 2 * i)) & 1) ? 0xF0 : 0) | (((b >> (6 - 2 * i)) & 1) ? 0x0F : 0);
    for (int i = 0; i < 2; i++)
      lut2[b][i] = (uint8_t)((((b >> (6 - 4 * i)) & 3) * 5) << 4) | (((b >> (4 - 4 * i)) & 3) * 5);
  }
  lutsReady = true;
}

void TVout32::blit(int16_t x, int16_t y, int16_t w, int16_t h,
                   const uint8_t *bits, uint16_t stride, uint8_t bpp) {
  if (w <= 0 || h <= 0) return;
  if (!lutsReady) buildLuts();
  uint8_t *fb = Composite.frame();

  /* Partly off the left/right edge: per-pixel, clipped. Rare (DVD bounce
   * never goes there), so it does not need to be fast. */
  if (x < 0 || x + w > LOG_W) {
    for (int16_t r = 0; r < h; r++) {
      const int16_t py = y + r;
      if ((uint16_t)py >= LOG_H) continue;
      const uint8_t *p = bits + (uint32_t)r * stride;
      for (int16_t k = 0; k < w; k++) set_grey(x + k, py, srcLevel(p, k, bpp));
    }
    return;
  }

  /* On screen horizontally: expand the source row to 2-pixels-per-byte with
   * the tables above, then copy -- straight for an even x, shifted by one
   * nibble for an odd one. A full screen takes well under a millisecond. */
  static uint8_t row[FB_STRIDE + 4];
  const int16_t sb = (w * bpp + 7) / 8;                 // source bytes used
  for (int16_t r = 0; r < h; r++) {
    const int16_t py = y + r;
    if ((uint16_t)py >= LOG_H) continue;
    const uint8_t *p = bits + (uint32_t)r * stride;
    if (bpp == 1)      for (int16_t i = 0; i < sb; i++) memcpy(row + 4 * i, lut1[p[i]], 4);
    else if (bpp == 2) for (int16_t i = 0; i < sb; i++) memcpy(row + 2 * i, lut2[p[i]], 2);
    else               memcpy(row, p, sb);

    uint8_t *d = fb + py * FB_STRIDE + (x >> 1);
    if (!(x & 1)) {
      const int16_t n = w >> 1;
      memcpy(d, row, n);
      if (w & 1) d[n] = (d[n] & 0x0F) | (row[n] & 0xF0);
    } else {
      d[0] = (d[0] & 0xF0) | (row[0] >> 4);
      int16_t rem = w - 1, j = 1;
      for (; rem >= 2; rem -= 2, j++) d[j] = (uint8_t)((row[j - 1] << 4) | (row[j] >> 4));
      if (rem == 1) d[j] = (d[j] & 0x0F) | (uint8_t)(row[j - 1] << 4);
    }
  }
}

/**ASCII -> C64 screen code (set 1 is uppercase only, so a-z reuse A-Z).**/
static inline uint8_t screenCode(uint8_t c) {
  if (c >= 64 && c <= 95)  return c - 64;
  if (c >= 96 && c <= 127) return c - 96;
  return c;
}

void TVout32::drawChar(int16_t x, int16_t y, uint8_t c) {
  const unsigned char *g = font8x8_c64 + (uint16_t)screenCode(c) * 8;
  for (uint8_t row = 0; row < FONT_H; row++) {
    uint8_t bits = pgm_read_byte(g + row);
    for (uint8_t bit = 0; bit < FONT_W; bit++) {
      uint8_t on = (bits >> (7 - bit)) & 1;
      for (uint8_t sy = 0; sy < tsize; sy++)
        for (uint8_t sx = 0; sx < tsize; sx++)
          set_pixel(x + bit * tsize + sx, y + row * tsize + sy, on ? WHITE : BLACK);
    }
  }
}

/**Shift the picture up by one text row.**/
void TVout32::scroll() {
  uint8_t *f = Composite.frame();
  const int step = FONT_H * tsize * FB_STRIDE;
  if (step >= FB_SIZE) { memset(f, 0, FB_SIZE); return; }
  memmove(f, f + step, FB_SIZE - step);
  memset(f + FB_SIZE - step, 0, step);
}

size_t TVout32::write(uint8_t c) {
  const int16_t cw = FONT_W * tsize, ch = FONT_H * tsize;
  if (c == '\r') { cx = 0; return 1; }
  if (c == '\n') { cx = 0; cy += ch; }
  else {
    if (cx + cw > LOG_W) { cx = 0; cy += ch; }
    drawChar(cx, cy, c);
    cx += cw;
  }
  while (cy + ch > LOG_H) { scroll(); cy -= ch; }
  return 1;
}
