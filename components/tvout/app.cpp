#define TVOUT_ENGINE
#include "TVout32.h"
#include "logos.h"
#include "cube_tv.h"
#include "tv_state.h"
#include "imgstore.h"
#include "sound.h"
#include "media.h"
#include "game.h"

/*********************
* The TvOut_FGF_ESP32 sketch, minus networking. setup()'s drawing half and
* loop() run in their own task on core 1 (app_start()), because loop() blocks
* for whole frames and seconds at a time -- ESPHome's main loop must not.
*
* MODES (prog):
*   a  intro       img_name scrolls in, then bounces (d)
*   i  show        img_name centred
*   m  message     msgbuf centred
*   d  dvd         img_name bouncing (trail on/off)
*   b  butterfly   the ROM butterflies
*   c  cube        3D cube
*   g  game        first-person raycaster; play it from http://<board>/game
*   3  birthday    hb_photo + the tune once on entry
*   img_name defaults to logo_princ; the boot intro always uses logo_princ.
*********************/

volatile char prog = 'd';
char msgbuf[64] = "";
volatile bool msg_dirty = false;
char img_name[32] = "logo_princ";
volatile bool img_dirty = false;
volatile bool dvd_trail = true;
volatile float field_rate = 0;

/**Program select**/
static char lastprog = 0;

/***Logo files in LittleFS used by the fixed modes***/
#define IMG_LOGO     "logo_princ"   // a, boot intro
#define IMG_BATH1    "bath1"
#define IMG_BATH2    "bath2"
#define IMG_BDAY     "hb_photo"     // 3

/***Variables for asdvd***/
static int x_st, y_st;
static int vx = 1;
static int vy = 2;

/***Index for Butterfly***/
static int butterfly_index = 0;

/**Variables 4 butterfly and bathroom**/
static int b_status = 0;
static const unsigned char *b_ptr[6]     = {butterfly0, butterfly1, butterfly2,
                                            butterfly3, butterfly4, butterfly5};
static const unsigned char *b_ptr_inv[6] = {butterfly0_inv, butterfly1_inv, butterfly2_inv,
                                            butterfly3_inv, butterfly4_inv, butterfly5_inv};

/**The .ino got these prototypes from the Arduino preprocessor.**/
static void intro(const char *name);
static void show_stored();
static void show_message();
static void asdvd(const char *name);
static void butterfly_fly();
static void birthday();
static void center_image(const char *name);
static void center_message(const char *tmsg);
static void place_random(const char *name);

static void app_loop() {
  if (lastprog != prog) {
    if (lastprog == 'g') game_end();    // back to single-buffered drawing
    TV.clear_screen();
    lastprog = prog;
    if (prog == 'd') place_random(img_name);
    if (prog == 'm') msg_dirty = true;  // redraw the current message
    if (prog == '3') snd_birthday();    // the tune plays once on entry
    media_rewind();
  }
  static uint32_t seenGen = 0;        // a file was replaced: drop any remnants
  if (seenGen != imgstore_generation()) {
    seenGen = imgstore_generation();
    TV.clear_screen();
    media_rewind();
  }
  if (img_dirty) {                    // web UI / ESPHome picked another image
    img_dirty = false;
    TV.clear_screen();
    media_rewind();
    if (prog == 'd') place_random(img_name);   // maybe a new size
  }
  switch (prog) {
    case 'a': intro(img_name);            break;
    case 'i': show_stored();              break;
    case 'm': show_message();             break;
    case 'd': asdvd(img_name);            break;
    case 'b': butterfly_fly();            break;
    case 'c': rotate_cube(TV);            break;
    case 'g': game_frame();               break;
    case '3': birthday();                 break;
    default:  prog = 'd';                 break;
  }
}

static void appTask(void *) {
  {                                   // sanity check: should read ~60.0
    Composite.waitFrames(1);
    uint32_t f0 = Composite.frames(), t0 = micros();
    Composite.waitFrames(120);
    uint32_t n = Composite.frames() - f0, dt = micros() - t0;
    field_rate = n * 1e6f / dt;       // tvout_component logs and reports it
  }
  snd_startup();
  intro(IMG_LOGO);
  for (;;) app_loop();
}

void app_start() {
  /**Same core as Arduino's loop() was, and the same priority as ESPHome's
   * loop task, so a mode that never sleeps (the cube) still time-slices with
   * it. The video task outranks both.**/
  xTaskCreatePinnedToCore(appTask, "tvapp", 8192, nullptr, 1, nullptr, 1);
}

static void butterfly_fly() {
  TVout32::BmpInfo bi = TVout32::info(b_ptr[butterfly_index]);
  int16_t w = bi.w, l = bi.h;
  TV.clear_screen();
  if (vx < 0) TV.bitmap(x_st, y_st, b_ptr[butterfly_index]);
  else        TV.bitmap(x_st, y_st, b_ptr_inv[butterfly_index]);
  TV.delay(50);
  if ((x_st + w + vx) >= TV.hres() || (x_st + vx) <= 0) vx = -vx;
  if ((y_st + l + vy) >= TV.vres() || (y_st + vy) <= 0) vy = -vy;
  x_st += vx;
  y_st += vy;
  butterfly_index = (butterfly_index + 1) % 6;
}

