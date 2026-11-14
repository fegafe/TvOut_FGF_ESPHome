#define TVOUT_ENGINE
#include "game.h"
#include "TVout32.h"
#include "sound.h"
#include <LittleFS.h>
#include <math.h>

/**------------ geometry ------------**/
#define GW      160               // render width  (x2 = 320 on screen)
#define VH      112               // 3D view rows  (x2 = 224 on screen)
#define HUD_Y   (VH * 2)          // HUD occupies screen lines 224..239
#define MAXM    32                // largest map side
#define TEX     32                // texture / sprite size
#define MAXE    48                // entities per level
#define TRANSP  0xFF              // transparent sprite texel

static_assert(GW * 2 == FB_W && HUD_Y + 16 == FB_H, "view must double onto the framebuffer");

/**------------ levels ------------**
 *  # or 1 brick   2 stone   3 wood   4 tech panel   5 EXIT (walk into it)
 *  . or space floor   P player start (facing east)
 *  e enemy   h health   a ammo
 * /lvl/<n>.txt in LittleFS, same format, overrides or extends these.**/
static const char *const LEVEL1[] = {
  "########################",
  "#P.....#..........#....#",
  "#......#..e.......#..h.#",
  "#......#..........1....#",
  "#..a...2222..222222....#",
  "#......................#",
  "###.####...........e...#",
  "#......#....33333......#",
  "#..e...#....3...3......#",
  "#......#....3.a.3....###",
  "#...........3...3......#",
  "####.###....33.33..e...#",
  "#......#...............#",
  "#..h...#.......e.......#",
  "#......####4444.4444####",
  "#..e...................#",
  "#......................#",
  "#####..########.....e..#",
  "#...#..#......#........#",
  "#.a.#..#..e...#..h.....#",
  "#...#..#......#........#",
  "#......#..............5#",
  "#......#......#......e5#",
  "########################",
};

static const char *const LEVEL2[] = {
  "4444444444444444444444444",
  "4P......4.......4.......4",
  "4.......4...e...4...a...4",
  "4...4...4.......4.......4",
  "4...4.......4.......e...4",
  "4...4...e...4...........4",
  "4...44444...44444...44444",
  "4.......4.......4.......4",
  "4..a....4...h...4...e...4",
  "4.......2.......2.......4",
  "44444...2...e...2...44444",
  "4.......2.......2.......4",
  "4...e...22222.222222....4",
  "4.......................4",
  "4...3333.........3333...4",
  "4...3..3....e....3..3...4",
  "4...3.h3.........3a.3...4",
  "4...33.3.........3.33...4",
  "4............e..........4",
  "4...e...4444.4444...e...4",
  "4.......4.......4.......4",
  "4..h....4...e...4.....555",
  "4.......4.......4.......5",
  "4444444444444444444444444",
};

static const char *const *const BUILTIN[] = { LEVEL1, LEVEL2 };
static const uint8_t BUILTIN_ROWS[] = { sizeof(LEVEL1) / sizeof(*LEVEL1),
                                        sizeof(LEVEL2) / sizeof(*LEVEL2) };
#define NBUILTIN 2

/**------------ state ------------**/
enum EType  : uint8_t { E_ENEMY, E_HEALTH, E_AMMO };
enum EState : uint8_t { S_IDLE, S_CHASE, S_FIRE, S_PAIN, S_DEAD, S_GONE };
enum GState : uint8_t { G_PLAY, G_DEAD, G_LEVEL, G_WON };
enum Spr    : uint8_t { SP_ENEMY, SP_FIRE, SP_PAIN, SP_DEAD, SP_HEALTH, SP_AMMO, NSPR };

struct Ent {
  float x, y;
  EType type;
  EState st;
  int16_t hp;
  float timer, cool;
  int16_t scrX, scrHalf;          // last projection, for hit tests
  float depth;
};

static uint8_t mapW, mapH, cells[MAXM][MAXM];
static Ent     ents[MAXE];
static uint8_t nEnts;

