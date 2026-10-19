/*
 * esp8266.h — machine ESPboy (ESP8266 + ST7735 128x128 + MCP23017) pour gm0.
 *
 * Le cœur Xtensa (esp8266_xt.h) exécute le vrai code du firmware (cœur
 * Arduino, runtime, code généré).  Ce qui n'existe pas ici — la ROM de la
 * puce et le SDK Espressif (binaire) — est ÉMULÉ AU NIVEAU DES FONCTIONS :
 *   - ROM : libc, divisions et virgule flottante logicielles, ets_*, SPI
 *     flash, par adresse (esp8266_rom_syms.h : noms et adresses seulement) ;
 *   - SDK / cœur Arduino : quelques fonctions repérées par leur NOM dans la
 *     table des symboles du firmware (micros, delay, twi_*, spi_flash_*,
 *     system_update_cpu_freq...) ; leur premier mot est remplacé par un ILL
 *     que le cœur redirige ici ;
 *   - périphériques par registres : FRC1 (timer1), GPIO (dont le
 *     sigma-delta), SPI1 vers l'écran, RTC GPIO16 (DC), IOMUX ;
 *   - bus I2C : MCP23017 (boutons, CS de l'écran) et MCP4725 (rétroéclairage)
 *     au niveau de twi_writeTo / twi_readFrom.
 * Le démarrage du SDK (eboot, app_entry, user_init) n'est pas exécuté : la
 * machine prépare ce qu'il laisse (g_pcont, timer1, constructeurs C++) puis
 * appelle setup() sur la pile « cont », puis loop().
 *
 * Le temps est compté en cycles du cœur ; flash et SPI ajoutent des cycles
 * (esp8266_xt.h) : ESTIMATIONS, pas des mesures de la puce.
 */
#ifndef ESP8266_H
#define ESP8266_H

#include "esp8266_xt.h"
#include "esp8266_rom_syms.h"
#include <math.h>

#define ESP_SCR_W 128
#define ESP_SCR_H 128
#define ESP_SAVE_OFF 0x3C0000u
#define ESP_SAVE_END 0x3FB000u
#define ESP_ROM_OFF  0x100000u
#define ESP_ISR_RET  0x4000fff0u
#define ESP_BOOT_NEXT 0x4000ffe0u
#define ESP_LOOP_NEXT 0x4000ffd0u

/* fournis par l'hôte (gm0.c ou le test) */
static void esp_audio_out(uint8_t v);      /* nouvelle valeur du sigma-delta (0..255) */
static void esp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static uint8_t esp_buttons_pressed(void);  /* bit 0 gauche, 1 haut, 2 bas, 3 droite, 4 B, 5 A, 6 gauche aux., 7 droite aux. ; 1 = appuyé */

/* ---------------------------------------------------------------- état */
static uint16_t esp_fb[ESP_SCR_W * ESP_SCR_H];
static uint32_t esp_gram[162][132];       /* mémoire d'image du ST7735 : 18 bits, R 17:12, V 11:6, B 5:0 */
static uint32_t esp_miss_opt;             /* GM0_ESP_FLASH_MISS : défaut de cache d'instructions (optionnel, non validé) */
static struct {
    double   cpu_hz;
    uint64_t cycle_base; double us_base;   /* temps : us_base à cycle_base */
    int      booted, restart;
    /* symboles du firmware */
    uint32_t sym_addr[32768]; char (*sym_name)[40]; unsigned nsym;
    /* ISR */
    struct { uint32_t fn, arg; } isr[16];
    struct { uint32_t a[16], pc, ps, sar, lbeg, lend, lcount; } ctx[4];
    int      isr_depth;
    /* sommeil de delay()/WAITI */
    uint64_t sleep_until; int sleeping;
    /* timer1 */
    uint32_t t1_load, t1_ctrl; uint64_t t1_next; int t1_on;
    /* GPIO / SPI / RTC / IOMUX / générique */
    uint32_t gpio[64], spi1[64], rtc[64], iomux[64], wdt[8], regs[0x800];
    int      dc;                            /* GPIO16 : donnée/commande de l'écran */
    uint8_t  sd_value;                      /* dernier sigma-delta */
    /* I2C */
    uint8_t  mcp[22], mcp_ptr, dac[3];
    /* flash */
    unsigned flash_bad_writes; int flash_dirty;
    uint64_t spi_bytes, flash_reads, flash_writes, flash_erases;
    /* écran */
    int      lcd_cmd, lcd_argn, lcd_npix, lcd_writing, lcd_sleeping, lcd_on, lcd_inv;
    int      lcd_xs, lcd_xe, lcd_ys, lcd_ye, lcd_cx, lcd_cy;
    uint8_t  lcd_args[16], lcd_madctl, lcd_colmod, lcd_pix[3];
    uint64_t lcd_pixels, lcd_frames;
    uint16_t lcd_last_ys;
    uint32_t cont_bytes;                    /* CONT_STACKSIZE du firmware */
    uint32_t scont, stage;                  /* démarrage : structure cont, étape */
    const uint8_t *fw; size_t fwlen; int fw_is_elf; /* image, pour redémarrer */
    int fault_shown;
} esp;

static double esp_us(void) { return esp.us_base + (double)(xt.cycles - esp.cycle_base) * 1e6 / esp.cpu_hz; }
static void esp_set_freq(unsigned mhz) {
    esp.us_base = esp_us();
    esp.cycle_base = xt.cycles;
    esp.cpu_hz = mhz * 1e6;
    /* coûts mesurés à 160 MHz (émulateur de référence) ; temps de bus : moitié en cycles à 80 MHz */
    xt_imem_extra = 5;
    xt_p_read = mhz >= 160 ? 15 : 8;
    xt_p_write = mhz >= 160 ? 10 : 5;
    xt_miss_cycles = esp_miss_opt ? (mhz >= 160 ? esp_miss_opt : esp_miss_opt / 2) : 0;
    uint32_t cpu2x = 0;
    memcpy(&cpu2x, xt_dport + 0x14, 4);
    cpu2x = (cpu2x & ~1u) | (mhz >= 160 ? 1u : 0u);
    memcpy(xt_dport + 0x14, &cpu2x, 4);
}

/* ---------------------------------------------------------------- symboles */
static int esp_cmp_sym(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static uint32_t esp_sym(const char *name) {
    for (unsigned i = 0; i < esp.nsym; i++) if (!strcmp(esp.sym_name[i], name)) return esp.sym_addr[i];
    return 0;
}
static void esp_add_sym(uint32_t addr, const char *name) {
    if (esp.nsym >= 32768 || !esp.sym_name) return;
    esp.sym_addr[esp.nsym] = addr;
    snprintf(esp.sym_name[esp.nsym], 40, "%s", name);
    esp.nsym++;
}
static void esp_symbol_at(uint32_t addr, char *out, size_t n) {
    uint32_t best = 0; const char *bn = "?";
    for (unsigned i = 0; i < esp.nsym; i++)
        if (esp.sym_addr[i] <= addr && esp.sym_addr[i] >= best && esp.sym_addr[i] > 0x3ff00000u) { best = esp.sym_addr[i]; bn = esp.sym_name[i]; }
    snprintf(out, n, "%s+0x%x", bn, addr - best);
}

/* ------------------------------------------------------- accès émulé */
static uint32_t esp_peek32(uint32_t a) { uint32_t off; uint32_t v = 0; if ((off = a - XT_DRAM_BASE) < XT_DRAM_SIZE) memcpy(&v, xt_dram + off, 4); else if ((off = a - XT_IROM_BASE) < XT_MAPPED_SIZE) memcpy(&v, xt_flash + off, 4); else if ((off = a - XT_IRAM_BASE) < XT_IRAM_SIZE) memcpy(&v, xt_iram + off, 4); return v; }
static uint8_t esp_peek8(uint32_t a) { uint32_t v = esp_peek32(a & ~3u); return (uint8_t)(v >> (8 * (a & 3u))); }
static void esp_poke8(uint32_t a, uint8_t v) { uint32_t off; if ((off = a - XT_DRAM_BASE) < XT_DRAM_SIZE) xt_dram[off] = v; else if ((off = a - XT_IRAM_BASE) < XT_IRAM_SIZE) xt_iram[off] = v; }
static void esp_poke32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) esp_poke8(a + (uint32_t)i, (uint8_t)(v >> (8 * i))); }

