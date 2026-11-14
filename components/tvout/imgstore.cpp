#define TVOUT_ENGINE
#include "TVout32.h"
#include "imgstore.h"
#include <LittleFS.h>
#include "media.h"
#include <algorithm>
#include <vector>

/**Bumped by every save/delete (web or MQTT task); the cache compares it.**/
static volatile uint32_t gen = 1;

/**Reject anything that would not blit sanely, and keep names filesystem-safe.**/
static bool safeName(const char *n) {
  if (!n || !*n) return false;
  size_t len = strlen(n);
  if (len > 24) return false;
  for (const char *p = n; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return false;
  return true;
}

static String pathOf(const char *name, const char *ext = ".tvb") {
  return String(IMG_DIR "/") + name + ext;
}

String imgstore_path(const char *name, bool anim) {
  return pathOf(name, anim ? ".tva" : ".tvb");
}

bool anim_parse(const uint8_t *b, size_t len, AnimHdr &a) {
  if (len < TVA_HDR || memcmp(b, "TVA1", 4)) return false;
  a.w      = b[4] | (b[5] << 8);
  a.h      = b[6] | (b[7] << 8);
  a.frames = b[8] | (b[9] << 8);
  a.flags  = b[10];
  a.bpp    = b[11];
  return a.w && a.h && a.w <= LOG_W && a.h <= LOG_H &&
         (a.bpp == 1 || a.bpp == 2 || a.bpp == 4) &&
         a.frames && a.frames <= ANIM_MAX_FRAMES && anim_total(a) <= ANIM_MAX_BYTES;
}

bool imgstore_begin() {
  if (!LittleFS.begin(true)) return false;        // true = format if unmounted
  if (!LittleFS.exists(IMG_DIR)) LittleFS.mkdir(IMG_DIR);
  return true;
}

bool imgstore_save(const char *name, const uint8_t *data, size_t len) {
  if (!safeName(name) || len < 3 || len > IMG_MAX_BYTES) return false;

  TVout32::BmpInfo bi = TVout32::info(data);
  if (bi.w <= 0 || bi.h <= 0 || bi.w > LOG_W || bi.h > LOG_H) return false;
  size_t need = bi.dataOff + (size_t)((bi.w + 7) / 8) * bi.h;
  if (len < need) return false;                   // truncated payload

  File f = LittleFS.open(pathOf(name), "w");
  if (!f) return false;
  size_t wrote = f.write(data, need);
  f.close();
  media_files_lock();
  LittleFS.remove(pathOf(name, ".tva"));          // a name is one kind or the other
  gen = gen + 1;
  media_files_unlock();
  return wrote == need;
}

bool imgstore_delete(const char *name) {
  if (!safeName(name)) return false;
  media_files_lock();
  bool ok = LittleFS.remove(pathOf(name));
  ok = LittleFS.remove(pathOf(name, ".tva")) || ok;
  LittleFS.remove(pathOf(name, ".part"));
  gen = gen + 1;
  media_files_unlock();
  return ok;
}

static uint8_t *readFile(const char *name) {
  if (!safeName(name)) return nullptr;
  File f = LittleFS.open(pathOf(name), "r");
  if (!f) return nullptr;
  size_t n = f.size();
  if (n < 3 || n > IMG_MAX_BYTES) { f.close(); return nullptr; }
  uint8_t *buf = (uint8_t *)malloc(n);
  if (!buf) { f.close(); return nullptr; }
  n = f.read(buf, n);
  f.close();
  TVout32::BmpInfo bi = TVout32::info(buf);
  if (bi.w <= 0 || bi.h <= 0 || bi.w > LOG_W || bi.h > LOG_H ||
      n < bi.dataOff + (size_t)((bi.w + 7) / 8) * bi.h) {
    free(buf);
    return nullptr;
  }
  return buf;
}

#define CACHE_SLOTS 12
static struct Slot { char name[25]; uint8_t *buf; uint32_t used; } cache[CACHE_SLOTS];
static uint32_t cacheGen = 0, tick = 0;

const uint8_t *imgstore_get(const char *name) {
  if (cacheGen != gen) {                          // something changed on the FS
    for (Slot &c : cache) { free(c.buf); c = Slot{}; }
    cacheGen = gen;
  }
  Slot *victim = &cache[0];
  for (Slot &c : cache) {
    if (c.name[0] && !strcmp(c.name, name)) { c.used = ++tick; return c.buf; }
    if (c.used < victim->used) victim = &c;
  }
  free(victim->buf);                              // LRU slot
  strlcpy(victim->name, name, sizeof(victim->name));
  victim->buf = readFile(name);                   // nullptr is cached too: no retry storm
  victim->used = ++tick;
  return victim->buf;
}

uint32_t imgstore_generation() { return gen; }

int imgstore_anim_put(const char *name, int32_t pos, const uint8_t *data, size_t len, String &err) {
  if (!safeName(name)) { err = "bad name"; return -1; }
  const String part = pathOf(name, ".part");
  auto fail = [&](const char *why) { LittleFS.remove(part); err = why; return -1; };

  if (pos < 0) {
    AnimHdr a;
    if (!anim_parse(data, len, a) || len != anim_data_off(a)) return fail("bad header");
    size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
    File old = LittleFS.open(pathOf(name, ".tva"), "r");
    if (old) { freeB += old.size(); old.close(); }  // replacing frees that
    if (anim_total(a) + 16384 > freeB) return fail("not enough space on the board");
    File f = LittleFS.open(part, "w");
    if (!f) return fail("cannot create file");
    bool ok = f.write(data, len) == len;
    f.close();
    return ok ? 0 : fail("write failed");
  }

  File f = LittleFS.open(part, "r");
  if (!f) { err = "no upload in progress"; return -1; }
  uint8_t hdr[TVA_HDR];
  AnimHdr a;
  const bool hdrOk = f.read(hdr, TVA_HDR) == TVA_HDR && anim_parse(hdr, TVA_HDR, a);
  const size_t have = f.size();
  f.close();
  if (!hdrOk) return fail("corrupt upload");
  if (!len || len > UPLOAD_CHUNK || anim_data_off(a) + pos + len > anim_total(a))
    return fail("bad chunk");
  if (have != anim_data_off(a) + (size_t)pos) return fail("chunk out of order");

  f = LittleFS.open(part, "a");
  if (!f) return fail("cannot append");
  bool ok = f.write(data, len) == len;
  f.close();
  if (!ok) return fail("write failed (full?)");
  if (have + len < anim_total(a)) return 0;

  media_files_lock();
  LittleFS.remove(pathOf(name, ".tva"));
  LittleFS.remove(pathOf(name));                  // replaces a still of that name
  const bool renamed = LittleFS.rename(part, pathOf(name, ".tva"));
  gen = gen + 1;
  media_files_unlock();
  if (!renamed) return fail("rename failed");
  return 1;
}

String imgstore_list_json() {
  String out = "[";
  File dir = LittleFS.open(IMG_DIR);
  if (dir) {
    bool first = true;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String fn = f.name();
      int dot = fn.lastIndexOf('.');
      if (dot < 0) continue;
      String ext = fn.substring(dot);
      int16_t w, h;
      uint16_t frames = 0;
      uint8_t bpp = 1;
      if (ext == ".tvb") {
        uint8_t hdr[5] = {0, 0, 0, 0, 0};
        f.read(hdr, 5);
        TVout32::BmpInfo bi = TVout32::info(hdr);
        w = bi.w; h = bi.h;
      } else if (ext == ".tva") {
        uint8_t hdr[TVA_HDR];
        AnimHdr a;
        if (f.read(hdr, TVA_HDR) != TVA_HDR || !anim_parse(hdr, TVA_HDR, a)) continue;
        w = a.w; h = a.h; frames = a.frames; bpp = a.bpp;
      } else continue;                            // .part and strays
      if (!first) out += ',';
      first = false;
      out += "{\"n\":\"" + fn.substring(0, dot) + "\",\"w\":" + w +
             ",\"h\":" + h + ",\"s\":" + f.size() + ",\"a\":" + frames +
             ",\"b\":" + bpp + "}";
    }
  }
  out += ']';
  return out;
}

String imgstore_names() {
  std::vector<String> names;
  File dir = LittleFS.open(IMG_DIR);
  if (dir) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String fn = f.name();
      int dot = fn.lastIndexOf('.');
      if (dot < 0) continue;
      String ext = fn.substring(dot);
      if (ext != ".tvb" && ext != ".tva") continue;   // .part and strays
      names.push_back(fn.substring(0, dot));
    }
  }
  std::sort(names.begin(), names.end());
  String out;
  for (const String &n : names) {
    if (out.length()) out += ',';
    out += n;
  }
  return out;
}

size_t imgstore_used()  { return LittleFS.usedBytes();  }
size_t imgstore_total() { return LittleFS.totalBytes(); }