/* ~30KB, allocated only while the game runs (game_frame / game_end) */
static uint8_t (*tex)[TEX * TEX];      // [1..5] walls, column-major, levels 0..15
static uint8_t (*spr)[TEX * TEX];      // [NSPR], column-major, TRANSP = see-through
static uint8_t *view;                  // GW*VH, one byte per pixel, 0..15
static uint8_t *gameMem;               // the one allocation behind all three
static float   zbuf[GW];
static uint8_t shadeLut[16][16];       // [darkness][level]
static uint8_t bgRow[VH][2];           // dithered ceiling/floor level per row

static float px, py, dirX, dirY, planeX, planeY;
static int16_t hp, ammo, kills, nEnemies, level;
static GState gst;
static float stateT, hurtT, fireT, fireCool, walkPhase;
static bool running = false;
static uint32_t lastMs, fpsT0;
static uint16_t fpsN, fps;

static volatile uint16_t keys = 0;
static volatile uint32_t keysAt = 0;

/**------------ sounds ------------**/
static const Tone SND_SHOT[]   = { {900, 25}, {300, 35} };
static const Tone SND_CLICK[]  = { {2000, 8} };
static const Tone SND_HURT[]   = { {150, 90} };
static const Tone SND_ESHOT[]  = { {600, 20}, {250, 30} };
static const Tone SND_EDIE[]   = { {400, 60}, {200, 90}, {100, 120} };
static const Tone SND_PICK[]   = { {880, 40}, {1320, 60} };
static const Tone SND_ALERT[]  = { {300, 50}, {450, 50} };
static const Tone SND_LEVEL[]  = { {523, 90}, {659, 90}, {784, 90}, {1047, 200} };
static const Tone SND_DEATH[]  = { {400, 150}, {300, 150}, {200, 150}, {100, 300} };
#define PLAY(t) sound_play(t, sizeof(t) / sizeof(Tone))

/**------------ helpers ------------**/
static uint32_t rng = 12345;
static inline uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static inline float frand() { return (rnd() & 0xFFFF) / 65535.0f; }
static inline uint8_t clamp15(int v) { return v < 0 ? 0 : v > 15 ? 15 : v; }

static inline void texSet(uint8_t *t, int x, int y, int v) {
  if ((unsigned)x < TEX && (unsigned)y < TEX) t[x * TEX + y] = (v == TRANSP) ? TRANSP : clamp15(v);
}
static void sprRect(uint8_t *s, int x0, int y0, int x1, int y1, int v) {
  for (int x = x0; x <= x1; x++) for (int y = y0; y <= y1; y++) texSet(s, x, y, v);
}
static void sprCircle(uint8_t *s, int cx, int cy, int r, int v) {
  for (int x = cx - r; x <= cx + r; x++)
    for (int y = cy - r; y <= cy + r; y++)
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) texSet(s, x, y, v);
}

/**------------ procedural art ------------**/
static void makeTextures() {
  uint8_t *t;
  /* 1 brick: 16x8 bricks, offset every other course, grey mortar */
  t = tex[1];
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++) {
      const int course = y / 8, bx = (x + (course & 1) * 8) % 16;
      const bool mortar = (y % 8 == 7) || (bx == 15);
      texSet(t, x, y, mortar ? 4 : 9 + (int)(rnd() % 3) - (y % 8 == 0 ? -2 : 0));
    }
  /* 2 stone blocks with bevelled edges */
  t = tex[2];
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++) {
      const int lx = x % 16, ly = y % 16;
      int v = 8 + (int)(rnd() % 3) - 1;
      if (lx == 0 || ly == 0) v = 12;
      if (lx == 15 || ly == 15) v = 3;
      texSet(t, x, y, v);
    }
  /* 3 wood: vertical planks with grain */
  t = tex[3];
  for (int x = 0; x < TEX; x++) {
    const float ph = frand() * 6.28f;
    for (int y = 0; y < TEX; y++) {
      int v = (x % 8 == 0) ? 2 : 7 + (int)(1.5f * sinf(y * 0.6f + ph + x));
      texSet(t, x, y, v);
    }
  }
  /* 4 tech panel: riveted plate with a light strip */
  t = tex[4];
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++) {
      int v = 6;
      if (x == 0 || y == 0) v = 10;
      if (x == TEX - 1 || y == TEX - 1) v = 3;
      if ((x == 3 || x == TEX - 4) && (y == 3 || y == TEX - 4)) v = 13;
      if (y >= 14 && y <= 17 && x > 4 && x < TEX - 5) v = (y == 15 || y == 16) ? 15 : 11;
      texSet(t, x, y, v);
    }
  /* 5 exit: hazard stripes */
  t = tex[5];
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++)
      texSet(t, x, y, ((x + y) / 4) & 1 ? 14 : 2);
}

