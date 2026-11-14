#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
/*********************
* sound - LEDC square-wave tone player.
*
* Composite video carries no audio, so this goes out its own RCA (the white
* one) on a separate pin:
*
*   GPIO32 --[ 1k ]--+-- audio centre pin
*                    |
*                  10nF        (rounds the square edges a little)
*   GND ------------+-- audio shield
*
* Into a powered speaker / amp input. For a bare 8R speaker put a small
* transistor or a series 100R + 100uF in between -- do not hang a speaker coil
* straight off the GPIO.
*
* LEDC is independent PWM hardware: once a tone is set it runs with zero CPU
* and cannot disturb the video task on core 1. The melody player is a small
* task on core 0 that just changes the frequency on cue.
**********************/
#pragma once
#include <Arduino.h>

#define SND_PIN  32

/**One entry of a melody. freq in Hz, 0 = rest. dur in ms.**/
struct Tone {
  uint16_t freq;
  uint16_t dur;
};

/**Note frequencies, octaves 4-5.**/
#define N_G4  392
#define N_A4  440
#define N_B4  494
#define N_C5  523
#define N_D5  587
#define N_E5  659
#define N_F5  698
#define N_G5  784
#define N_A5  880
#define N_REST  0

void sound_begin();

/**Start a melody. Returns immediately; a previously playing melody is
 * replaced. `m` must stay alive (use PROGMEM/static tables).**/
void sound_play(const Tone *m, uint16_t len);

/**Stop whatever is playing.**/
void sound_stop();

bool sound_busy();

/**Built-in tunes**/
void snd_birthday();
void snd_beep();
void snd_startup();

/**Look a melody up by the name used on the tvout/snd topic.
 * Returns false if the name is unknown.**/
bool sound_by_name(const char *name);

#endif  // TVOUT_ENGINE