/* ------------------------------------------------------- appels émulés */
typedef void (*esp_hle_fn)(void);
static int esp_stay;                    /* le handler rejoue : pc inchangé */
static uint32_t esp_ret(void) { return xt.a[0]; }
#define A(n) (xt.a[n])

static struct { uint32_t addr; esp_hle_fn fn; const char *name; } esp_hooks[512];
static unsigned esp_nhooks;
static struct { esp_hle_fn fn; } esp_rom_fn[XT_ROM_SYMS];

static void esp_events_reschedule(void);

/* ---- ROM : libc */
static void rom_memcpy(void) { uint32_t d = A(2), s = A(3), n = A(4); for (uint32_t i = 0; i < n; i++) { uint8_t v = esp_peek8(s + i); esp_poke8(d + i, v); } }
static void rom_memmove(void) {
    uint32_t d = A(2), s = A(3), n = A(4);
    if (d < s) for (uint32_t i = 0; i < n; i++) esp_poke8(d + i, esp_peek8(s + i));
    else for (uint32_t i = n; i-- > 0;) esp_poke8(d + i, esp_peek8(s + i));
}
static int esp_probe; static uint32_t esp_probe_n;
static void rom_memset(void) {
    uint32_t d = A(2), n = A(4); uint8_t v = (uint8_t)A(3);
    if (esp_probe) { esp_probe_n = n; xt.halted = 2; snprintf(xt.msg, sizeof xt.msg, "sonde"); return; } /* taille de cont_t : voir esp_cpu_boot */
    for (uint32_t i = 0; i < n; i++) esp_poke8(d + i, v);
}
static void rom_bzero(void) { uint32_t d = A(2), n = A(3); for (uint32_t i = 0; i < n; i++) esp_poke8(d + i, 0); }
static void rom_memcmp(void) {
    uint32_t p = A(2), q = A(3), n = A(4);
    for (uint32_t i = 0; i < n; i++) { int d = (int)esp_peek8(p + i) - (int)esp_peek8(q + i); if (d) { A(2) = (uint32_t)d; return; } }
    A(2) = 0;
}
static void rom_strlen(void) { uint32_t p = A(2), n = 0; while (esp_peek8(p + n)) n++; A(2) = n; }
static void rom_strcmp(void) {
    uint32_t p = A(2), q = A(3);
    for (uint32_t i = 0;; i++) { int d = (int)esp_peek8(p + i) - (int)esp_peek8(q + i); if (d || !esp_peek8(p + i)) { A(2) = (uint32_t)d; return; } }
}
static void rom_strncmp(void) {
    uint32_t p = A(2), q = A(3), n = A(4);
    for (uint32_t i = 0; i < n; i++) { int d = (int)esp_peek8(p + i) - (int)esp_peek8(q + i); if (d || !esp_peek8(p + i)) { A(2) = (uint32_t)d; return; } }
    A(2) = 0;
}
static void rom_strcpy(void) { uint32_t d = A(2), s = A(3), i = 0; do { esp_poke8(d + i, esp_peek8(s + i)); } while (esp_peek8(s + i++)); }
static void rom_strncpy(void) { uint32_t d = A(2), s = A(3), n = A(4); int end = 0; for (uint32_t i = 0; i < n; i++) { uint8_t c = end ? 0 : esp_peek8(s + i); if (!c) end = 1; esp_poke8(d + i, c); } }
static void rom_strstr(void) {
    uint32_t h = A(2), nd = A(3);
    if (!esp_peek8(nd)) { A(2) = h; return; }
    for (uint32_t i = 0; esp_peek8(h + i); i++) { uint32_t k = 0; while (esp_peek8(nd + k) && esp_peek8(h + i + k) == esp_peek8(nd + k)) k++; if (!esp_peek8(nd + k)) { A(2) = h + i; return; } }
    A(2) = 0;
}
/* ---- ROM : entiers (appel libgcc : arguments a2.. ; 64 bits en paires lo/hi) */
static void rom_udivsi3(void) { A(2) = A(3) ? A(2) / A(3) : 0; }
static void rom_umodsi3(void) { A(2) = A(3) ? A(2) % A(3) : 0; }
static void rom_divsi3(void) { int32_t a = (int32_t)A(2), b = (int32_t)A(3); A(2) = !b ? 0 : (a == INT32_MIN && b == -1) ? (uint32_t)a : (uint32_t)(a / b); }
static uint64_t a64(int i) { return (uint64_t)A(i) | ((uint64_t)A(i + 1) << 32); }
static void r64(uint64_t v) { A(2) = (uint32_t)v; A(3) = (uint32_t)(v >> 32); }
static void rom_muldi3(void) { r64(a64(2) * a64(4)); }
static void rom_umulsidi3(void) { r64((uint64_t)A(2) * A(3)); }
static void rom_udivdi3(void) { uint64_t b = a64(4); r64(b ? a64(2) / b : 0); }
static void rom_umoddi3(void) { uint64_t b = a64(4); r64(b ? a64(2) % b : 0); }
static void rom_divdi3(void) { int64_t a = (int64_t)a64(2), b = (int64_t)a64(4); r64(!b ? 0 : (a == INT64_MIN && b == -1) ? (uint64_t)a : (uint64_t)(a / b)); }
/* ---- ROM : virgule flottante */
static float f32(uint32_t i) { float f; memcpy(&f, &A(i), 4); return f; }
static double f64(int i) { uint64_t u = a64(i); double d; memcpy(&d, &u, 8); return d; }
static void rf32(float f) { memcpy(&A(2), &f, 4); }
static void rf64(double d) { uint64_t u; memcpy(&u, &d, 8); r64(u); }
static void rom_addsf3(void) { rf32(f32(2) + f32(3)); }
static void rom_subsf3(void) { rf32(f32(2) - f32(3)); }
static void rom_mulsf3(void) { rf32(f32(2) * f32(3)); }
static void rom_adddf3(void) { rf64(f64(2) + f64(4)); }
static void rom_subdf3(void) { rf64(f64(2) - f64(4)); }
static void rom_muldf3(void) { rf64(f64(2) * f64(4)); }
static void rom_divdf3(void) { rf64(f64(2) / f64(4)); }
static void rom_fixdfsi(void) { double d = f64(2); A(2) = (uint32_t)(d >= 2147483647.0 ? INT32_MAX : d <= -2147483648.0 ? INT32_MIN : (int32_t)d); }
static void rom_fixunsdfsi(void) { double d = f64(2); A(2) = d <= 0 ? 0u : d >= 4294967295.0 ? 0xffffffffu : (uint32_t)d; }
static void rom_fixunssfsi(void) { float f = f32(2); A(2) = f <= 0 ? 0u : f >= 4294967296.0f ? 0xffffffffu : (uint32_t)f; }
static void rom_truncdfsf2(void) { rf32((float)f64(2)); }
static void rom_extendsfdf2(void) { rf64((double)f32(2)); }
static void rom_floatsisf(void) { rf32((float)(int32_t)A(2)); }
static void rom_floatunsisf(void) { rf32((float)A(2)); }
static void rom_floatsidf(void) { rf64((double)(int32_t)A(2)); }
static void rom_floatunsidf(void) { rf64((double)A(2)); }
/* ---- ROM : divers */
static void rom_nop0(void) { A(2) = 0; }
static void rom_nop1(void) { A(2) = 1; }
static void rom_delay_us(void);
static void rom_isr_attach(void) { unsigned n = A(2) & 15u; esp.isr[n].fn = A(3); esp.isr[n].arg = A(4); }
static void rom_isr_mask(void) { xt.sr[SR_INTENABLE] &= ~A(2); xt_sr_changed(SR_INTENABLE); }
static void rom_isr_unmask(void) { xt.sr[SR_INTENABLE] |= A(2); xt_sr_changed(SR_INTENABLE); }
static void rom_get_cpu_freq(void) { A(2) = (uint32_t)(esp.cpu_hz / 1e6); }
static void rom_update_cpu_freq(void) { esp_set_freq(A(2)); }
static void rom_printf(void) { esp_log("[ets_printf] %.60s\n", ""); A(2) = 0; }
/* SPIRead / SPIWrite / SPIEraseSector de la ROM : mêmes règles que spi_flash_* */
static int esp_flash_check(uint32_t addr, uint32_t n) { return addr + n <= XT_FLASH_SIZE && addr + n >= addr; }
static void esp_flash_touch_save(uint32_t addr, uint32_t n) { if (addr < ESP_SAVE_END && addr + n > ESP_SAVE_OFF) esp.flash_dirty = 1; }
static void esp_flash_time(double seconds) { xt.cycles += (uint64_t)(seconds * esp.cpu_hz); }
static void rom_spi_read(void) {
    uint32_t src = A(2), dst = A(3), n = A(4);
    if ((src & 3) || (dst & 3) || (n & 3) || !esp_flash_check(src, n)) { esp_log("[flash] lecture refusée src=%08x dst=%08x n=%u\n", src, dst, n); A(2) = 1; return; }
    for (uint32_t i = 0; i < n; i++) esp_poke8(dst + i, xt_flash[src + i]);
    esp.flash_reads++;
    esp_flash_time(n * 1.0e-7 + 5e-6);
    A(2) = 0;
}
static void rom_spi_write(void) {
    uint32_t dst = A(2), src = A(3), n = A(4);
    if ((dst & 3) || (src & 3) || (n & 3) || !esp_flash_check(dst, n)) { esp_log("[flash] écriture refusée dst=%08x src=%08x n=%u\n", dst, src, n); A(2) = 1; return; }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v = esp_peek8(src + i), old = xt_flash[dst + i];
        if (v & ~old) esp.flash_bad_writes++;         /* un 1 là où le secteur n'est pas effacé */
        xt_flash[dst + i] = old & v;
    }
    esp.flash_writes++;
    esp_flash_touch_save(dst, n);
    esp_flash_time((double)((n + 255) / 256) * 7e-4);
    A(2) = 0;
}
static void rom_spi_erase_sector(void) {
    uint32_t sec = A(2);
    if ((sec + 1u) * 4096u > XT_FLASH_SIZE) { A(2) = 1; return; }
    memset(xt_flash + sec * 4096u, 0xff, 4096);
    esp.flash_erases++;
    esp_flash_touch_save(sec * 4096u, 4096);
    esp_flash_time(4.5e-2);
    A(2) = 0;
}
static void esp_flash_mirror_code(void); /* le premier Mio de flash est aussi du code : patché par les accroches */