static void makeSprites() {
  for (int i = 0; i < NSPR; i++) memset(spr[i], TRANSP, TEX * TEX);
  /* enemy: a squat horned figure */
  uint8_t *s = spr[SP_ENEMY];
  sprRect(s, 11, 30, 14, 31, 3); sprRect(s, 18, 30, 21, 31, 3);       // feet
  sprRect(s, 12, 22, 14, 29, 5); sprRect(s, 18, 22, 20, 29, 5);       // legs
  sprRect(s, 9, 12, 23, 22, 7);                                       // body
  sprRect(s, 9, 12, 10, 22, 9);                                       // lit edge
  sprRect(s, 5, 13, 8, 20, 6); sprRect(s, 24, 13, 27, 20, 6);         // arms
  sprCircle(s, 16, 7, 5, 8);                                          // head
  texSet(s, 10, 2, 10); texSet(s, 11, 1, 10); texSet(s, 21, 2, 10); texSet(s, 22, 1, 10); // horns
  sprRect(s, 13, 6, 14, 7, 15); sprRect(s, 18, 6, 19, 7, 15);         // glowing eyes
  sprRect(s, 14, 10, 18, 10, 2);                                      // mouth
  /* firing: same, gun arm forward with a flash */
  memcpy(spr[SP_FIRE], s, TEX * TEX);
  sprRect(spr[SP_FIRE], 22, 15, 28, 17, 6);
  sprCircle(spr[SP_FIRE], 29, 16, 3, 15);
  /* pain: washed out */
  for (int i = 0; i < TEX * TEX; i++)
    spr[SP_PAIN][i] = s[i] == TRANSP ? TRANSP : clamp15(s[i] + 5);
  /* dead: a heap on the floor */
  s = spr[SP_DEAD];
  sprRect(s, 6, 27, 26, 31, 5);
  sprCircle(s, 10, 27, 3, 7);
  sprRect(s, 14, 25, 22, 27, 6);
  texSet(s, 9, 26, 12); texSet(s, 11, 26, 12);
  /* health: white kit, dark cross */
  s = spr[SP_HEALTH];
  sprRect(s, 8, 18, 24, 31, 13);
  sprRect(s, 8, 18, 24, 18, 15);
  sprRect(s, 14, 20, 18, 29, 3); sprRect(s, 11, 23, 21, 26, 3);
  /* ammo: two clips */
  s = spr[SP_AMMO];
  sprRect(s, 9, 22, 15, 31, 6);  sprRect(s, 9, 22, 15, 23, 11);
  sprRect(s, 17, 20, 23, 31, 7); sprRect(s, 17, 20, 23, 21, 12);
}

static void makeTables() {
  for (int s = 0; s < 16; s++)
    for (int v = 0; v < 16; v++)
      shadeLut[s][v] = (uint8_t)lroundf(v * (1.0f - s / 20.0f));
  /* ceiling: bright overhead, dark at the horizon; floor the other way.
   * Two dither phases per row turn the 16 levels into smooth gradients. */
  const float half = VH / 2.0f;
  for (int y = 0; y < VH; y++) {
    float v = (y < half) ? 1.0f + 4.0f * (1.0f - y / half)
                         : 1.0f + 6.5f * ((y - half) / half);
    bgRow[y][0] = clamp15((int)(v + 0.25f));
    bgRow[y][1] = clamp15((int)(v + 0.75f));
  }
}

/**------------ levels ------------**/
static void addEnt(float x, float y, EType t) {
  if (nEnts >= MAXE) return;
  Ent &e = ents[nEnts++];
  e = Ent{};
  e.x = x; e.y = y; e.type = t; e.st = S_IDLE;
  e.hp = (t == E_ENEMY) ? 30 : 0;
  e.cool = 1.0f + frand();
}

