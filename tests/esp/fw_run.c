/* Fait tourner un firmware ESPboy (ELF, ou .bin + .sym) sur la machine de
 * esp8266.h, hors SDL : fw_run fw.elf|fw.bin [rom.bin] [--sym f.sym] [--frames n] [--shot out.ppm]
 * [--keys frame:mask:len,...] ; affiche les compteurs et la faute éventuelle. */
#include <stdlib.h>
#include <time.h>
#include "../../esp8266.h"

static unsigned long audio_changes;
static uint8_t keys_now;
static void esp_audio_out(uint8_t v) { (void)v; audio_changes++; }
static void esp_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
static uint8_t esp_buttons_pressed(void) { return keys_now; }

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f); b[*n] = 0;
    return b;
}

int main(int argc, char **argv) {
    const char *fwp = NULL, *romp = NULL, *symp = NULL, *shot = NULL, *keys = "";
    unsigned frames = 60;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sym") && i + 1 < argc) symp = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!fwp) fwp = argv[i]; else romp = argv[i];
    }
    size_t fn = 0, rn = 0, sn = 0;
    uint8_t *fw = fwp ? slurp(fwp, &fn) : NULL, *rom = romp ? slurp(romp, &rn) : NULL, *sym = symp ? slurp(symp, &sn) : NULL;
    if (!fw) { fprintf(stderr, "usage: fw_run fw.elf|fw.bin [rom.bin] [--sym f] [--frames n] [--shot f.ppm]\n"); return 2; }
    int r = esp_power_on(fw, fn, rom, rn, (const char *)sym, sn);
    if (r) { fprintf(stderr, "chargement impossible (%d)\n", r); return 1; }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t c0 = xt.cycles;
    for (unsigned f = 0; f < frames; f++) {
        keys_now = 0;
        for (const char *q = keys; *q;) {
            unsigned at, m, len;
            if (sscanf(q, "%u:%u:%u", &at, &m, &len) == 3 && f >= at && f < at + len) keys_now |= (uint8_t)m;
            q = strchr(q, 0x2c); if (!q) break; q++;
        }
        esp_run((uint64_t)(esp.cpu_hz / 59.7275));
        if (xt.halted && !esp.restart) break;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("cycles=%llu (%.2f s émulées à %.0f MHz) mur=%.2f s (x%.2f) halted=%d %s\n", (unsigned long long)(xt.cycles - c0),
           (double)(xt.cycles - c0) / esp.cpu_hz, esp.cpu_hz / 1e6, wall, (double)(xt.cycles - c0) / esp.cpu_hz / wall, xt.halted, xt.halted ? xt.msg : "");
    printf("écran : %llu pixels, %llu images, audio : %lu changements ; SPI %llu octets ; flash : %llu lectures %llu écritures %llu effacements (%u écritures sur secteur non effacé) ; défauts de cache flash %u\n",
           (unsigned long long)esp.lcd_pixels, (unsigned long long)esp.lcd_frames, audio_changes, (unsigned long long)esp.spi_bytes,
           (unsigned long long)esp.flash_reads, (unsigned long long)esp.flash_writes, (unsigned long long)esp.flash_erases, esp.flash_bad_writes, xt_flash_misses);
    { uint32_t g = esp_sym("gbl_frame"); /* images GB émulées : estimation de la vitesse */
      if (g) printf("gbl_frame=%u en %.2f s émulées : %.1f images/s (59.7 = pleine vitesse ; estimation, pas une mesure)\n", esp_peek32(g),
                    (double)(xt.cycles - c0) / esp.cpu_hz, esp_peek32(g) / ((double)(xt.cycles - c0) / esp.cpu_hz)); }
    if (shot) {
        FILE *f = fopen(shot, "wb");
        fprintf(f, "P6\n%d %d\n255\n", ESP_SCR_W, ESP_SCR_H);
        for (int i = 0; i < ESP_SCR_W * ESP_SCR_H; i++) {
            uint16_t c = esp_fb[i];
            uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3) };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    return xt.halted && !esp.restart ? 1 : 0;
}