static esp_hle_fn esp_rom_lookup(const char *n) {
    static const struct { const char *n; esp_hle_fn f; } t[] = {
        {"memcpy", rom_memcpy}, {"ets_memcpy", rom_memcpy}, {"memmove", rom_memmove}, {"ets_memmove", rom_memmove},
        {"memset", rom_memset}, {"ets_memset", rom_memset}, {"ets_bzero", rom_bzero}, {"memcmp", rom_memcmp}, {"ets_memcmp", rom_memcmp},
        {"strlen", rom_strlen}, {"ets_strlen", rom_strlen}, {"strcmp", rom_strcmp}, {"ets_strcmp", rom_strcmp},
        {"strncmp", rom_strncmp}, {"ets_strncmp", rom_strncmp}, {"strcpy", rom_strcpy}, {"ets_strcpy", rom_strcpy},
        {"strncpy", rom_strncpy}, {"ets_strncpy", rom_strncpy}, {"strstr", rom_strstr}, {"ets_strstr", rom_strstr},
        {"__udivsi3", rom_udivsi3}, {"__umodsi3", rom_umodsi3}, {"__divsi3", rom_divsi3},
        {"__muldi3", rom_muldi3}, {"__umulsidi3", rom_umulsidi3}, {"__udivdi3", rom_udivdi3}, {"__umoddi3", rom_umoddi3}, {"__divdi3", rom_divdi3},
        {"__addsf3", rom_addsf3}, {"__subsf3", rom_subsf3}, {"__mulsf3", rom_mulsf3},
        {"__adddf3", rom_adddf3}, {"__subdf3", rom_subdf3}, {"__muldf3", rom_muldf3}, {"__divdf3", rom_divdf3},
        {"__fixdfsi", rom_fixdfsi}, {"__fixunsdfsi", rom_fixunsdfsi}, {"__fixunssfsi", rom_fixunssfsi},
        {"__truncdfsf2", rom_truncdfsf2}, {"__extendsfdf2", rom_extendsfdf2},
        {"__floatsisf", rom_floatsisf}, {"__floatunsisf", rom_floatunsisf}, {"__floatsidf", rom_floatsidf}, {"__floatunsidf", rom_floatunsidf},
        {"ets_isr_attach", rom_isr_attach}, {"ets_isr_mask", rom_isr_mask}, {"ets_isr_unmask", rom_isr_unmask},
        {"ets_get_cpu_frequency", rom_get_cpu_freq}, {"ets_update_cpu_frequency", rom_update_cpu_freq},
        {"ets_delay_us", rom_delay_us},
        {"ets_printf", rom_printf}, {"ets_uart_printf", rom_printf}, {"ets_install_putc1", rom_nop0}, {"ets_install_uart_printf", rom_nop0},
        {"ets_wdt_disable", rom_nop0}, {"ets_wdt_enable", rom_nop0}, {"ets_wdt_restore", rom_nop0}, {"ets_wdt_init", rom_nop0},
        {"Cache_Read_Enable", rom_nop0}, {"Cache_Read_Disable", rom_nop0}, {"uart_div_modify", rom_nop0},
        {"rtc_get_reset_reason", rom_nop1}, {"ets_set_idle_cb", rom_nop0},
        {"SPIRead", rom_spi_read}, {"SPIWrite", rom_spi_write}, {"SPIEraseSector", rom_spi_erase_sector},
        {"SPIUnlock", rom_nop0}, {"SPILock", rom_nop0}, {"SPI_read_status", rom_nop0}, {"SPI_write_enable", rom_nop0},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, n)) return t[i].f;
    return NULL;
}

