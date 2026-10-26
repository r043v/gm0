/* Linked service entry points. The emulator replaces these with HLE traps. */
#include <stdint.h>
#define STUB __attribute__((noinline))
STUB uint32_t m0_micros(void) {
  __asm__ volatile("" ::: "memory");
  return 0;
}
STUB uint8_t esp32_buttons(void) {
  __asm__ volatile("" ::: "memory");
  return 0;
}
STUB uint16_t gb_ll_expander_read(void) {
  __asm__ volatile("" ::: "memory");
  return 0;
}
STUB int gb_ll_adc_read_joyx(void) { __asm__ volatile("" ::: "memory"); return 0; }
STUB int gb_ll_adc_read_joyy(void) { __asm__ volatile("" ::: "memory"); return 0; }
STUB int gb_ll_adc_read_vbatt_mv(void) { __asm__ volatile("" ::: "memory"); return 0; }
STUB int gb_ll_adc_read_vbatt_percent(void) { __asm__ volatile("" ::: "memory"); return 0; }
/* Same ABI symbol as the library method: its body must run as guest code. */
STUB void joystick_update(uint16_t *value) __asm__("_ZN11gb_joystick6updateEv");
STUB void joystick_update(uint16_t *value) { *value = gb_ll_adc_read_joyx(); }
STUB void m0_lcd_init(void) { __asm__ volatile("" ::: "memory"); }
STUB uint32_t m0_lcd_send_rows(const void *p, uint8_t a, uint8_t b) {
  (void)p;
  (void)a;
  (void)b;
  __asm__ volatile("" ::: "memory");
  return 0;
}
STUB int m0_sd_init(void) {
  __asm__ volatile("" ::: "memory");
  return 0;
}
STUB int m0_sd_read(uint32_t a, uint8_t *p) {
  (void)a;
  (void)p;
  __asm__ volatile("" ::: "memory");
  return 0;
}
STUB int m0_sd_write(uint32_t a, const uint8_t *p) {
  (void)a;
  (void)p;
  __asm__ volatile("" ::: "memory");
  return 0;
}
