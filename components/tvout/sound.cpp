#define TVOUT_ENGINE
#include "sound.h"

/**A unit is a quaver; "Happy Birthday" in C, starting on G4.**/
#define U 160

static const Tone melody_birthday[] = {
  {N_G4, U},   {N_G4, U},   {N_A4, U*2}, {N_G4, U*2}, {N_C5, U*2}, {N_B4, U*4}, {N_REST, U},
  {N_G4, U},   {N_G4, U},   {N_A4, U*2}, {N_G4, U*2}, {N_D5, U*2}, {N_C5, U*4}, {N_REST, U},
  {N_G4, U},   {N_G4, U},   {N_G5, U*2}, {N_E5, U*2}, {N_C5, U*2}, {N_B4, U*2}, {N_A4, U*4},
  {N_REST, U},
  {N_F5, U},   {N_F5, U},   {N_E5, U*2}, {N_C5, U*2}, {N_D5, U*2}, {N_C5, U*5},
};

static const Tone melody_beep[] = {
  {N_C5, 60}, {N_REST, 30}, {N_G5, 80},
};

static const Tone melody_startup[] = {
  {N_C5, 70}, {N_E5, 70}, {N_G5, 70}, {N_A5, 160},
};

/**Player state, written by the API, read by the task.**/
static const Tone * volatile cur = nullptr;
static volatile uint16_t cur_len = 0;
static volatile uint32_t seq = 0;          // bumped on every new melody
static volatile bool     busy = false;

static void soundTask(void *) {
  uint32_t mine = 0;
  for (;;) {
    if (seq != mine) {
      mine = seq;
      const Tone *m = cur;
      uint16_t n = cur_len;
      busy = true;
      for (uint16_t i = 0; i < n; i++) {
        if (seq != mine) break;            // superseded, drop this melody
        if (m[i].freq) ledcWriteTone(SND_PIN, m[i].freq);
        else           ledcWrite(SND_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(m[i].dur));
        ledcWrite(SND_PIN, 0);             // tiny gap so repeated notes articulate
        vTaskDelay(pdMS_TO_TICKS(12));
      }
      if (seq == mine) { ledcWrite(SND_PIN, 0); busy = false; }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void sound_begin() {
  if (!ledcAttach(SND_PIN, 1000, 10)) {    // 10-bit; frequency is set per note
    Serial.printf("sound: LEDC attach on GPIO%d failed\n", SND_PIN);
    return;
  }
  ledcWrite(SND_PIN, 0);
  Serial.printf("sound: LEDC on GPIO%d ok\n", SND_PIN);
  xTaskCreatePinnedToCore(soundTask, "snd", 2048, nullptr, 2, nullptr, 0);
}

void sound_play(const Tone *m, uint16_t len) {
  cur = m;
  cur_len = len;
  seq = seq + 1;                           // wakes the task, cancels any current melody
}

void sound_stop() {
  cur = nullptr;
  cur_len = 0;
  seq = seq + 1;
  ledcWrite(SND_PIN, 0);
  busy = false;
}

bool sound_busy() { return busy; }

void snd_birthday() { sound_play(melody_birthday, sizeof(melody_birthday) / sizeof(Tone)); }
void snd_beep()     { sound_play(melody_beep,     sizeof(melody_beep)     / sizeof(Tone)); }
void snd_startup()  { sound_play(melody_startup,  sizeof(melody_startup)  / sizeof(Tone)); }

bool sound_by_name(const char *name) {
  if      (!strcasecmp(name, "birthday")) snd_birthday();
  else if (!strcasecmp(name, "beep"))     snd_beep();
  else if (!strcasecmp(name, "startup"))  snd_startup();
  else if (!strcasecmp(name, "off"))      sound_stop();
  else return false;
  return true;
}