/* ---- temps : attente de n µs / jusqu'à une échéance, ISR comprises */
static void esp_sleep(uint64_t until_cycle) {
    if (!esp.sleeping) { esp.sleep_until = until_cycle; esp.sleeping = 1; }
    if (xt.cycles >= esp.sleep_until) { esp.sleeping = 0; return; }
    uint64_t t = esp.sleep_until;
    if (xt.next_event < t && xt.next_event > xt.cycles) t = xt.next_event;
    if (t <= xt.cycles) t = xt.cycles + 1;
    xt.cycles = t;
    if (xt.cycles >= esp.sleep_until) { esp.sleeping = 0; return; }
    esp_stay = 1;
}
static void rom_delay_us(void) { if (!esp.sleeping) esp_sleep(xt.cycles + (uint64_t)((double)A(2) * esp.cpu_hz / 1e6)); else esp_sleep(0); }

/* ---- SDK / cœur Arduino accrochés par nom */
/* esp_time_scale > 1 : micros()/millis() courent plus vite que le temps émulé, pour qu'un frontal qui attend
 * l'échéance de son image tourne à la vitesse que le processeur permet (mesure de marge, jamais par défaut) */
static double esp_time_scale = 1.0;
/* temps d'attente : les appels de micros() très rapprochés sont la boucle qui attend l'échéance de l'image
 * (frontaux de m0boy) ; leur durée cumulée donne la part du processeur qui reste libre */
static uint64_t esp_idle_cycles, esp_last_micros;
static void hk_micros(void) {
    uint64_t d = xt.cycles - esp_last_micros;
    if (d < 300) esp_idle_cycles += d;
    esp_last_micros = xt.cycles;
    A(2) = (uint32_t)(esp_us() * esp_time_scale);
}
static void hk_millis(void) { A(2) = (uint32_t)(esp_us() * esp_time_scale / 1000.0); }
static void hk_delay(void) { if (!esp.sleeping) esp_sleep(xt.cycles + (uint64_t)((double)A(2) * esp.cpu_hz / 1e3)); else esp_sleep(0); }
static void hk_ret(void) { }
static void hk_ret1(void) { A(2) = 1; }
static void hk_ret0(void) { A(2) = 0; }
static void hk_get_cpu_freq(void) { A(2) = (uint32_t)(esp.cpu_hz / 1e6); }
static void hk_update_cpu_freq(void) { unsigned m = A(2); if (m != 80 && m != 160) { A(2) = 0; return; } esp_set_freq(m); A(2) = 1; }
static void hk_restart(void) { esp.restart = 1; esp_stay = 1; xt.halted = 2; snprintf(xt.msg, sizeof xt.msg, "redémarrage"); }
/* MCP23017 : niveau des broches d'un port (sortie : le verrou ; entrée : le dehors, boutons à la masse sur A,
 * sinon le tirage) ; le CS de l'écran est la broche B0, bas = sélectionné, et reste sélectionné tant que B0 est
 * une entrée sans tirage */
static uint8_t esp_mcp_pins(int port, uint8_t pressed) {
    uint8_t dir = esp.mcp[0x00 + port], olat = esp.mcp[0x14 + port], pu = esp.mcp[0x0c + port];
    uint8_t outside = pu;
    if (port == 0) outside &= (uint8_t)~pressed;
    return (uint8_t)((olat & ~dir) | (outside & dir));
}
static int esp_lcd_selected(void) { return !(esp_mcp_pins(1, 0) & 1); }
static void hk_twi_write(void) {
    uint32_t addr = A(2), buf = A(3), len = A(4);
    if (addr == 0x20) {
        if (len >= 1) esp.mcp_ptr = esp_peek8(buf);
        for (uint32_t i = 1; i < len; i++) {
            uint8_t reg = esp.mcp_ptr;
            if (reg == 0x12 || reg == 0x13) reg += 2;     /* GPIO écrit le verrou */
            if (reg < sizeof esp.mcp) esp.mcp[reg] = esp_peek8(buf + i);
            esp.mcp_ptr = (uint8_t)((esp.mcp_ptr + 1) % 0x16);
        }
        A(2) = 0;
    } else if (addr == 0x60) { for (uint32_t i = 0; i < len && i < 3; i++) esp.dac[i] = esp_peek8(buf + i); A(2) = 0; }
    else A(2) = 2; /* NACK */
}
static void hk_twi_read(void) {
    uint32_t addr = A(2), buf = A(3), len = A(4);
    if (addr != 0x20) { A(2) = 2; return; }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t v = 0;
        if (esp.mcp_ptr == 0x12 || esp.mcp_ptr == 0x13) v = esp_mcp_pins(esp.mcp_ptr - 0x12, esp_buttons_pressed());
        else if (esp.mcp_ptr < sizeof esp.mcp) v = esp.mcp[esp.mcp_ptr];
        esp_poke8(buf + i, v);
        esp.mcp_ptr = (uint8_t)((esp.mcp_ptr + 1) % 0x16);
    }
    A(2) = 0;
}
static void hk_spi_flash_read(void) { rom_spi_read(); }
static void hk_spi_flash_write(void) { rom_spi_write(); }
static void hk_spi_flash_erase(void) { rom_spi_erase_sector(); }
static void hk_flash_size(void) { A(2) = XT_FLASH_SIZE; }

static void esp_hook(const char *name, esp_hle_fn fn) {
    uint32_t addr = esp_sym(name);
    if (!addr || esp_nhooks >= 512) return;
    uint32_t off;
    uint8_t *p = NULL;
    if ((off = addr - XT_IRAM_BASE) < XT_IRAM_SIZE) p = xt_iram + off;
    else if ((off = addr - XT_IROM_BASE) < XT_MAPPED_SIZE) p = xt_flash + off;
    if (!p) return;
    p[0] = p[1] = p[2] = 0;
    esp_hooks[esp_nhooks].addr = addr; esp_hooks[esp_nhooks].fn = fn; esp_hooks[esp_nhooks].name = name;
    esp_nhooks++;
}
static void esp_install_hooks(void) {
    static const struct { const char *n; esp_hle_fn f; } t[] = {
        {"micros", hk_micros}, {"millis", hk_millis}, {"system_get_time", hk_micros},
        {"delay", hk_delay}, {"yield", hk_ret}, {"optimistic_yield", hk_ret}, {"esp_yield", hk_ret}, {"esp_schedule", hk_ret},
        {"system_update_cpu_freq", hk_update_cpu_freq}, {"system_get_cpu_freq", hk_get_cpu_freq},
        {"system_restart", hk_restart}, {"_ZN8EspClass7restartEv", hk_restart}, {"system_restart_core", hk_restart},
        {"twi_writeTo", hk_twi_write}, {"twi_readFrom", hk_twi_read}, {"twi_init", hk_ret}, {"twi_setClock", hk_ret},
        {"twi_setClockStretchLimit", hk_ret},
        {"spi_flash_read", hk_spi_flash_read}, {"spi_flash_write", hk_spi_flash_write}, {"spi_flash_erase_sector", hk_spi_flash_erase},
        {"spi_flash_get_id", hk_ret0}, {"system_get_flash_size_map", hk_flash_size},
        {"wifi_set_opmode_current", hk_ret1}, {"wifi_set_opmode", hk_ret1}, {"wifi_set_sleep_type", hk_ret1},
        {"system_soft_wdt_stop", hk_ret}, {"system_soft_wdt_feed", hk_ret}, {"system_soft_wdt_restart", hk_ret},
        {"system_wdt_feed", hk_ret}, {"ets_wdt_disable", hk_ret},
    };
    esp_nhooks = 0;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) esp_hook(t[i].n, t[i].f);
}

