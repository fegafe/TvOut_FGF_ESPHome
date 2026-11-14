#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
/*********************
* imgstore - runtime images in LittleFS.
*
* Files live under /img/<name>.tvb and hold exactly the same byte layout as the
* PROGMEM arrays in logos.cpp:
*
*   classic   { w, h, rows... }                    w,h <= 255
*   extended  { 0, wLo, wHi, hLo, hHi, rows... }   anything wider than 255
*
* Rows are packed 1bpp, ceil(width/8) bytes each, MSB = leftmost pixel.
*
* On ESP32 pgm_read_byte() is a plain dereference, so a loaded image can be
* handed straight to TV.bitmap() with no special case.
*
* The fixed logos the modes use (logo_princ, bath1, ...) live here too; their
* original files are in data/img and tools/push_logos.py puts them back.
*
* At the full 320x240 an image is 9605 bytes, so the 2MB filesystem
* holds around 200 of them; smaller art packs in proportionally more.
**********************/
#pragma once
#include <Arduino.h>

#define IMG_MAX_BYTES  (8 + ((LOG_W + 7) / 8) * LOG_H)  // 1bpp 320x240 plus header slack
#define UPLOAD_CHUNK   9600                             // max decoded bytes per request
#define IMG_DIR        "/img"

/**Animations and grey images: /img/<name>.tva
 *   "TVA1" | w u16 | h u16 | frames u16 | flags u8 | bpp u8   (12 bytes, LE)
 *   delay_ms u16 x frames
 *   frames x (ceil(w*bpp/8) * h) bytes, rows MSB-first
 * bpp is 1 (black/white), 2 (4 greys) or 4 (16 greys). A grey still is simply
 * a 1-frame .tva; .tvb stays 1bpp only, for compatibility.
 * A name is either a still (.tvb) or an animation (.tva), never both --
 * saving one kind removes the other, so "replace" can swap between them.**/
#define TVA_HDR         12
#define ANIM_MAX_FRAMES 120
#define ANIM_MAX_BYTES  (1024UL * 1024UL)

struct AnimHdr { uint16_t w, h, frames; uint8_t flags, bpp; };

/**Parse and sanity-check a .tva header; false if it is not one.**/
bool anim_parse(const uint8_t *b, size_t len, AnimHdr &a);
inline uint16_t anim_stride(const AnimHdr &a)      { return (uint16_t)((a.w * a.bpp + 7) / 8); }
inline uint32_t anim_frame_bytes(const AnimHdr &a) { return (uint32_t)anim_stride(a) * a.h; }
inline uint32_t anim_data_off(const AnimHdr &a)    { return TVA_HDR + 2UL * a.frames; }
inline uint32_t anim_total(const AnimHdr &a)       { return anim_data_off(a) + anim_frame_bytes(a) * a.frames; }

/**Chunked .tva upload, one HTTP request per piece (<= UPLOAD_CHUNK bytes):
 *   pos < 0      -> header + delay table (starts /img/<name>.part)
 *   pos >= 0     -> frame data at byte `pos` of the frame area, strictly in order
 * Returns 1 when the last byte lands and the file is committed as .tva, 0 when
 * a piece was accepted, -1 on any error (the .part is discarded).**/
int imgstore_anim_put(const char *name, int32_t pos, const uint8_t *data, size_t len, String &err);

/**Path helpers for the player.**/
String imgstore_path(const char *name, bool anim);

bool imgstore_begin();

/**Write (or overwrite) an image. Returns false on a bad header or a full FS.**/
bool imgstore_save(const char *name, const uint8_t *data, size_t len);

bool imgstore_delete(const char *name);

/**Cached image by name, or nullptr if it is not on the FS. Call ONLY from the
 * loop task: the cache is owned by it and reloads itself after any save or
 * delete, so a logo replaced from the web UI shows up on the next draw. The
 * pointer stays valid until the next imgstore_get() after such a change.**/
const uint8_t *imgstore_get(const char *name);

/**Changes whenever any image is saved or deleted.**/
uint32_t imgstore_generation();

/**JSON array of {"n":name,"w":w,"h":h,"s":bytes,"a":frames,"b":bpp} for the
 * web UI ("a" is 0 for .tvb stills, 1 for a grey still).**/
String imgstore_list_json();

/**Every stored image/animation name, sorted, comma separated
 * ("bath1,bath2,logo_princ"). Safe from any task.**/
String imgstore_names();

/**Bytes used / total, for the web UI footer.**/
size_t imgstore_used();
size_t imgstore_total();

#endif  // TVOUT_ENGINE
