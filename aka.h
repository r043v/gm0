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
uint64_t aka_cycles(void);
const char *aka_error(void);
extern void aka_audio_sample(int16_t, uint64_t);
extern int aka_sd_read(uint32_t, uint8_t *);
extern int aka_sd_write(uint32_t, const uint8_t *);
#endif