/* ---------------------------------------------------------------- écran ST7735
 * Modèle de la fiche du ST7735S (mémoire 132x162, 18 bits) et de ce que fait le
 * panneau de l'ESPboy : adresses de colonne et de ligne sur leur octet bas
 * seulement, panneau BGR (le bit BGR de MADCTL à 0 échange rouge et bleu quand
 * l'image est balayée, donc aussi pour ce que la mémoire contient déjà),
 * SWRESET qui garde MADCTL et COLMOD, écran blanc tant qu'il est endormi ou
 * éteint.  Tel que posé par TFT_eSPI (MADCTL 0xC8, fenêtre colonne 2, ligne 3),
 * le pixel (x, y) de la vitre est la case mémoire (129 - x, 128 - y).  Mode
 * partiel, défilement et veille ne sont pas modélisés (le firmware ne s'en sert pas). */
#define LCD_MAD_MY 0x80
#define LCD_MAD_MX 0x40
#define LCD_MAD_MV 0x20
#define LCD_MAD_BGR 0x08
static void lcd_common_reset(void) {
    esp.lcd_cmd = 0; esp.lcd_argn = 0; esp.lcd_writing = 0; esp.lcd_npix = 0;
    esp.lcd_sleeping = 1; esp.lcd_on = 0; esp.lcd_inv = 0;
    esp.lcd_xs = 0; esp.lcd_xe = 131; esp.lcd_ys = 0; esp.lcd_ye = 131; esp.lcd_cx = esp.lcd_cy = 0;
}
static void lcd_power_on(void) {
    uint32_t r = 0x12345678u;
    for (int y = 0; y < 162; y++) for (int x = 0; x < 132; x++) { r = r * 1664525u + 1013904223u; esp_gram[y][x] = (r >> 14) & 0x3ffffu; }
    esp.lcd_madctl = 0; esp.lcd_colmod = 6;
    lcd_common_reset();
}
static void lcd_put(uint32_t c) {
    int col = esp.lcd_cx, row = esp.lcd_cy;
    if (esp.lcd_madctl & LCD_MAD_MV) { int t = col; col = row; row = t; }
    if (esp.lcd_madctl & LCD_MAD_MX) col = 131 - col;
    if (esp.lcd_madctl & LCD_MAD_MY) row = 131 - row;
    if ((unsigned)col < 132 && (unsigned)row < 162) esp_gram[row][col] = c;
    esp.lcd_pixels++;
    if (++esp.lcd_cx > esp.lcd_xe) {
        esp.lcd_cx = esp.lcd_xs;
        if (++esp.lcd_cy > esp.lcd_ye) { esp.lcd_cy = esp.lcd_ys; esp.lcd_frames++; }
    }
}
static uint32_t lcd_c5(unsigned v) { return v << 1 | (v == 31); }   /* 5 bits -> 6 : décalé, bit bas à 1 seulement à pleine intensité */
static uint32_t lcd_c4(unsigned v) { return v << 2 | v >> 2; }
static void lcd_pixel_byte(uint8_t b) {
    esp.lcd_pix[esp.lcd_npix++] = b;
    switch (esp.lcd_colmod & 7) {
    case 5: /* 16 bits */
        if (esp.lcd_npix == 2) {
            unsigned c = (unsigned)esp.lcd_pix[0] << 8 | esp.lcd_pix[1];
            lcd_put(lcd_c5(c >> 11) << 12 | ((c >> 5) & 63u) << 6 | lcd_c5(c & 31u));
            esp.lcd_npix = 0;
        }
        break;
    case 3: /* 12 bits : deux pixels en trois octets */
        if (esp.lcd_npix == 2) lcd_put(lcd_c4(esp.lcd_pix[0] >> 4) << 12 | lcd_c4(esp.lcd_pix[0] & 15u) << 6 | lcd_c4(esp.lcd_pix[1] >> 4));
        else if (esp.lcd_npix == 3) { lcd_put(lcd_c4(esp.lcd_pix[1] & 15u) << 12 | lcd_c4(esp.lcd_pix[2] >> 4) << 6 | lcd_c4(esp.lcd_pix[2] & 15u)); esp.lcd_npix = 0; }
        break;
    default: /* 18 bits */
        if (esp.lcd_npix == 3) { lcd_put((uint32_t)(esp.lcd_pix[0] >> 2) << 12 | (uint32_t)(esp.lcd_pix[1] >> 2) << 6 | (uint32_t)(esp.lcd_pix[2] >> 2)); esp.lcd_npix = 0; }
        break;
    }
}
static void lcd_byte(int dc, uint8_t b) {
    if (!dc) { /* commande */
        esp.lcd_cmd = b; esp.lcd_argn = 0; esp.lcd_writing = 0; esp.lcd_npix = 0;
        switch (b) {
        case 0x01: lcd_common_reset(); break;          /* SWRESET : garde MADCTL et COLMOD */
        case 0x10: esp.lcd_sleeping = 1; break;
        case 0x11: esp.lcd_sleeping = 0; break;
        case 0x20: esp.lcd_inv = 0; break;
        case 0x21: esp.lcd_inv = 1; break;
        case 0x28: esp.lcd_on = 0; break;
        case 0x29: esp.lcd_on = 1; break;
        case 0x2c:
            if (esp.lcd_ys <= esp.lcd_last_ys) esp.lcd_frames++;
            esp.lcd_last_ys = (uint16_t)esp.lcd_ys;
            esp.lcd_writing = 1; esp.lcd_cx = esp.lcd_xs; esp.lcd_cy = esp.lcd_ys;
            break;
        }
        return;
    }
    if (esp.lcd_writing) { lcd_pixel_byte(b); return; }
    int n = ++esp.lcd_argn;
    if (n <= 16) esp.lcd_args[n - 1] = b;
    switch (esp.lcd_cmd) {
    case 0x2a: if (n == 2) esp.lcd_xs = esp.lcd_args[1]; if (n == 4) esp.lcd_xe = esp.lcd_args[3]; break;  /* octet bas seulement */
    case 0x2b: if (n == 2) esp.lcd_ys = esp.lcd_args[1]; if (n == 4) esp.lcd_ye = esp.lcd_args[3]; break;
    case 0x36: if (n == 1) esp.lcd_madctl = b; break;
    case 0x3a: if (n == 1) esp.lcd_colmod = b; break;
    }
}
/* la vitre telle qu'elle est maintenant, en RGB565 (R en haut), 128x128 */
static void esp_lcd_render(uint16_t *out) {
    if (!esp.lcd_on || esp.lcd_sleeping) { for (int i = 0; i < ESP_SCR_W * ESP_SCR_H; i++) out[i] = 0xffffu; return; }
    const int swap = !(esp.lcd_madctl & LCD_MAD_BGR);
    for (int y = 0; y < 128; y++) {
        const uint32_t *src = esp_gram[128 - y];
        for (int x = 0; x < 128; x++) {
            uint32_t v = src[129 - x];
            if (esp.lcd_inv) v ^= 0x3ffffu;
            uint32_t r = v >> 12, g = (v >> 6) & 63u, b = v & 63u;
            if (swap) { uint32_t t = r; r = b; b = t; }
            out[y * 128 + x] = (uint16_t)((r >> 1) << 11 | g << 5 | (b >> 1));
        }
    }
}