[[maybe_unused]] static void bathroom() {
  b_status = b_status % 2;
  TV.clear_screen();
  media_rewind();
  if (b_status == 0) center_image(IMG_BATH1);
  else               center_image(IMG_BATH2);
  b_status += 1;
  TV.delay(500);
}

//Draw a still or the current animation frame. A new frame is put down right
//after the field ends, inside vertical blanking, so it never tears.
static void put_frame(int16_t x, int16_t y, const MediaFrame &f) {
  if (!f.changed) return;
  if (f.anim) TV.delay_frame(1);
  TV.blit(x, y, f.w, f.h, f.bits, f.stride, f.bpp);
}

//Center an image/animation file from LittleFS
static void center_image(const char *name) {
  MediaFrame f;
  if (!media_frame(name, f)) { center_message(name); TV.delay(100); return; }
  put_frame((TV.hres() - f.w) / 2, (TV.vres() - f.h) / 2, f);
  TV.delay(5);
}

//Random start position that keeps the image fully on screen
static void place_random(const char *name) {
  MediaFrame f;
  if (!media_frame(name, f)) return;
  x_st = random(max(1, TV.hres() - f.w));
  y_st = random(max(1, TV.vres() - f.h));
}

//Some Logo bouncing as dvd logo -- animations keep playing while they move
static void asdvd(const char *name) {
  MediaFrame f;
  if (!media_frame(name, f)) { center_message(name); TV.delay(200); return; }
  int16_t wid = f.w, hei = f.h;
  x_st = constrain(x_st, 0, max(0, TV.hres() - wid));   // image may have been
  y_st = constrain(y_st, 0, max(0, TV.vres() - hei));   // replaced by a bigger one
  TV.delay_frame(1);                  // draw every field, inside vblank, so an
  if (!dvd_trail) TV.clear_screen();  // animation keeps its speed; no trail =
  TV.blit(x_st, y_st, wid, hei, f.bits, f.stride, f.bpp);   // wipe first, same vblank
  static uint8_t step = 0;            // ...but move every 3rd field (~50ms),
  if (++step < 3) return;             // the same bounce speed as before
  step = 0;
  if ((x_st + wid + vx) >= TV.hres() || (x_st + vx) <= 0) vx = -vx;
  if ((y_st + hei + vy) >= TV.vres() || (y_st + vy) <= 0) vy = -vy;
  if (wid < TV.hres()) x_st += vx;    // full-width/height art only moves
  if (hei < TV.vres()) y_st += vy;    // along the axis it has room on
}

//Birthday card: the image, plus the tune once on entry (see app_loop())
static void birthday() {
  center_image(IMG_BDAY);
}

//Show an image uploaded to LittleFS, selected by the web UI or ESPHome
static void show_stored() {
  center_image(img_name);
}

//Show whatever message was last set
static void show_message() {
  if (msg_dirty) {
    TV.clear_screen();
    center_message(msgbuf);
    msg_dirty = false;
  }
  TV.delay(100);
}

//Center some Message
static void center_message(const char *tmsg) {
  int len = strlen(tmsg);
  int x = (len >= TV.char_line()) ? 0 : (TV.hres() - len * FONT_W) / 2;
  int y = (TV.vres() - FONT_H) / 2;
  TV.set_cursor(x, y);
  TV.delay_frame(1);
  TV.println(tmsg);
}

/**INTRO** -- an animated logo keeps playing while it scrolls in**/
static void intro(const char *name) {
  MediaFrame f;
  if (!media_frame(name, f)) { prog = 'd'; return; }
  const int16_t w = f.w, l = f.h;
  const int16_t x = (TV.hres() - w) / 2;

  /* ~48 steps whatever the image height, so tall art does not crawl */
  const int16_t rstep = (l / 48) > 1 ? (l / 48) : 1;
  const int16_t drop  = (TV.vres() - l) / 2;
  const int16_t dstep = (drop / 48) > 1 ? (drop / 48) : 1;

  /* Scroll down image from top */
  for (int16_t i = 1; i < l; i += rstep) {
    media_frame(name, f);
    TV.delay_frame(3);
    TV.clear_screen();
    TV.blit(x, 0, w, i, f.bits + f.stride * (l - i), f.stride, f.bpp);
  }
  /* Slide down to centre */
  for (int16_t i = 0; i < drop; i += dstep) {
    media_frame(name, f);
    TV.delay_frame(3);
    TV.clear_screen();
    TV.blit(x, i, w, l, f.bits, f.stride, f.bpp);
  }
  /* Hold ~3s, still playing */
  for (uint32_t t0 = millis(); millis() - t0 < 3000; ) {
    media_frame(name, f);
    TV.delay_frame(1);
    TV.blit(x, drop, w, l, f.bits, f.stride, f.bpp);
  }
  x_st = (TV.hres() - w) / 2;
  y_st = l / 2;
  prog = 'd';
  TV.clear_screen();
  media_rewind();
}
