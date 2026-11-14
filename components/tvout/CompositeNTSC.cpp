#define TVOUT_ENGINE
#include "CompositeNTSC.h"
#include "driver/i2s.h"
#include "soc/i2s_reg.h"
#include "soc/rtc.h"

CompositeNTSC Composite;

/**Write `len` samples of `level` starting at sample `from`. Each sample is a
 * 16-bit word holding the DAC code in its high byte, and the I2S emits the two
 * words of every 32-bit FIFO entry in swapped order -- hence i^1.**/
static inline void fillSamples(uint16_t *line, int from, int len, uint8_t level) {
  const uint16_t v = (uint16_t)level << 8;
  for (int i = from; i < from + len; i++) line[i ^ 1] = v;
}

/**Build the byte (two grey pixels) -> BYTE_SAMPLES expansion table, already
 * in the swapped order, so it can be memcpy'd straight to an even offset.**/
void CompositeNTSC::buildPixelLUT() {
  for (int b = 0; b < 256; b++) {
    for (int s = 0; s < BYTE_SAMPLES; s++) {
      const int g = (s < PIX_SAMPLES) ? (b >> 4) : (b & 15);
      const uint8_t lv = LEVEL_BLACK + ((LEVEL_WHITE - LEVEL_BLACK) * g + 7) / 15;
      lut[b][s ^ 1] = (uint16_t)lv << 8;
    }
  }
}

/**Ordinary line: sync tip, then blanking for the rest.**/
void CompositeNTSC::fillBlankLine(uint16_t *ln) {
  fillSamples(ln, 0, SAMPLES_LINE, LEVEL_BLANK);
  fillSamples(ln, 0, SAMPLES_HSYNC, LEVEL_SYNC);
}

/**Vertical sync: two broad pulses, one per half line.**/
void CompositeNTSC::fillVsyncLine(uint16_t *ln) {
  const int half = SAMPLES_LINE / 2;          // 424
  const int broad = half - SAMPLES_HSYNC;     // 360 sync, 64 blank
  fillSamples(ln, 0,            broad,         LEVEL_SYNC);
  fillSamples(ln, broad,        SAMPLES_HSYNC, LEVEL_BLANK);
  fillSamples(ln, half,         broad,         LEVEL_SYNC);
  fillSamples(ln, half + broad, SAMPLES_HSYNC, LEVEL_BLANK);
}

/**Equalizing: two narrow (2.4us) pulses, one per half line.**/
void CompositeNTSC::fillEqLine(uint16_t *ln) {
  const int half = SAMPLES_LINE / 2;
  const int eq = 32;
  fillSamples(ln, 0, SAMPLES_LINE, LEVEL_BLANK);
  fillSamples(ln, 0, eq, LEVEL_SYNC);
  fillSamples(ln, half, eq, LEVEL_SYNC);
}

/**Picture line y of the framebuffer. Sync and porches never change, so only
 * the active window of the picture buffer is rewritten. Runs 240 times a
 * field, so it lives in IRAM and copies the table entries as 32-bit words --
 * a memcpy() call per byte cost ~31us of every 63.6us line.**/
void IRAM_ATTR CompositeNTSC::fillActiveLine(int y) {
  constexpr int W = BYTE_SAMPLES / 2;                 // 32-bit words per fb byte
  const uint8_t *src = fb + y * FB_STRIDE;
  uint32_t *dst = (uint32_t *)(lnActive + ACTIVE_START);   // ACTIVE_START is even
  const uint32_t *tbl = (const uint32_t *)lut;
  for (int x = 0; x < FB_STRIDE; x++) {
    const uint32_t *e = tbl + src[x] * W;
    for (int k = 0; k < W; k++) *dst++ = e[k];
  }
}

/**Hand a line to the DMA. Blocks until there is room, which is what paces
 * the whole generator -- no timer, no ISR bookkeeping.**/
void CompositeNTSC::sendLine(const uint16_t *ln) {
  size_t written;
  i2s_write(I2S_NUM_0, ln, SAMPLES_LINE * sizeof(uint16_t), &written, portMAX_DELAY);
}