/* ---------------------------------------------------------------- périphériques */
static double esp_spi_hz(void) {
    uint32_t c = esp.spi1[0x18 / 4];
    if (c & 0x80000000u) return 80e6;
    uint32_t pre = ((c >> 18) & 0x1fffu) + 1u, cnt = ((c >> 12) & 0x3fu) + 1u;
    return 80e6 / ((double)pre * (double)cnt);
}
static void spi1_go(void) {
    uint32_t bits = ((esp.spi1[0x20 / 4] >> 17) & 0x1ffu) + 1u, bytes = (bits + 7u) / 8u;
    if (!(esp.spi1[0x1c / 4] & (1u << 27))) return;      /* pas de phase MOSI */
    /* D/C est GPIO16 (bloc RTC) : haut = donnée, y compris tant que la broche n'est pas une sortie */
    const int dc = (esp.rtc[(0x774 - 0x700) / 4] & 1) ? (int)(esp.rtc[(0x768 - 0x700) / 4] & 1) : 1;
    const int selected = esp_lcd_selected();
    for (uint32_t i = 0; selected && i < bytes && i < 64; i++) lcd_byte(dc, (uint8_t)(esp.spi1[0x40 / 4 + i / 4] >> (8 * (i & 3))));
    esp.spi_bytes += bytes;
    xt.cycles += (uint64_t)((double)bytes * 8.0 * esp.cpu_hz / esp_spi_hz());
}
static void t1_schedule(void) {
    esp.t1_on = 0;
    if (!(esp.t1_ctrl & 0x80) || !esp.t1_load) return;
    static const uint32_t divs[4] = {1, 16, 256, 256};
    double cyc_per_tick = esp.cpu_hz / 80e6 * divs[(esp.t1_ctrl >> 2) & 3];
    esp.t1_next = xt.cycles + (uint64_t)((double)esp.t1_load * cyc_per_tick);
    esp.t1_on = 1;
}
static uint32_t esp_rng_state = 0x2545f491u;
static uint32_t xt_periph_read(uint32_t a) {
    if (a >= 0x3FF20000u && a < 0x3FF40000u) { /* ROM de données ; 3FF20E44 : générateur aléatoire matériel (WDEV) */
        if (a == 0x3FF20E44u) { esp_rng_state ^= esp_rng_state << 13; esp_rng_state ^= esp_rng_state >> 17; esp_rng_state ^= esp_rng_state << 5; return esp_rng_state; }
        return 0;
    }
    uint32_t r = a & 0xffffu;
    if (r >= 0x100 && r < 0x200) return esp.spi1[(r - 0x100) / 4];
    if (r >= 0x300 && r < 0x400) { if (r == 0x318) return 0xffffu; return esp.gpio[(r - 0x300) / 4]; }
    if (r >= 0x600 && r < 0x610) {
        if (r == 0x604) { /* COUNT */
            if (!esp.t1_on) return 0;
            uint64_t left = esp.t1_next > xt.cycles ? esp.t1_next - xt.cycles : 0;
            static const uint32_t divs[4] = {1, 16, 256, 256};
            return (uint32_t)((double)left / (esp.cpu_hz / 80e6 * divs[(esp.t1_ctrl >> 2) & 3]));
        }
        return r == 0x600 ? esp.t1_load : r == 0x608 ? esp.t1_ctrl : 0;
    }
    if (r >= 0x620 && r < 0x630) { if (r == 0x624) return (uint32_t)(esp_us() * 80.0); return 0; } /* FRC2 : 80 MHz libre */
    if (r >= 0x700 && r < 0x800) return esp.rtc[(r - 0x700) / 4];
    if (r >= 0x800 && r < 0x900) return esp.iomux[(r - 0x800) / 4];
    if (r == 0x21c || r == 0x01c) return 0;
    return esp.regs[(r & 0x1fffu) / 4];
}
static void xt_periph_write(uint32_t a, uint32_t v) {
    uint32_t r = a & 0xffffu;
    if (r >= 0x100 && r < 0x200) {
        unsigned i = (r - 0x100) / 4;
        esp.spi1[i] = v;
        if (i == 0 && (v & (1u << 18))) { spi1_go(); esp.spi1[0] &= ~(1u << 18); }
        return;
    }
    if (r >= 0x300 && r < 0x400) {
        unsigned i = (r - 0x300) / 4;
        if (i == 1) esp.gpio[0] |= v;            /* OUT_W1TS */
        else if (i == 2) esp.gpio[0] &= ~v;      /* OUT_W1TC */
        else if (i == 4) esp.gpio[3] |= v;
        else if (i == 5) esp.gpio[3] &= ~v;
        else esp.gpio[i] = v;
        if (i == 0x68 / 4) { /* GPIO_SIGMA_DELTA */
            uint8_t t = (uint8_t)v;
            if (t != esp.sd_value) { esp.sd_value = t; esp_audio_out(t); }
        }
        return;
    }
    if (r >= 0x600 && r < 0x610) {
        if (r == 0x600) { esp.t1_load = v & 0x7fffffu; t1_schedule(); }
        else if (r == 0x608) { esp.t1_ctrl = v; if (!(v & 0x80)) esp.t1_on = 0; else if (!esp.t1_on) t1_schedule(); }
        else if (r == 0x60c) { /* T1I : acquittement */ }
        esp_events_reschedule();
        return;
    }
    if (r >= 0x700 && r < 0x800) {
        esp.rtc[(r - 0x700) / 4] = v;
        return;
    }
    if (r >= 0x800 && r < 0x900) { esp.iomux[(r - 0x800) / 4] = v; return; }
    esp.regs[(r & 0x1fffu) / 4] = v;
}

/* ---------------------------------------------------------------- interruptions & événements */
static void esp_events_reschedule(void) {
    uint64_t n = ~0ull;
    if (esp.t1_on && esp.t1_next < n) n = esp.t1_next;
    if ((xt.sr[SR_INTERRUPT] & xt.sr[SR_INTENABLE]) && !(xt.ps & (XT_PS_INTLEVEL | XT_PS_EXCM)) && esp.isr_depth < 4) n = xt.cycles;
    xt.next_event = n;
}
static void xt_sr_changed(unsigned n) { (void)n; xt.next_event = xt.cycles; }
static void esp_isr_enter(unsigned bit) {
    int d = esp.isr_depth++;
    for (int i = 0; i < 16; i++) esp.ctx[d].a[i] = xt.a[i];
    esp.ctx[d].pc = xt.pc; esp.ctx[d].ps = xt.ps; esp.ctx[d].sar = xt.sar;
    esp.ctx[d].lbeg = xt.lbeg; esp.ctx[d].lend = xt.lend; esp.ctx[d].lcount = xt.lcount;
    xt.ps = (xt.ps & ~XT_PS_INTLEVEL) | 1u;
    xt.a[1] -= 32;
    xt.a[0] = ESP_ISR_RET;
    xt.a[2] = esp.isr[bit].arg;
    xt.a[3] = 0;
    xt.lcount = 0;
    xt.pc = esp.isr[bit].fn;
}
static void xt_events(void) {
    if (esp.t1_on && xt.cycles >= esp.t1_next) {
        uint32_t edge_enable; memcpy(&edge_enable, xt_dport + 4, 4);
        if (edge_enable & 2u) xt.sr[SR_INTERRUPT] |= 1u << 9; /* FRC1 : interruption de front, si DPORT l'autorise */
        if (esp.t1_ctrl & 0x40) { /* recharge */
            static const uint32_t divs[4] = {1, 16, 256, 256};
            double per = (double)esp.t1_load * (esp.cpu_hz / 80e6 * divs[(esp.t1_ctrl >> 2) & 3]);
            if (per < 1) per = 1;
            esp.t1_next += (uint64_t)per;
            if (esp.t1_next <= xt.cycles) esp.t1_next = xt.cycles + 1;
        } else esp.t1_on = 0;
    }
    uint32_t pend = xt.sr[SR_INTERRUPT] & xt.sr[SR_INTENABLE];
    if (pend && !(xt.ps & (XT_PS_INTLEVEL | XT_PS_EXCM)) && esp.isr_depth < 4) {
        unsigned bit = (unsigned)__builtin_ctz(pend);
        xt.sr[SR_INTERRUPT] &= ~(1u << bit);       /* front : consommé */
        if (bit < 16 && esp.isr[bit].fn) esp_isr_enter(bit);
    }
    esp_events_reschedule();
}
static void xt_wait_for_interrupt(void) {
    if (xt.sr[SR_INTERRUPT] & xt.sr[SR_INTENABLE]) return;
    uint64_t n = xt.next_event;
    if (n == ~0ull) n = xt.cycles + (uint64_t)(esp.cpu_hz / 1000.0);
    if (n > xt.cycles) xt.cycles = n;
}