static void parseRow(int r, const char *row, int len) {
  for (int c = 0; c < len && c < MAXM; c++) {
    const char ch = row[c];
    uint8_t cell = 0;
    if (ch == '#') cell = 1;
    else if (ch >= '1' && ch <= '5') cell = ch - '0';
    else if (ch == 'P') { px = c + 0.5f; py = r + 0.5f; }
    else if (ch == 'e') addEnt(c + 0.5f, r + 0.5f, E_ENEMY);
    else if (ch == 'h') addEnt(c + 0.5f, r + 0.5f, E_HEALTH);
    else if (ch == 'a') addEnt(c + 0.5f, r + 0.5f, E_AMMO);
    cells[r][c] = cell;
    if (c + 1 > mapW) mapW = c + 1;
  }
}

static bool loadLevel(int n) {
  memset(cells, 0, sizeof(cells));
  mapW = mapH = 0; nEnts = 0;
  px = py = 1.5f;
  char path[16];
  snprintf(path, sizeof(path), "/lvl/%d.txt", n);
  File f = LittleFS.open(path, "r");
  if (f) {
    while (f.available() && mapH < MAXM) {
      String line = f.readStringUntil('\n');
      line.replace("\r", "");
      if (!line.length()) continue;
      parseRow(mapH++, line.c_str(), line.length());
    }
    f.close();
  } else if (n >= 1 && n <= NBUILTIN) {
    for (int r = 0; r < BUILTIN_ROWS[n - 1] && r < MAXM; r++)
      parseRow(mapH++, BUILTIN[n - 1][r], strlen(BUILTIN[n - 1][r]));
  } else {
    return false;
  }
  if (!mapH) return false;
  dirX = 1; dirY = 0; planeX = 0; planeY = 0.66f;
  nEnemies = 0;
  for (int i = 0; i < nEnts; i++) if (ents[i].type == E_ENEMY) nEnemies++;
  kills = 0;
  level = n;
  return true;
}

/**------------ world queries ------------**/
static inline uint8_t cellAt(int x, int y) {
  if ((unsigned)x >= mapW || (unsigned)y >= mapH) return 1;
  return cells[y][x];
}

static bool blocked(float x, float y, float r, bool *exitHit = nullptr) {
  bool b = false;
  for (int i = 0; i < 4; i++) {
    const uint8_t c = cellAt((int)(x + ((i & 1) ? r : -r)), (int)(y + ((i & 2) ? r : -r)));
    if (c) { b = true; if (c == 5 && exitHit) *exitHit = true; }
  }
  return b;
}

static bool lineOfSight(float x0, float y0, float x1, float y1) {
  const float dx = x1 - x0, dy = y1 - y0, d = sqrtf(dx * dx + dy * dy);
  const int n = (int)(d / 0.1f);
  for (int i = 1; i < n; i++) {
    const float t = (float)i / n;
    if (cellAt((int)(x0 + dx * t), (int)(y0 + dy * t))) return false;
  }
  return true;
}

/**------------ simulation ------------**/
static uint16_t readKeys() {
  if (millis() - keysAt > 800) return 0;       // page went quiet: release everything
  return keys;
}

static void startGame() {
  hp = 100; ammo = 30;
  loadLevel(1);
  gst = G_PLAY; stateT = hurtT = fireT = fireCool = 0;
}

static void playerShoot() {
  if (ammo <= 0) { PLAY(SND_CLICK); fireCool = 0.35f; return; }
  ammo--; fireCool = 0.35f; fireT = 0.12f;
  PLAY(SND_SHOT);
  Ent *best = nullptr;
  for (int i = 0; i < nEnts; i++) {
    Ent &e = ents[i];
    if (e.type != E_ENEMY || e.st == S_DEAD || e.st == S_GONE || e.scrHalf <= 0) continue;
    if (abs(e.scrX - GW / 2) > e.scrHalf * 6 / 10) continue;
    if (e.depth >= zbuf[GW / 2]) continue;                   // behind a wall
    if (!best || e.depth < best->depth) best = &e;
  }
  if (!best) return;
  best->hp -= 8 + rnd() % 11;
  if (best->hp <= 0) {
    best->st = S_DEAD; kills++;
    PLAY(SND_EDIE);
  } else {
    best->st = S_PAIN; best->timer = 0.2f;
  }
}

