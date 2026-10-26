/* Gamebuino AKA: native ESP32-S3 Xtensa code, host platform services.
 * CPU timing is an estimate. See README for the supported SDK boundary. */
#ifndef GM0_AKA_H
#define GM0_AKA_H
#include <stddef.h>
#include <stdint.h>
int aka_is_image(const uint8_t *, size_t);
int aka_load(const uint8_t *, size_t, const char *symbols, size_t symbols_len);
void aka_run(uint64_t cycles);
void aka_render(uint16_t *out);
void aka_set_keys(uint8_t keys);
/* Additional Aka expander keys: R1=0x40, L1=0x80, D=0x1000, C=0x4000. */
void aka_set_extra_keys(uint16_t keys);
/* SDL-style axes: X positive right, Y positive down. */
void aka_set_joystick(int16_t x, int16_t y);
uint64_t aka_cycles(void);
const char *aka_error(void);
int aka_degraded(void);
extern void aka_audio_sample(int16_t, uint64_t);
extern int aka_sd_read(uint32_t, uint8_t *);
extern int aka_sd_write(uint32_t, const uint8_t *);
#endif