/* appel d'une fonction émulée (ROM, accrochée), retour d'ISR, étapes du démarrage */
static void esp_boot_next(void);
static void esp_call(uint32_t fn, uint32_t a2, uint32_t a3, uint32_t ret) {
    xt.a[0] = ret; xt.a[2] = a2; xt.a[3] = a3; xt.pc = fn;
}
static int xt_rom_call(uint32_t pc) {
    if (pc == ESP_ISR_RET) {
        if (esp.isr_depth <= 0) { xt_fault("retour d'ISR sans ISR"); return 0; }
        int d = --esp.isr_depth;
        for (int i = 0; i < 16; i++) xt.a[i] = esp.ctx[d].a[i];
        xt.pc = esp.ctx[d].pc; xt.ps = esp.ctx[d].ps; xt.sar = esp.ctx[d].sar;
        xt.lbeg = esp.ctx[d].lbeg; xt.lend = esp.ctx[d].lend; xt.lcount = esp.ctx[d].lcount;
        esp_events_reschedule();
        return 1;
    }
    if (pc == ESP_BOOT_NEXT) { esp_boot_next(); return 1; }
    if (pc == ESP_LOOP_NEXT) { esp_call(esp_sym("loop"), 0, 0, ESP_LOOP_NEXT); return 1; }
    esp_hle_fn fn = NULL;
    const char *name = NULL;
    if (pc < 0x40010000u) {
        for (unsigned i = 0; i < XT_ROM_SYMS; i++)
            if (xt_rom_syms[i].addr == pc) {
                name = xt_rom_syms[i].name;
                if (!esp_rom_fn[i].fn) esp_rom_fn[i].fn = esp_rom_lookup(name);
                fn = esp_rom_fn[i].fn;
                break;
            }
        if (!fn) { xt_fault("fonction de la ROM non émulée : %s (%08x), appelée depuis %08x", name ? name : "?", pc, xt.a[0]); return 0; }
    } else {
        for (unsigned i = 0; i < esp_nhooks; i++) if (esp_hooks[i].addr == pc) { fn = esp_hooks[i].fn; name = esp_hooks[i].name; break; }
        if (!fn) return 0;
    }
    esp_stay = 0;
    const uint32_t ret = xt.a[0];
    fn();
    if (!esp_stay && !xt.halted) xt.pc = ret;
    else if (esp_stay && !xt.halted) xt.pc = pc;
    return 1;
}

/* ---------------------------------------------------------------- chargement et démarrage */
static int esp_in_irom(uint32_t a) { return a - XT_IROM_BASE < XT_MAPPED_SIZE; }

/* image .bin : l'application est la seconde image, à 0x1000 (la première est eboot) */
static int esp_load_app_image(const uint8_t *fw, size_t len, uint32_t off) {
    if (off + 8 > len || fw[off] != 0xe9) return -1;
    unsigned nseg = fw[off + 1];
    uint32_t entry; memcpy(&entry, fw + off + 4, 4);
    size_t p = off + 8;
    for (unsigned i = 0; i < nseg; i++) {
        uint32_t addr, size;
        if (p + 8 > len) return -1;
        memcpy(&addr, fw + p, 4); memcpy(&size, fw + p + 4, 4);
        p += 8;
        if (p + size > len) return -1;
        if (!esp_in_irom(addr)) {
            for (uint32_t k = 0; k < size; k++) {
                uint32_t a = addr + k, o;
                if ((o = a - XT_DRAM_BASE) < XT_DRAM_SIZE) xt_dram[o] = fw[p + k];
                else if ((o = a - XT_IRAM_BASE) < XT_IRAM_SIZE) xt_iram[o] = fw[p + k];
            }
        }
        p += size;
    }
    return 0;
}
static int esp_load_syms(const char *text, size_t len) {
    /* lignes « adresse-hex nom » ; « cont_stacksize n » */
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n') j++;
        char line[96];
        size_t n = j - i < sizeof line - 1 ? j - i : sizeof line - 1;
        memcpy(line, text + i, n); line[n] = 0;
        unsigned long v; char name[64];
        if (sscanf(line, "%lx %63s", &v, name) == 2) esp_add_sym((uint32_t)v, name);
        else if (sscanf(line, "cont_stacksize %lu", &v) == 1) esp.cont_bytes = (uint32_t)v;
        i = j + 1;
    }
    return (int)esp.nsym;
}
/* ELF (firmware.elf) : segments et table des symboles */
static int esp_load_elf(const uint8_t *e, size_t len) {
    if (len < 52 || memcmp(e, "\177ELF", 4)) return -1;
    uint32_t phoff, shoff; uint16_t phent, phnum, shent, shnum;
    memcpy(&phoff, e + 28, 4); memcpy(&shoff, e + 32, 4);
    memcpy(&phent, e + 42, 2); memcpy(&phnum, e + 44, 2); memcpy(&shent, e + 46, 2); memcpy(&shnum, e + 48, 2);
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = e + phoff + (size_t)i * phent;
        uint32_t type, off, vaddr, filesz, memsz;
        memcpy(&type, ph, 4); memcpy(&off, ph + 4, 4); memcpy(&vaddr, ph + 8, 4);
        memcpy(&filesz, ph + 16, 4); memcpy(&memsz, ph + 20, 4);
        if (type != 1) continue;
        for (uint32_t k = 0; k < memsz; k++) {
            uint8_t b = k < filesz ? e[off + k] : 0;
            uint32_t a = vaddr + k, o;
            if ((o = a - XT_DRAM_BASE) < XT_DRAM_SIZE) xt_dram[o] = b;
            else if ((o = a - XT_IRAM_BASE) < XT_IRAM_SIZE) xt_iram[o] = b;
            else if ((o = a - XT_IROM_BASE) < XT_MAPPED_SIZE) xt_flash[o] = b;
        }
    }
    for (unsigned i = 0; i < shnum; i++) {
        const uint8_t *sh = e + shoff + (size_t)i * shent;
        uint32_t type, off, size, link;
        memcpy(&type, sh + 4, 4); memcpy(&off, sh + 16, 4); memcpy(&size, sh + 20, 4); memcpy(&link, sh + 24, 4);
        if (type != 2) continue; /* SYMTAB */
        const uint8_t *ssh = e + shoff + (size_t)link * shent;
        uint32_t stroff; memcpy(&stroff, ssh + 16, 4);
        for (uint32_t k = 0; k + 16 <= size; k += 16) {
            uint32_t nm, val; memcpy(&nm, e + off + k, 4); memcpy(&val, e + off + k + 4, 4);
            if (val && nm) esp_add_sym(val, (const char *)e + stroff + nm);
        }
    }
    uint32_t cs = esp_sym("__esp_cont_stacksize");
    (void)cs;
    return 0;
}
static void esp_machine_reset(void) {
    memset(xt_dram, 0, sizeof xt_dram);
    memset(xt_iram, 0, sizeof xt_iram);
    memset(&xt, 0, sizeof xt);
    memset(xt_ctag, 0xff, sizeof xt_ctag);
    memset(esp.gpio, 0, sizeof esp.gpio); memset(esp.spi1, 0, sizeof esp.spi1);
    memset(esp.rtc, 0, sizeof esp.rtc); memset(esp.iomux, 0, sizeof esp.iomux); memset(esp.regs, 0, sizeof esp.regs);
    memset(esp.isr, 0, sizeof esp.isr);
    esp.isr_depth = 0; esp.sleeping = 0; esp.t1_on = 0; esp.t1_load = esp.t1_ctrl = 0;
    lcd_power_on();
    memset(esp.mcp, 0, sizeof esp.mcp); esp.mcp[0] = esp.mcp[1] = 0xff; esp.mcp_ptr = 0; /* IODIR : tout en entrée */
    esp.sd_value = 128;
    esp_lcd_render(esp_fb);
}