static void enemyThink(Ent &e, float dt) {
  const float dx = px - e.x, dy = py - e.y, d = sqrtf(dx * dx + dy * dy);
  e.timer -= dt; e.cool -= dt;
  const bool sees = d < 14 && lineOfSight(e.x, e.y, px, py);
  switch (e.st) {
    case S_IDLE:
      if (sees || d < 3) { e.st = S_CHASE; PLAY(SND_ALERT); }
      break;
    case S_CHASE:
      if (d > 1.0f && d < 16) {                              // close in
        const float sp = 1.3f * dt / d;
        const float nx = e.x + dx * sp, ny = e.y + dy * sp;
        if (!blocked(nx, e.y, 0.3f)) e.x = nx;
        if (!blocked(e.x, ny, 0.3f)) e.y = ny;
      }
      if (sees && e.cool <= 0 && d < 10) {                   // shoot
        e.st = S_FIRE; e.timer = 0.35f; e.cool = 1.2f + frand() * 1.5f;
        PLAY(SND_ESHOT);
        float chance = 0.75f - d * 0.06f;           // close = dangerous
        if (chance < 0.10f) chance = 0.10f;
        if (frand() < chance) {
          hp -= 3 + rnd() % 7;
          hurtT = 0.25f;
          PLAY(SND_HURT);
        }
      }
      break;
    case S_FIRE:
    case S_PAIN:
      if (e.timer <= 0) e.st = S_CHASE;
      break;
    default:
      break;
  }
}

static void simulate(float dt) {
  const uint16_t k = readKeys();
  stateT += dt;
  if (hurtT > 0) hurtT -= dt;
  if (fireT > 0) fireT -= dt;
  if (fireCool > 0) fireCool -= dt;

  if (gst != G_PLAY) {
    if (stateT > 1.0f && (k & (GK_USE | GK_FIRE))) {
      if (gst == G_DEAD)       { int16_t l = level; startGame(); if (l != 1) loadLevel(l); }
      else if (gst == G_LEVEL) { if (!loadLevel(level + 1)) { gst = G_WON; stateT = 0; return; } }
      else                     startGame();
      gst = G_PLAY; stateT = 0; fireCool = 0.5f;
    }
    return;
  }

  /* turn */
  float rot = 0;
  if (k & GK_TURN_L) rot -= 2.4f * dt;
  if (k & GK_TURN_R) rot += 2.4f * dt;
  if (rot != 0) {
    const float c = cosf(rot), s = sinf(rot);
    float ox = dirX;   dirX = dirX * c - dirY * s;   dirY = ox * s + dirY * c;
    ox = planeX;       planeX = planeX * c - planeY * s; planeY = ox * s + planeY * c;
  }
  /* move + strafe (the camera plane points right) */
  float mf = 0, ms = 0;
  if (k & GK_FWD) mf += 1;
  if (k & GK_BACK) mf -= 1;
  if (k & GK_STRAFE_R) ms += 1;
  if (k & GK_STRAFE_L) ms -= 1;
  if (mf != 0 || ms != 0) {
    const float n = (mf != 0 && ms != 0) ? 0.7071f : 1.0f;
    const float sp = 2.8f * dt * n, rx = planeX / 0.66f, ry = planeY / 0.66f;
    const float vx = (dirX * mf + rx * ms) * sp, vy = (dirY * mf + ry * ms) * sp;
    bool exitHit = false;
    if (!blocked(px + vx, py, 0.25f, &exitHit)) px += vx;
    if (!blocked(px, py + vy, 0.25f, &exitHit)) py += vy;
    walkPhase += dt * 9;
    if (exitHit) { gst = G_LEVEL; stateT = 0; PLAY(SND_LEVEL); return; }
  }
  /* fire */
  if ((k & GK_FIRE) && fireCool <= 0) playerShoot();
  /* pickups */
  for (int i = 0; i < nEnts; i++) {
    Ent &e = ents[i];
    if (e.type == E_ENEMY || e.st == S_GONE) continue;
    if (fabsf(e.x - px) > 0.6f || fabsf(e.y - py) > 0.6f) continue;
    if (e.type == E_HEALTH && hp >= 100) continue;
    if (e.type == E_HEALTH) hp = min(100, hp + 25);
    else                    ammo = min(99, ammo + 12);
    e.st = S_GONE;
    PLAY(SND_PICK);
  }
  /* enemies */
  for (int i = 0; i < nEnts; i++)
    if (ents[i].type == E_ENEMY && ents[i].st != S_DEAD) enemyThink(ents[i], dt);
  if (hp <= 0) { hp = 0; gst = G_DEAD; stateT = 0; PLAY(SND_DEATH); }
}

