#ifdef TVOUT_ENGINE   // hidden from ESPHome's main.cpp, see tvout_component.h
/*********************
* CompositeNTSC - 240p NTSC composite video out for ESP32 (classic / WROOM-32 only)
*
* Generates a 4bpp (16 grey levels) framebuffer onto the ESP32 built-in DAC (GPIO25) via I2S0 in
* I2S_MODE_DAC_BUILT_IN. A dedicated FreeRTOS task pinned to core 1 pushes one
* scanline at a time; i2s_write() blocks on DMA so the task is self-pacing and
* WiFi/MQTT on core 0 cannot disturb the timing.
*
* The I2S setup follows bitluni's ESP32CompositeVideo (CC0); the clock divider
* is re-measured for IDF 5 (core 3.x): 160MHz / (6 x 2) = 13.3333 MSa/s.
*
* REQUIRES the classic ESP32 (or ESP32-S2). ESP32-S3/C3/C6 have no DAC.
*
* Wiring:  GPIO25 --[ 180R ]--+-- composite centre pin
*                             |
*                            75R  (inside the TV, not fitted by you)
*          GND ---------------+-- composite shield
*
* 180R sets 1Vpp at the TV: 3.3V * 75/(180+75) = 0.97V. See README.md.
**********************/
#pragma once
#include <Arduino.h>

/**Framebuffer geometry. 4bpp grey, two pixels per byte, high nibble = left
 * pixel. 0 = black ... 15 = white.**/
#define FB_W       320                 // must divide ACTIVE_SAMPLES evenly (160 or 320)
#define FB_H       240                 // must divide ACTIVE_LINES evenly (120 or 240)
#define FB_BPP     4
#define FB_LEVELS  16
#define FB_STRIDE  (FB_W / 2)          // 160 bytes per line
#define FB_SIZE    (FB_STRIDE * FB_H)  // 38400 bytes

/**Sample-level timing at 13.3333 MSa/s. In DAC mode every sample is one
 * 16-bit I2S word (the DAC takes its high byte) and the hardware emits the
 * words of each 32-bit FIFO entry swapped, so sample i lives at index i^1.
 * Keep ACTIVE_START and the per-byte span even so the LUT can be copied in
 * pre-swapped.**/
#define SAMPLES_LINE    848            // 63.6us line
#define SAMPLES_HSYNC    64            // 4.8us sync tip
#define SAMPLES_BPORCH   62            // 4.65us back porch
#define ACTIVE_START    156            // first picture sample
#define ACTIVE_SAMPLES  640            // 48us of picture
#define PIX_SAMPLES     (ACTIVE_SAMPLES / FB_W)     // samples per pixel
#define BYTE_SAMPLES    (2 * PIX_SAMPLES)           // samples per framebuffer byte

/**DAC levels over a nominal 1Vpp composite swing. Trim LEVEL_WHITE down if the
 * picture blooms, or the series resistor up. Grey k (0..15) is spaced evenly
 * between LEVEL_BLACK and LEVEL_WHITE: composite video is already gamma
 * encoded, so linear steps here look like linear steps of image luma.**/
#define LEVEL_SYNC        0
#define LEVEL_BLANK      73
#define LEVEL_BLACK      86
#define LEVEL_WHITE     255

/**262-line progressive field (240p). Most TVs lock to this happily and it
 * avoids the field-shift artefacts of real interlace.**/
#define LINES_TOTAL     262
#define LINES_VSYNC       3            // broad pulses
#define LINES_EQ          3            // equalizing
#define LINES_BLANK_TOP  13
#define LINE_ACTIVE_FIRST (LINES_VSYNC + LINES_EQ + LINES_BLANK_TOP)   // 19
#define ACTIVE_LINES    240
#define LINE_REPEAT     (ACTIVE_LINES / FB_H)       // 2 = line doubling at 120 rows

#if (ACTIVE_SAMPLES % FB_W) || (ACTIVE_LINES % FB_H) || (ACTIVE_START & 1)
#error "FB_W/FB_H must divide the active area evenly and ACTIVE_START must be even"
#endif

class CompositeNTSC {
  public:
    /**Allocate the framebuffer, start I2S and the core-1 video task.
     * Returns false if allocation or I2S setup failed.**/
    bool begin();

    /**Framebuffer access. Nothing here is clipped -- callers clip.**/
    uint8_t *frame() { return fb; }

    /**Frame counter, incremented once per field by the video task.**/
    uint32_t frames() const { return frameCount; }

    /**Block until n further fields have been output (the TVout delay_frame).
     * Returns right at the field boundary: the next ~25 lines (~1.6ms) are
     * sync/blanking, so drawing then does not tear. Woken by a semaphore, not
     * polled, so the window is not lost to a 1ms tick.**/
    void waitFrames(uint8_t n);

  private:
    uint8_t *fb        = nullptr;      // 4bpp framebuffer
    uint16_t *lnVsync  = nullptr;      // prebuilt scanlines of DAC samples (value << 8)
    uint16_t *lnEq     = nullptr;
    uint16_t *lnBlank  = nullptr;
    uint16_t *lnActive = nullptr;      // sync/porches prebuilt, picture refilled per line
    uint16_t (*lut)[BYTE_SAMPLES] = nullptr;   // byte -> samples, pre-swapped

    volatile uint32_t frameCount = 0;
    SemaphoreHandle_t vblank = nullptr;     // given once per field

    void buildPixelLUT();
    void fillBlankLine(uint16_t *ln);
    void fillVsyncLine(uint16_t *ln);
    void fillEqLine(uint16_t *ln);
    void fillActiveLine(int y);
    void sendLine(const uint16_t *ln);
    void videoLoop();

    static void videoTask(void *arg);
};

extern CompositeNTSC Composite;

#endif  // TVOUT_ENGINE
