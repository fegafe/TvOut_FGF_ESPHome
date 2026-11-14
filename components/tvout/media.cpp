#define TVOUT_ENGINE
#include "TVout32.h"
#include "media.h"
#include "imgstore.h"
#include <LittleFS.h>

/**The one open animation.**/
static struct {
  char      name[25] = "";
  bool      open = false;
  AnimHdr   hdr;
  uint16_t *delays = nullptr;
  uint8_t  *all = nullptr;       // whole file when it fits in RAM...
  uint8_t  *frame = nullptr;     // ...else one frame, refilled from flash
  uint16_t  idx = 0;
  uint32_t  dueAt = 0;           // millis() when idx+1 is due
} A;

/**Streamed animations keep their file open and read frames in order: on this
 * flash an open costs ~23ms and a seek ~17ms, a 7KB read only ~8ms. The handle
 * is guarded so the web task can close it before replacing/deleting the file.**/
static SemaphoreHandle_t fileMx = xSemaphoreCreateMutex();
static File     af;
static uint16_t afNext = 0xFFFF;  // frame the file position is at

void media_files_lock() {
  xSemaphoreTake(fileMx, portMAX_DELAY);
  if (af) af.close();
  af = File();
}

void media_files_unlock() { xSemaphoreGive(fileMx); }

/**What the last call handed out, for MediaFrame::changed.**/
static char     lastName[25] = "";
static uint32_t lastGen = 0;
static int32_t  lastIdx = -1;

static void closeAnim() {
  media_files_lock();
  media_files_unlock();
  free(A.delays); free(A.all); free(A.frame);
  A.delays = nullptr; A.all = A.frame = nullptr;
  A.open = false;
  A.name[0] = 0;
}

static bool readFrame(uint16_t i) {
  if (A.all) return true;
  xSemaphoreTake(fileMx, portMAX_DELAY);
  if (!af) {
    af = LittleFS.open(imgstore_path(A.name, true), "r");
    afNext = 0xFFFF;
  }
  const uint32_t fb = anim_frame_bytes(A.hdr);
  bool ok = af;
  if (ok && afNext != i) ok = af.seek(anim_data_off(A.hdr) + (uint32_t)i * fb);   // only on loop/skip
  ok = ok && af.read(A.frame, fb) == fb;
  afNext = ok ? i + 1 : 0xFFFF;
  xSemaphoreGive(fileMx);
  return ok;
}

static bool openAnim(const char *name) {
  closeAnim();
  File f = LittleFS.open(imgstore_path(name, true), "r");
  if (!f) return false;
  uint8_t h[TVA_HDR];
  AnimHdr a;
  if (f.read(h, TVA_HDR) != TVA_HDR || !anim_parse(h, TVA_HDR, a) || f.size() < anim_total(a)) {
    f.close();
    return false;
  }
  A.hdr = a;
  A.delays = (uint16_t *)malloc(2UL * a.frames);
  bool ok = A.delays && f.read((uint8_t *)A.delays, 2UL * a.frames) == 2UL * a.frames;
  if (ok && anim_total(a) <= ANIM_RAM_MAX && (A.all = (uint8_t *)malloc(anim_total(a)))) {
    f.seek(0);
    ok = f.read(A.all, anim_total(a)) == anim_total(a);
  } else if (ok) {
    ok = (A.frame = (uint8_t *)malloc(anim_frame_bytes(a))) != nullptr;
  }
  f.close();
  strlcpy(A.name, name, sizeof(A.name));
  A.open = ok;
  if (!ok) { closeAnim(); return false; }
  for (uint16_t i = 0; i < a.frames; i++)        // GIF convention: tiny = "fast"
    if (A.delays[i] < 20) A.delays[i] = 100;
  A.idx = 0;
  A.dueAt = millis() + A.delays[0];
  return readFrame(0);
}

void media_rewind() {
  lastName[0] = 0;                  // screen was cleared: next call is "changed"
  lastIdx = -1;
  if (!A.open) return;
  A.idx = 0;
  A.dueAt = millis() + A.delays[0];
  readFrame(0);
}

bool media_frame(const char *name, MediaFrame &f) {
  const uint32_t gen = imgstore_generation();
  const bool fresh = (gen != lastGen) || strcmp(name, lastName);
  if (gen != lastGen) closeAnim();               // file may have been replaced

  if (const uint8_t *img = imgstore_get(name)) {
    TVout32::BmpInfo bi = TVout32::info(img);
    f = { bi.w, bi.h, (uint16_t)((bi.w + 7) / 8), img + bi.dataOff, 1, false, fresh };
    if (A.open) closeAnim();
  } else {
    if (!A.open || strcmp(A.name, name)) {
      if (!openAnim(name)) return false;
      lastIdx = -1;
    }
    const uint32_t now = millis();
    if ((int32_t)(now - A.dueAt) >= 0) {
      A.idx = (A.idx + 1) % A.hdr.frames;
      A.dueAt += A.delays[A.idx];
      if ((int32_t)(now - A.dueAt) > 500) A.dueAt = now + A.delays[A.idx];  // fell behind
      readFrame(A.idx);
    }
    const uint8_t *bits = A.all ? A.all + anim_data_off(A.hdr) + (uint32_t)A.idx * anim_frame_bytes(A.hdr)
                                : A.frame;
    f = { (int16_t)A.hdr.w, (int16_t)A.hdr.h, anim_stride(A.hdr), bits, A.hdr.bpp,
          A.hdr.frames > 1, fresh || lastIdx != A.idx };
    lastIdx = A.idx;
  }
  strlcpy(lastName, name, sizeof(lastName));
  lastGen = gen;
  return true;
}