/**------------ rendering ------------**/
static void drawBackground() {
  for (int y = 0; y < VH; y++) {
    uint8_t *row = view + y * GW;
    const uint8_t a = bgRow[y][y & 1], b = bgRow[y][(y & 1) ^ 1];
    for (int x = 0; x < GW; x += 2) { row[x] = a; row[x + 1] = b; }
  }
}

static void drawWalls() {
  for (int x = 0; x < GW; x++) {
    const float cam = 2.0f * x / GW - 1.0f;
    const float rdx = dirX + planeX * cam, rdy = dirY + planeY * cam;
    int mx = (int)px, my = (int)py;
    const float ddx = (rdx == 0) ? 1e30f : fabsf(1.0f / rdx);
    const float ddy = (rdy == 0) ? 1e30f : fabsf(1.0f / rdy);
    int sx, sy;
    float sdx, sdy;
    if (rdx < 0) { sx = -1; sdx = (px - mx) * ddx; } else { sx = 1; sdx = (mx + 1.0f - px) * ddx; }
    if (rdy < 0) { sy = -1; sdy = (py - my) * ddy; } else { sy = 1; sdy = (my + 1.0f - py) * ddy; }
    int side = 0;
    uint8_t c = 0;
    for (int n = 0; n < 64 && !c; n++) {
      if (sdx < sdy) { sdx += ddx; mx += sx; side = 0; }
      else           { sdy += ddy; my += sy; side = 1; }
      c = cellAt(mx, my);
    }
    if (!c) c = 1;
    float perp = side == 0 ? sdx - ddx : sdy - ddy;
    if (perp < 0.05f) perp = 0.05f;
    zbuf[x] = perp;

    const int lineH = (int)(VH / perp);
    const int y0 = VH / 2 - lineH / 2;
    const int cy0 = y0 < 0 ? 0 : y0, cy1 = (y0 + lineH > VH) ? VH : y0 + lineH;
    float wallX = side == 0 ? py + perp * rdy : px + perp * rdx;
    wallX -= floorf(wallX);
    int tx = (int)(wallX * TEX);
    if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) tx = TEX - tx - 1;
    const uint8_t *col = tex[c] + (tx & (TEX - 1)) * TEX;
    int dark = (int)(perp * 1.5f) + (side ? 2 : 0);
    const uint8_t *lut = shadeLut[dark > 15 ? 15 : dark];
    const uint32_t step = (uint32_t)((TEX << 16) / (lineH ? lineH : 1));
    uint32_t tp = (uint32_t)(cy0 - y0) * step;
    uint8_t *dst = view + cy0 * GW + x;
    for (int y = cy0; y < cy1; y++, dst += GW, tp += step) *dst = lut[col[(tp >> 16) & (TEX - 1)]];
  }
}

