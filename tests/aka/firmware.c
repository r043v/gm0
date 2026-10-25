#include <stdint.h>
extern uint32_t m0_micros(void);
extern uint8_t esp32_buttons(void);
extern void m0_lcd_init(void);
extern uint32_t m0_lcd_send_rows(const void *, uint8_t, uint8_t);
extern int m0_sd_init(void), m0_sd_read(uint32_t, uint8_t *),
    m0_sd_write(uint32_t, const uint8_t *);
volatile uint16_t audio_head, audio_tail;
uint8_t audio_ring[1024];
static uint16_t pixels[160 * 64];
static uint8_t sector[512];
static uint16_t be(uint16_t v) { return (v << 8) | (v >> 8); }
void m0boy_console_main(void) {
  m0_lcd_init();
  if (m0_sd_init() && m0_sd_read(5, sector)) {
    sector[0] = 'A';
    sector[1] = 'K';
    sector[2] = 'A';
    m0_sd_write(5, sector);
  }
  for (;;) {
    uint32_t then = m0_micros();
    uint16_t marker = (esp32_buttons() & 4) ? 0x001f : 0xf800;
    for (unsigned y = 0; y < 64; y++)
      for (unsigned x = 0; x < 160; x++)
        pixels[y * 160 + x] = be(x < 40 ? marker : y < 32 ? 0x07e0 : 0xffff);
    m0_lcd_send_rows(pixels, 0, 64);
    for (unsigned i = 0; i < 160 * 64; i++)
      pixels[i] = be(0x001f);
    m0_lcd_send_rows(pixels, 64, 64);
    for (unsigned i = 0; i < 183; i++) {
      uint16_t next = (audio_head + 1) & 1023;
      if (next == audio_tail)
        break;
      audio_ring[audio_head] = (i & 16) ? 80 : 176;
      audio_head = next;
    }
    while ((uint32_t)(m0_micros() - then) < 16667) {
    }
  }
}