/**Only picture lines do any work per line; the sync lines are prebuilt.**/
void CompositeNTSC::videoLoop() {
  for (;;) {
    int l = 0;
    for (int i = 0; i < LINES_VSYNC; i++, l++)     sendLine(lnVsync);
    for (int i = 0; i < LINES_EQ; i++, l++)        sendLine(lnEq);
    for (int i = 0; i < LINES_BLANK_TOP; i++, l++) sendLine(lnBlank);
    for (int i = 0; i < ACTIVE_LINES; i++, l++)    { fillActiveLine(i / LINE_REPEAT); sendLine(lnActive); }
    for (; l < LINES_TOTAL; l++)                   sendLine(lnBlank);
    frameCount = frameCount + 1;
    xSemaphoreGive(vblank);
  }
}

void CompositeNTSC::videoTask(void *arg) {
  static_cast<CompositeNTSC *>(arg)->videoLoop();
}

void CompositeNTSC::waitFrames(uint8_t n) {
  while (n--) {
    xSemaphoreTake(vblank, 0);                 // drop a stale give
    xSemaphoreTake(vblank, portMAX_DELAY);     // wait for the next field end
  }
}

bool CompositeNTSC::begin() {
  fb   = (uint8_t *)heap_caps_malloc(FB_SIZE, MALLOC_CAP_8BIT);
  lut  = (uint16_t (*)[BYTE_SAMPLES])heap_caps_malloc(256 * sizeof(lut[0]), MALLOC_CAP_8BIT);
  const size_t lnBytes = SAMPLES_LINE * sizeof(uint16_t);
  lnVsync  = (uint16_t *)heap_caps_malloc(lnBytes, MALLOC_CAP_8BIT);
  lnEq     = (uint16_t *)heap_caps_malloc(lnBytes, MALLOC_CAP_8BIT);
  lnBlank  = (uint16_t *)heap_caps_malloc(lnBytes, MALLOC_CAP_8BIT);
  lnActive = (uint16_t *)heap_caps_malloc(lnBytes, MALLOC_CAP_8BIT);
  vblank   = xSemaphoreCreateBinary();
  if (!fb || !lut || !lnVsync || !lnEq || !lnBlank || !lnActive || !vblank) return false;

  memset(fb, 0, FB_SIZE);
  buildPixelLUT();
  fillVsyncLine(lnVsync);
  fillEqLine(lnEq);
  fillBlankLine(lnBlank);
  fillBlankLine(lnActive);

  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate          = 1000000,           // overridden by the divider writes below
    .bits_per_sample      = (i2s_bits_per_sample_t)I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,   // one word per sample, as bitluni
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_MSB,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 6,
    .dma_buf_len          = SAMPLES_LINE / 2,             // frames of 2 words = one line
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0,
    .mclk_multiple        = I2S_MCLK_MULTIPLE_256,
    .bits_per_chan        = I2S_BITS_PER_CHAN_DEFAULT
  };
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
  i2s_set_pin(I2S_NUM_0, NULL);                       // built-in DAC, no external pins
  i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);         // DAC1 -> GPIO25

  /**Force the real conversion rate to 13.3333 MSa/s. On IDF 5 (core 3.x) one
   * DAC sample is consumed every DIV_NUM x BCK_DIV cycles of the 160MHz PLL_D2
   * clock: 160 / (6 x 2) = 13.333 MHz, measured at exactly 63.60us per 848-
   * sample line. bitluni's 2+1/1 setting dates from IDF 3 and on this core
   * saturates at ~42.6us per line, which no TV will lock to.**/
  SET_PERI_REG_BITS(I2S_CLKM_CONF_REG(0),        I2S_CLKM_DIV_A_V,      1, I2S_CLKM_DIV_A_S);
  SET_PERI_REG_BITS(I2S_CLKM_CONF_REG(0),        I2S_CLKM_DIV_B_V,      0, I2S_CLKM_DIV_B_S);
  SET_PERI_REG_BITS(I2S_CLKM_CONF_REG(0),        I2S_CLKM_DIV_NUM_V,    6, I2S_CLKM_DIV_NUM_S);
  SET_PERI_REG_BITS(I2S_SAMPLE_RATE_CONF_REG(0), I2S_TX_BCK_DIV_NUM_V,  2, I2S_TX_BCK_DIV_NUM_S);

  /**Core 1 is ours; Arduino's WiFi/lwIP stack lives on core 0.**/
  xTaskCreatePinnedToCore(videoTask, "ntsc", 4096, this, configMAX_PRIORITIES - 1, NULL, 1);
  return true;
}
