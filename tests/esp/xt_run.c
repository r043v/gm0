/* Exécute un programme Xtensa nu (ELF sans libc, lié en IRAM/DRAM) sur le
 * cœur de esp8266_xt.h : affiche a2 à l'arrêt (BREAK).  Sert aux tests
 * comparés à l'hôte (tests/esp/run_tests.py). */
#include <stdlib.h>
#include "../../esp8266.h"

static void esp_audio_out(uint8_t v) { (void)v; }
static void esp_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
static uint8_t esp_buttons_pressed(void) { return 0; }

static int load_elf(const char *path, uint32_t *entry) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 52 || memcmp(buf, "\177ELF", 4)) return -1;
    uint32_t phoff, ent;
    uint16_t phentsize, phnum;
    memcpy(entry, buf + 24, 4); memcpy(&phoff, buf + 28, 4);
    memcpy(&phentsize, buf + 42, 2); memcpy(&phnum, buf + 44, 2);
    (void)ent;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = buf + phoff + i * phentsize;
        uint32_t type, off, vaddr, filesz, memsz;
        memcpy(&type, ph, 4); memcpy(&off, ph + 4, 4); memcpy(&vaddr, ph + 8, 4);
        memcpy(&filesz, ph + 16, 4); memcpy(&memsz, ph + 20, 4);
        if (type != 1) continue;
        for (uint32_t k = 0; k < memsz; k++) {
            uint8_t b = k < filesz ? buf[off + k] : 0;
            uint32_t a = vaddr + k;
            if (a - XT_IRAM_BASE < XT_IRAM_SIZE) xt_iram[a - XT_IRAM_BASE] = b;
            else if (a - XT_DRAM_BASE < XT_DRAM_SIZE) xt_dram[a - XT_DRAM_BASE] = b;
            else if (a - XT_IROM_BASE < XT_MAPPED_SIZE) xt_flash[a - XT_IROM_BASE] = b;
            else { fprintf(stderr, "segment hors mémoire : %08x\n", a); return -1; }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    uint32_t entry;
    if (argc < 2 || load_elf(argv[1], &entry)) { fprintf(stderr, "usage: xt_run prog.elf\n"); return 2; }
    memset(xt_ctag, 0xff, sizeof xt_ctag);
    esp.cpu_hz = 160e6;
    xt.pc = entry;
    xt.a[1] = XT_DRAM_BASE + XT_DRAM_SIZE - 16;
    xt.next_event = ~0ull;
    xt.a[2] = argc > 2 ? (uint32_t)atoi(argv[2]) : 99;
    xt_run(2000000000ull);
    if (xt.halted != 2) { printf("FAULT %s\n", xt.msg); return 1; }
    printf("a2=%08x cycles=%llu\n", xt.a[2], (unsigned long long)xt.cycles);
    return 0;
}