/* Démarrage : ce que le SDK laisse après user_init/init_done, puis setup()/loop() */
static void esp_boot_next(void) {
    uint32_t st = esp.stage++;
    const uint32_t cs = esp.scont;
    if (st == 0) {
        uint32_t f = esp_sym("timer1_isr_init");
        if (f) { esp_call(f, 0, 0, ESP_BOOT_NEXT); return; }
        st = esp.stage++;
    }
    if (st == 1) { /* cont_init(g_pcont) */
        uint32_t f = esp_sym("cont_init");
        if (f) { esp_call(f, cs, 0, ESP_BOOT_NEXT); return; }
        st = esp.stage++;
    }
    uint32_t start = esp_sym("__init_array_start"), end = esp_sym("__init_array_end");
    uint32_t n = end > start ? (end - start) / 4 : 0;
    if (st - 2 < n) { /* constructeurs, du dernier au premier comme do_global_ctors */
        uint32_t f = esp_peek32(start + 4 * (n - 1 - (st - 2)));
        esp_call(f, 0, 0, ESP_BOOT_NEXT);
        return;
    }
    if (st - 2 == n) { /* setup() sur la pile cont */
        uint32_t stack_end = esp_sym("cont_init") ? esp_peek32(cs + 16) : 0; /* sans cont : la pile système */
        xt.a[1] = stack_end ? (stack_end & ~15u) - 16u : cs - 64u;
        esp_call(esp_sym("setup"), 0, 0, ESP_LOOP_NEXT);
        esp.booted = 1;
        return;
    }
    xt_fault("étape de démarrage %u inattendue", st);
}
static void esp_cpu_boot(void) {
    esp_machine_reset();
    if (esp.fw_is_elf) esp_load_elf(esp.fw, esp.fwlen);
    else esp_load_app_image(esp.fw, esp.fwlen, 0x1000);
    memset(xt_ctag, 0xff, sizeof xt_ctag);
    esp.cpu_hz = 80e6; esp.cycle_base = 0; esp.us_base = 0;
    esp_set_freq(80);
    esp_install_hooks();
    /* taille de cont_t : cont_init(c) commence par memset(c, 0, sizeof *c), on lit n */
    uint32_t size = 40u + 4096u;
    if (esp_sym("cont_init")) {
        esp_probe = 1; esp_probe_n = 0;
        xt.a[0] = ESP_BOOT_NEXT; xt.a[1] = 0x3FFFFF00u; xt.a[2] = XT_DRAM_BASE; xt.pc = esp_sym("cont_init");
        xt.next_event = ~0ull;
        xt_run(xt.cycles + 200);
        esp_probe = 0; xt.halted = 0; xt.msg[0] = 0;
        if (esp_probe_n >= 4096u && esp_probe_n < 0x4000u) size = esp_probe_n;
    }
    esp.scont = (0x3FFFFFF0u - 64u - size) & ~15u;
    uint32_t gp = esp_sym("g_pcont");
    if (gp) esp_poke32(gp, esp.scont);
    xt.a[1] = esp.scont - 64u;
    xt.ps = 0;
    xt.next_event = ~0ull;
    esp.stage = 0;
    esp.booted = 0;
    esp.fault_shown = 0;
    esp_boot_next();
}
/* Charge un firmware : .bin (application à 0x1000), ou .elf ; ROM (rom.bin) à 0x100000 ;
 * syms : texte « adresse nom » (optionnel avec un .elf). */
static int esp_power_on(const uint8_t *fw, size_t fwlen, const uint8_t *rom, size_t romlen,
                        const char *syms, size_t symslen) {
    static char (*names)[40];
    if (!names) names = malloc(32768 * sizeof *names);
    esp.sym_name = names; esp.nsym = 0; esp.cont_bytes = 0;
    memset(xt_flash, 0xff, XT_FLASH_SIZE);
    esp.fw = fw; esp.fwlen = fwlen;
    esp.fw_is_elf = fwlen > 4 && !memcmp(fw, "\177ELF", 4);
    if (esp.fw_is_elf) {
        if (esp_load_elf(fw, fwlen) < 0) return -1;
    } else {
        if (fwlen > XT_FLASH_SIZE) return -1;
        memcpy(xt_flash, fw, fwlen);
    }
    if (rom && romlen && ESP_ROM_OFF + romlen <= XT_FLASH_SIZE) memcpy(xt_flash + ESP_ROM_OFF, rom, romlen);
    if (syms) esp_load_syms(syms, symslen);
    if (!esp_sym("setup")) return -2; /* sans symboles, pas de démarrage possible */
    esp.flash_dirty = 0; esp.flash_bad_writes = 0;
    esp.flash_reads = esp.flash_writes = esp.flash_erases = 0; esp.spi_bytes = 0;
    esp_cpu_boot();
    return 0;
}
/* exécute n cycles (ou jusqu'à une faute/un redémarrage, traité ici) */
static void esp_run(uint64_t cycles) {
    uint64_t until = xt.cycles + cycles;
    while (xt.cycles < until) {
        xt_run(until);
        if (xt.halted == 2 && esp.restart) { esp.restart = 0; esp_log("[esp] redémarrage\n"); esp_cpu_boot(); continue; }
        if (xt.halted) {
            if (!esp.fault_shown) {
                char where[96];
                esp_symbol_at(xt.pc, where, sizeof where);
                esp_log("[esp] ARRÊT : %s (pc=%08x %s)\n", xt.msg, xt.pc, where);
                esp.fault_shown = 1;
            }
            xt.cycles = until; /* le temps passe, la machine est figée */
            return;
        }
    }
}

#endif /* ESP8266_H */