static void drawSprites() {
  const float invDet = 1.0f / (planeX * dirY - dirX * planeY);
  uint8_t order[MAXE];
  uint8_t n = 0;
  for (int i = 0; i < nEnts; i++) {
    Ent &e = ents[i];
    e.scrHalf = 0;
    if (e.st == S_GONE) continue;
    const float dx = e.x - px, dy = e.y - py;
    const float tX = invDet * (dirY * dx - dirX * dy);
    const float tY = invDet * (-planeY * dx + planeX * dy);
    e.depth = tY;
    if (tY < 0.15f) continue;
    e.scrX = (int16_t)((GW / 2) * (1 + tX / tY));
    order[n++] = i;
  }
  for (int i = 1; i < n; i++)                          // far to near
    for (int j = i; j > 0 && ents[order[j]].depth > ents[order[j - 1]].depth; j--) {
      uint8_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
    }

  for (int o = 0; o < n; o++) {
    Ent &e = ents[order[o]];
    const uint8_t *img;
    float scale = 1.0f;
    if (e.type == E_HEALTH)      { img = spr[SP_HEALTH]; scale = 0.5f; }
    else if (e.type == E_AMMO)   { img = spr[SP_AMMO];   scale = 0.5f; }
    else if (e.st == S_DEAD)     img = spr[SP_DEAD];
    else if (e.st == S_FIRE)     img = spr[SP_FIRE];
    else if (e.st == S_PAIN)     img = spr[SP_PAIN];
    else                         img = spr[SP_ENEMY];

    const int full = (int)(VH / e.depth);            // a wall-high sprite
    const int h = (int)(full * scale);
    if (h <= 0) continue;
    const int top = VH / 2 + full / 2 - h;           // stands on the floor
    const int left = e.scrX - h / 2;
    e.scrHalf = h / 2;
    int dark = (int)(e.depth * 1.5f);
    const uint8_t *lut = shadeLut[dark > 15 ? 15 : dark];
    const uint32_t step = (uint32_t)((TEX << 16) / h);
    for (int x = left < 0 ? 0 : left; x < left + h && x < GW; x++) {
      if (e.depth >= zbuf[x]) continue;
      const uint8_t *col = img + (((uint32_t)(x - left) * step) >> 16) * TEX;
      const int y0 = top < 0 ? 0 : top, y1 = (top + h > VH) ? VH : top + h;
      uint32_t tp = (uint32_t)(y0 - top) * step;
      uint8_t *dst = view + y0 * GW + x;
      for (int y = y0; y < y1; y++, dst += GW, tp += step) {
        const uint8_t v = col[(tp >> 16) & (TEX - 1)];
        if (v != TRANSP) *dst = lut[v];
      }
    }
  }
}

static void viewRect(int x0, int y0, int x1, int y1, uint8_t v) {
  for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < VH; y++)
    for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < GW; x++) view[y * GW + x] = v;
}

static void drawGun() {
  const bool moving = (readKeys() & (GK_FWD | GK_BACK | GK_STRAFE_L | GK_STRAFE_R)) != 0;
  const int bx = moving ? (int)(3 * sinf(walkPhase)) : 0;
  const int by = (moving ? (int)(2 * fabsf(cosf(walkPhase))) : 0) - (fireT > 0 ? 3 : 0);
  const int cx = GW / 2 + bx, base = VH + by;
  if (fireT > 0.04f) {                                      // muzzle flash
    for (int y = -7; y <= 7; y++)
      for (int x = -7; x <= 7; x++) {
        const int r2 = x * x + y * y;
        if (r2 <= 49 && (rnd() & 3)) {
          const int vx = cx + x, vy = base - 34 + y;
          if ((unsigned)vx < GW && (unsigned)vy < VH) view[vy * GW + vx] = r2 < 16 ? 15 : 12;
        }
      }
  }
  viewRect(cx - 3, base - 28, cx + 3, base - 12, 5);        // barrel
  viewRect(cx - 3, base - 28, cx - 2, base - 12, 9);        // highlight
  viewRect(cx - 1, base - 30, cx + 1, base - 28, 3);        // muzzle
  viewRect(cx - 8, base - 14, cx + 8, base, 4);             // slide / hand
  viewRect(cx - 8, base - 14, cx + 8, base - 13, 8);
  viewRect(cx - 5, base - 10, cx + 5, base, 6);             // grip
}

static void present() {
  /* hurt: flash the view brighter */
  if (hurtT > 0)
    for (int i = 0; i < GW * VH; i++) view[i] = clamp15(view[i] + 4);
  if (gst == G_DEAD)                                        // fade to dark
    for (int i = 0; i < GW * VH; i++) view[i] >>= 1;

  /* Single-buffered, racing the beam: wait for the field to end, then copy
   * top to bottom. A view row covers two screen lines (127us of beam) and
   * takes a few us to copy, so the copy -- and the text after it -- is done
   * long before the beam reaches it, and nothing tears. Saves the 38KB a
   * second framebuffer would cost. */
  Composite.waitFrames(1);
  /* 160x112 -> 320x224: one byte holds two identical grey pixels */
  uint8_t *fb = Composite.frame();
  for (int y = 0; y < VH; y++) {
    const uint8_t *s = view + y * GW;
    uint8_t *d = fb + (2 * y) * FB_STRIDE;
    for (int x = 0; x < GW; x++) d[x] = s[x] * 0x11;
    memcpy(d + FB_STRIDE, d, FB_STRIDE);
  }

  /* HUD with TVout32's own text */
  memset(fb + HUD_Y * FB_STRIDE, 0x00, 16 * FB_STRIDE);
  memset(fb + HUD_Y * FB_STRIDE, 0x88, FB_STRIDE);
  const uint8_t ts = TV.text_size();
  TV.set_text_size(1);
  TV.set_cursor(4, HUD_Y + 5);
  TV.printf("HP %3d  AMMO %2d  KILLS %d/%d  L%d", hp, ammo, kills, nEnemies, level);
  TV.set_cursor(FB_W - 8 * 5 - 2, HUD_Y + 5);
  TV.printf("%2dFPS", fps);

  /* banners */
  const char *l1 = nullptr, *l2 = nullptr;
  if (gst == G_DEAD)       { l1 = "YOU DIED";       l2 = "FIRE TO RETRY"; }
  else if (gst == G_LEVEL) { l1 = "LEVEL COMPLETE"; l2 = "FIRE TO GO ON"; }
  else if (gst == G_WON)   { l1 = "YOU WIN!";       l2 = "FIRE TO REPLAY"; }
  if (l1) {
    TV.set_text_size(2);
    TV.set_cursor((FB_W - strlen(l1) * 16) / 2, 80);
    TV.print(l1);
    if (stateT > 1.0f) {
      TV.set_text_size(1);
      TV.set_cursor((FB_W - strlen(l2) * 8) / 2, 110);
      TV.print(l2);
    }
  }
  TV.set_text_size(ts);
}

/**------------ API ------------**/
void game_keys(uint16_t mask) {
  keys = mask;
  keysAt = millis();
}

void game_frame() {
  if (!running) {
    const size_t texB = 6 * TEX * TEX, sprB = NSPR * TEX * TEX;
    if (!(gameMem = (uint8_t *)malloc(texB + sprB + GW * VH))) {   // ~30KB
      TV.set_cursor(0, 0); TV.print("NO RAM FOR GAME"); TV.delay(500);
      return;
    }
    tex  = (uint8_t (*)[TEX * TEX])gameMem;
    spr  = (uint8_t (*)[TEX * TEX])(gameMem + texB);
    view = gameMem + texB + sprB;
    makeTextures(); makeSprites(); makeTables();
    rng ^= esp_random();
    startGame();
    lastMs = fpsT0 = millis();
    fpsN = fps = 0;
    running = true;
  }
  const uint32_t now = millis();
  float dt = (now - lastMs) / 1000.0f;
  lastMs = now;
  if (dt > 0.1f) dt = 0.1f;                             // after a stall, do not jump

  simulate(dt);
  drawBackground();
  drawWalls();
  drawSprites();
  if (gst == G_PLAY) drawGun();
  present();

  fpsN++;
  if (now - fpsT0 >= 1000) { fps = fpsN; fpsN = 0; fpsT0 = now; }
}

void game_end() {
  if (!running) return;
  running = false;
  free(gameMem);
  gameMem = view = nullptr; tex = spr = nullptr;
  sound_stop();
}

String game_status() {
  return String(hp) + "," + ammo + "," + kills + "," + nEnemies + "," + level + "," + fps +
         "," + (int)gst;
}
