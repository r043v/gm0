#include "aka.h"
#include <math.h>
#include <stdlib.h>
#define XT_IROM_BASE 0x42000000u
#define XT_EXTERNAL_MEMORY
#define XT_WINDOWED
static uint8_t xt_flash[8 * 1024 * 1024 + 8];
#include "esp8266_xt.h"
#define AKA_DRAM 0x3fc88000u
#define AKA_DRAM_SIZE 0x78000u
#define AKA_IRAM 0x40370000u
#define AKA_IRAM_SIZE 0x70000u
#define AKA_PSRAM 0x3c800000u
#define AKA_PSRAM_SIZE 0x800000u
static uint8_t dram[AKA_DRAM_SIZE + 8], iram[AKA_IRAM_SIZE + 8],
    psram[AKA_PSRAM_SIZE + 8];
static uint8_t rodata[8 * 1024 * 1024 + 8];
static uint16_t screen[320 * 240];
static uint32_t saved[1024][16];
static unsigned depth;
static uint8_t keys;
static uint16_t extra_keys;
static int joyx_mv = 1650, joyy_mv = 1650;
static uint32_t heap = AKA_PSRAM, internal_heap = AKA_DRAM + 0x50000;
static uint32_t entrypoint;
static int degraded;
static unsigned frames_drawn;
static uint64_t audio_at;
static uint32_t ring_addr, head_addr, tail_addr;
static int16_t last_sample;
static uint8_t *mem(uint32_t a, size_t n) {  uint32_t off;
  if (n > 0x800000)
    return NULL;
  if ((off = a - AKA_DRAM) < AKA_DRAM_SIZE && n <= AKA_DRAM_SIZE - off)
    return dram + off;
  if ((off = a - AKA_IRAM) < AKA_IRAM_SIZE && n <= AKA_IRAM_SIZE - off)
    return iram + off;
  if ((off = a - 0x42000000u) < 0x800000 && n <= 0x800000 - off)
    return xt_flash + off;
  if ((off = a - 0x3c000000u) < 0x800000 && n <= 0x800000 - off)
    return rodata + off;
  if ((off = a - AKA_PSRAM) < AKA_PSRAM_SIZE && n <= AKA_PSRAM_SIZE - off)
    return psram + off;
  return NULL;
}
/* Fenêtre périphériques S3 (cache/MMU, horloges, timers...) : le démarrage
 * ESP-IDF et les drivers y font des lectures/écritures de configuration.
 * Lectures à 0, écritures ignorées — mode dégradé, aucun état simulé. */
static int xt_periph(uint32_t a, unsigned n) {
  return a - 0x60000000u < 0x1000000u && n == 4 && !(a & 3);
}
static uint32_t xt_load(uint32_t a, unsigned n) {
  uint8_t *p = mem(a, n);
  uint32_t v = 0;
  if (xt_periph(a, n))
    return 0;
  if (!p || (a & (n - 1))) {
    xt_fault("AKA load %u at %08x pc=%08x", n, a, xt.pc);
    return 0;
  }
  memcpy(&v, p, n);
  return v;
}
static void xt_store(uint32_t a, uint32_t v, unsigned n) {
  uint8_t *p = mem(a, n);
  if (xt_periph(a, n))
    return;
  if (!p || (a & (n - 1))) {
    xt_fault("AKA store %u at %08x pc=%08x", n, a, xt.pc);
    return;
  }
  memcpy(p, &v, n);
}
static const uint8_t *xt_code(uint32_t pc, uint32_t *base, uint32_t *end) {
  if (pc - AKA_IRAM < AKA_IRAM_SIZE) {
    *base = AKA_IRAM;
    *end = AKA_IRAM + AKA_IRAM_SIZE;
    return iram;
  }
  if (pc - 0x42000000u < 0x800000) {
    *base = 0x42000000u;
    *end = 0x42800000u;
    return xt_flash;
  }
  *base = *end = 0;
  return NULL;
}
/* Window overflow/underflow is serviced here instead of running the ROM's
 * spill handlers. Every live caller survives arbitrary call4/8/12 nesting. */
static void xt_entry(unsigned s, unsigned bytes) {
  unsigned rotate = ((xt.ps >> 16) & 3) * 4;
  uint32_t stack = xt.a[s] - bytes;
  if (depth >= 1024) {
    xt_fault("AKA window stack overflow");
    return;
  }
  memcpy(saved[depth++], xt.a, sizeof xt.a);
  memmove(xt.a, xt.a + rotate, (16 - rotate) * 4);
  memset(xt.a + 16 - rotate, 0, rotate * 4);
  xt.a[1] = stack;
}
static uint32_t xt_retw(uint32_t pc) {
  unsigned rotate = (xt.a[0] >> 30) * 4;
  uint32_t target = (pc & 0xc0000000u) | (xt.a[0] & 0x3fffffffu), live[16];
  if (!depth || !rotate) {
    xt_fault("AKA RETW without ENTRY pc=%08x", pc);
    return 0;
  }
  memcpy(live, xt.a, sizeof live);
  memcpy(xt.a, saved[--depth], sizeof xt.a);
  memcpy(xt.a + rotate, live, (16 - rotate) * 4);
  return target;
}
static void xt_sr_changed(unsigned n) { (void)n; }
static void xt_events(void) { xt.next_event = ~0ull; }
static void xt_wait_for_interrupt(void) { xt.cycles += 240000; }

static uint32_t fp_bits[16];
static float fget(unsigned r) {
  float f;
  memcpy(&f, &fp_bits[r], 4);
  return f;
}
static void fset(unsigned r, float f) { memcpy(&fp_bits[r], &f, 4); }
static int xt_fp(unsigned op1, unsigned op2, unsigned r, unsigned s,
                 unsigned t) {
  float fs = fget(s), ft = fget(t);
  if (op1 == 11) {
    int v = 0;
    switch (op2) {
    case 0:
      v = isnan(fs) || isnan(ft);
      break;
    case 1:
      v = isnan(fs) || isnan(ft) || fs == ft;
      break;
    case 2:
      v = fs == ft;
      break;
    case 3:
      v = isnan(fs) || isnan(ft) || fs < ft;
      break;
    case 4:
      v = fs < ft;
      break;
    case 5:
      v = isnan(fs) || isnan(ft) || fs <= ft;
      break;
    case 6:
      v = fs <= ft;
      break;
    default:
      return 0;
    }
    xt.sr[4] = (xt.sr[4] & ~(1u << r)) | ((unsigned)v << r);
    return 1;
  }
  switch (op2) {
  case 0:
    fset(r, fs + ft);
    break;
  case 1:
    fset(r, fs - ft);
    break;
  case 2:
    fset(r, fs * ft);
    break;
  case 4:
    fset(r, fmaf(fs, ft, fget(r)));
    break;
  case 5:
    fset(r, fmaf(-fs, ft, fget(r)));
    break;
  case 8:
  case 9:
  case 10:
  case 11: {
    double v = ldexp((double)fs, t);
    v = op2 == 8    ? nearbyint(v)
        : op2 == 9  ? trunc(v)
        : op2 == 10 ? floor(v)
                    : ceil(v);
    xt.a[r] = !isfinite(v) || v > INT32_MAX ? INT32_MAX
              : v < INT32_MIN               ? (uint32_t)INT32_MIN
                                            : (uint32_t)(int32_t)v;
    break;
  }
  case 12:
    fset(r, ldexpf((float)(int32_t)xt.a[s], -(int)t));
    break;
  case 13:
    fset(r, ldexpf((float)xt.a[s], -(int)t));
    break;
  case 14: {
    double v = trunc(ldexp(fs, t));
    xt.a[r] = !isfinite(v) || v > UINT32_MAX ? UINT32_MAX
              : v < 0                        ? 0
                                             : (uint32_t)v;
    break;
  }
  case 15:
    if (t == 0)
      fp_bits[r] = fp_bits[s];
    else if (t == 1)
      fp_bits[r] = fp_bits[s] & 0x7fffffff;
    else if (t == 6)
      fp_bits[r] = fp_bits[s] ^ 0x80000000;
    else if (t == 4)
      xt.a[r] = fp_bits[s];
    else if (t == 5)
      fp_bits[r] = xt.a[s];
    else
      return 0;
    break;
  default:
    return 0;
  }
  return 1;
}
static uint32_t xt_fp_mem(unsigned r, unsigned s, unsigned t, unsigned offset) {
  if (r == 0)
    fp_bits[t] = xt_load(xt.a[s] + offset * 4, 4);
  else if (r == 4)
    xt_store(xt.a[s] + offset * 4, fp_bits[t], 4);
  else
    xt_fault("AKA unsupported FP memory opcode %u", r);
  return 0;
}

/* HLE is explicit and closed: an unknown instruction/service fails with its
 * address. No success stub for unknown calls. Symbols come from the ELF or
 * a sidecar produced from that exact ELF. */
typedef struct {
  uint32_t address;
  char name[160];
} Symbol;
static Symbol symbols[20000];
static unsigned nsymbols;
static uint32_t symbol(const char *name) {
  for (unsigned i = 0; i < nsymbols; i++)
    if (!strcmp(symbols[i].name, name))
      return symbols[i].address;
  return 0;
}
static void add_symbol(uint32_t a, const char *name) {
  if (!a || nsymbols >= 20000)
    return;
  symbols[nsymbols].address = a;
  snprintf(symbols[nsymbols++].name, 160, "%s", name);
}
static uint32_t word(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}
static uint16_t half(const uint8_t *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
static unsigned source_rows = 128;
static uint32_t sequence;
static int audio_volume = 4, clock_mhz = 240;
static uint32_t rng = 0x4a3b2917;
static void *guest(uint32_t a, size_t n) {
  void *p = mem(a, n);
  if (!p)
    xt_fault("AKA invalid service buffer %08x + %zu", a, n);
  return p;
}
static uint32_t alloc(unsigned n, int internal) {
  uint32_t *cursor = internal ? &internal_heap : &heap;
  uint32_t limit = internal ? AKA_DRAM + AKA_DRAM_SIZE - 0x10000
                            : AKA_PSRAM + AKA_PSRAM_SIZE;
  if (n > limit - *cursor)
    return 0;
  uint32_t p = *cursor;
  *cursor = (p + n + 15) & ~15u;
  return p;
}
static const char *services[] = {"_ZN7gb_core4initEv",
                                 "lcd_set_fps",
                                 "gb_ll_expander_read",
                                 "gb_ll_adc_init",
                                 "gb_ll_adc_read_joyx",
                                 "gb_ll_adc_read_joyy",
                                 "gb_ll_adc_read_vbatt_mv",
                                 "gb_ll_adc_read_vbatt_percent",
                                 "gb_get_millis",
                                 "gb_delay_ms",
                                 "gb_delay_us",
                                 "_ZN11gb_graphics6updateEv",
                                 "vsnprintf",
                                 "xTaskGetTickCount",
                                 "srand",
                                 "rand",
                                 "m0_clock_init",
                                 "m0_clock_set",
                                 "m0_clock_mhz",
                                 "m0_micros",
                                 "millis",
                                 "delay",
                                 "esp_timer_get_time",
                                 "m0_watchdog_feed",
                                 "esp32_buttons",
                                 "m0_lcd_init",
                                 "m0_lcd_send_rows",
                                 "m0_lcd_done_seq",
                                 "m0_lcd_busy",
                                 "m0_lcd_release",
                                 "m0_lcd_wait",
                                 "m0_lcd_end_frame",
                                 "m0_lcd_fill",
                                 "esp32_audio_start",
                                 "esp32_audio_stop",
                                 "m0_sd_init",
                                 "m0_sd_read",
                                 "m0_sd_write",
                                 "m0_sd_deselect",
                                 "meta_sd_sectors",
                                 "malloc",
                                 "calloc",
                                 "realloc",
                                 "free",
                                 "heap_caps_malloc",
                                 "heap_caps_calloc",
                                 "heap_caps_free",
                                 "memcpy",
                                 "memmove",
                                 "memset",
                                 "memcmp",
                                 "strlen",
                                 "esp_random",
                                 "esp32_restart",
                                 "_Z4millisv",
                                 "_Z5delaym",
                                 NULL};
static int service(const char *name) {
  uint32_t *a = xt.a;
  uint32_t x = a[2], y = a[3], z = a[4];
  if (!strcmp(name, "_ZN7gb_core4initEv"))
    return 1;
  if (!strcmp(name, "gb_ll_adc_init")) {
    a[2] = 0; /* GB_OK */
    return 1;
  }
  if (!strcmp(name, "gb_ll_adc_read_joyx")) {
    a[2] = joyx_mv;
    return 1;
  }
  if (!strcmp(name, "gb_ll_adc_read_joyy")) {
    a[2] = joyy_mv;
    return 1;
  }
  if (!strcmp(name, "gb_ll_adc_read_vbatt_mv")) {
    a[2] = 3900;
    return 1;
  }
  if (!strcmp(name, "gb_ll_adc_read_vbatt_percent")) {
    a[2] = 100 * (3900 - 3300) / (4000 - 3300);
    return 1;
  }
  if (!strcmp(name, "gb_get_millis")) {
    a[2] = xt.cycles / 240000;
    return 1;
  }
  if (!strcmp(name, "gb_delay_ms")) {
    xt.cycles += (uint64_t)x * 240000;
    return 1;
  }
  if (!strcmp(name, "gb_delay_us")) {
    xt.cycles += ((((uint64_t)y << 32) | x) * 240);
    return 1;
  }
  if (!strcmp(name, "lcd_set_fps")) {
    a[2] = x;
    return 1;
  }
  if (!strcmp(name, "xTaskGetTickCount")) {
    a[2] = xt.cycles / 240000;
    xt.cycles += 240;
    return 1;
  }
  if (!strcmp(name, "gb_ll_expander_read")) {
    /* PCF8574 halves on Aka CORE v1.4: released keys are pulled high,
     * except RUN, which reads high only while pressed. The frontend's
     * eight common buttons map HOME to RUN and MENU to MENU. */
    uint32_t pins = 0xfffd;
    if (keys & 1) pins &= ~0x0400u;  /* down */
    if (keys & 2) pins &= ~0x0800u;  /* left */
    if (keys & 4) pins &= ~0x0100u;  /* right */
    if (keys & 8) pins &= ~0x0200u;  /* up */
    if (keys & 16) pins &= ~0x8000u; /* A */
    if (keys & 32) pins &= ~0x2000u; /* B */
    if (keys & 64) pins &= ~0x0004u; /* MENU */
    if (keys & 128) pins |= 0x0002u; /* RUN */
    pins &= ~(extra_keys & (0x0040u | 0x0080u | 0x1000u | 0x4000u));
    a[2] = pins;
    return 1;
  }
  if (!strcmp(name, "_ZN11gb_graphics6updateEv")) {
    uint16_t *p = guest(symbol("framebuffer"), sizeof screen);
    if (p)
      memcpy(screen, p, sizeof screen);
    frames_drawn++;
    xt.cycles += 2400000;
    return 1;
  }
  if (!strcmp(name, "srand")) {
    rng = x;
    return 1;
  }
  if (!strcmp(name, "rand")) {
    rng = rng * 1103515245u + 12345;
    a[2] = (rng >> 16) & 0x7fff;
    return 1;
  }
  if (!strcmp(name, "__divsf3") || !strcmp(name, "__mulsf3") ||
      !strcmp(name, "__addsf3") || !strcmp(name, "__subsf3")) {
    float l, r, v;
    memcpy(&l, &x, 4);
    memcpy(&r, &y, 4);
    v = !strcmp(name, "__divsf3")   ? l / r
        : !strcmp(name, "__mulsf3") ? l * r
        : !strcmp(name, "__addsf3") ? l + r
                                    : l - r;
    memcpy(a + 2, &v, 4);
    return 1;
  }
  if (!strcmp(name, "vsnprintf")) {
    char *dst = guest(x, y), *fmt = guest(z, 1);
    if (!dst || !fmt)
      return 1;
    uint32_t stack = a[5], regsave = a[6], idx = a[7];
    char output[2048];
    size_t used = 0;
    while (*fmt && used < sizeof output - 1) {
      if (*fmt != '%') {
        output[used++] = *fmt++;
        continue;
      }
      fmt++;
      if (*fmt == '%') {
        output[used++] = '%';
        fmt++;
        continue;
      }
      char spec[40] = "%";
      unsigned k = 1;
      while (*fmt && strchr("-+ #0.123456789", *fmt) && k < 30)
        spec[k++] = *fmt++;
      while (*fmt == 'l' || *fmt == 'h')
        fmt++;
      char kind = *fmt++;
      spec[k++] = kind;
      spec[k] = 0;
      uint32_t arg = xt_load(idx < 24 ? regsave + idx : stack + idx - 24, 4);
      idx += 4;
      char part[512];
      int n;
      if (kind == 's') {
        char *str = guest(arg, 1);
        n = snprintf(part, sizeof part, spec, str ? str : "");
      } else if (kind == 'd' || kind == 'i')
        n = snprintf(part, sizeof part, spec, (int32_t)arg);
      else if (strchr("uxXoc", kind))
        n = snprintf(part, sizeof part, spec, arg);
      else {
        xt_fault("AKA unsupported format %c", kind);
        return 1;
      }
      if (n < 0)
        return 1;
      size_t take = (size_t)n < sizeof part ? (size_t)n : sizeof part - 1;
      if (take > sizeof output - 1 - used)
        take = sizeof output - 1 - used;
      memcpy(output + used, part, take);
      used += take;
    }
    output[used] = 0;
    if (y) {
      size_t take = used < y - 1 ? used : y - 1;
      memcpy(dst, output, take);
      dst[take] = 0;
    }
    a[2] = used;
    return 1;
  }
  if (!strcmp(name, "__udivdi3") || !strcmp(name, "__umoddi3") ||
      !strcmp(name, "__divdi3") || !strcmp(name, "__moddi3")) {
    uint64_t n = ((uint64_t)y << 32) | x, d = ((uint64_t)a[5] << 32) | z, v = 0;
    if (!d) {
      xt_fault("AKA divide by zero");
      return 1;
    }
    if (name[2] == 'u')
      v = strstr(name, "mod") ? n % d : n / d;
    else {
      int64_t sn = (int64_t)n, sd = (int64_t)d;
      if (sn == INT64_MIN && sd == -1)
        v = strstr(name, "mod") ? 0 : n;
      else
        v = strstr(name, "mod") ? (uint64_t)(sn % sd) : (uint64_t)(sn / sd);
    }
    a[2] = v;
    a[3] = v >> 32;
    return 1;
  }
  if (!strcmp(name, "m0_micros") || !strcmp(name, "esp_timer_get_time")) {
    uint64_t t = xt.cycles / 240;
    a[2] = t;
    a[3] = t >> 32;
    xt.cycles += 240;
    return 1;
  }
  if (!strcmp(name, "millis") || !strcmp(name, "_Z4millisv")) {
    a[2] = xt.cycles / 240000;
    xt.cycles += 240;
    return 1;
  }
  if (!strcmp(name, "delay") || !strcmp(name, "_Z5delaym")) {
    xt.cycles += (uint64_t)x * 240000;
    return 1;
  }
  if (!strcmp(name, "m0_clock_set")) {
    if (x == 80 || x == 160 || x == 240)
      clock_mhz = x;
    a[2] = clock_mhz;
    return 1;
  }
  if (!strcmp(name, "m0_clock_mhz")) {
    a[2] = clock_mhz;
    return 1;
  }
  if (!strcmp(name, "esp32_buttons")) {
    a[2] = keys;
    return 1;
  }
  if (!strcmp(name, "m0_lcd_send_rows")) {
    uint16_t *p = guest(x, (size_t)z * 160 * 2);
    if (!p || y + z > source_rows) {
      xt_fault("AKA invalid LCD rows");
      return 1;
    }
    for (unsigned row = 0; row < z; row++) {
      unsigned lo = (y + row) * 240 / source_rows,
               hi = (y + row + 1) * 240 / source_rows;
      for (unsigned py = lo; py < hi; py++)
        for (unsigned px = 0; px < 320; px++) {
          uint16_t c = p[row * 160 + px / 2];
          screen[py * 320 + px] = (c >> 8) | (c << 8);
        }
    }
    a[2] = ++sequence;
    frames_drawn++;
    xt.cycles += (uint64_t)z * 160 * 2 * 12;
    return 1;
  }
  if (!strcmp(name, "m0_lcd_done_seq")) {
    a[2] = sequence;
    return 1;
  }
  if (!strcmp(name, "m0_lcd_fill")) {
    uint16_t c = (x >> 8) | (x << 8);
    for (unsigned i = 0; i < 320 * 240; i++)
      screen[i] = c;
    return 1;
  }
  if (!strcmp(name, "m0_lcd_release")) {
    a[2] = 1;
    return 1;
  }
  if (!strcmp(name, "m0_lcd_busy")) {
    a[2] = 0;
    return 1;
  }
  if (!strcmp(name, "m0_audio_set_volume")) {
    audio_volume = x;
    return 1;
  }
  if (!strcmp(name, "m0_audio_volume")) {
    a[2] = audio_volume;
    return 1;
  }
  if (!strcmp(name, "m0_sd_init")) {
    uint8_t sector[512];
    a[2] = aka_sd_read(0, sector);
    return 1;
  }
  if (!strcmp(name, "m0_sd_read") || !strcmp(name, "m0_sd_write")) {
    uint8_t *p = guest(y, 512);
    a[2] = p ? (!strcmp(name, "m0_sd_read") ? aka_sd_read(x, p)
                                            : aka_sd_write(x, p))
             : 0;
    return 1;
  }
  if (!strcmp(name, "meta_sd_sectors")) {
    a[2] = 0;
    return 1;
  }
  if (!strcmp(name, "malloc") || !strcmp(name, "heap_caps_malloc")) {
    a[2] = alloc(x, 0);
    return 1;
  }
  if (!strcmp(name, "calloc") || !strcmp(name, "heap_caps_calloc")) {
    if (y && x > 0xffffffffu / y)
      a[2] = 0;
    else {
      a[2] = alloc(x * y, 0);
      if (a[2])
        memset(mem(a[2], x * y), 0, x * y);
    }
    return 1;
  }
  if (!strcmp(name, "free") || !strcmp(name, "heap_caps_free"))
    return 1;
  if (!strcmp(name, "memcpy") || !strcmp(name, "memmove")) {
    void *p = guest(x, z), *q = guest(y, z);
    if (p && q)
      memmove(p, q, z);
    return 1;
  }
  if (!strcmp(name, "memset")) {
    void *p = guest(x, z);
    if (p)
      memset(p, y, z);
    return 1;
  }
  if (!strcmp(name, "memcmp")) {
    void *p = guest(x, z), *q = guest(y, z);
    if (p && q)
      a[2] = memcmp(p, q, z);
    return 1;
  }
  if (!strcmp(name, "strlen")) {
    unsigned n = 0;
    while (mem(x + n, 1) && *mem(x + n, 1))
      n++;
    a[2] = n;
    return 1;
  }
  if (!strcmp(name, "esp_random")) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    a[2] = rng;
    return 1;
  }
  if (!strcmp(name, "esp32_restart")) {
    xt_fault("AKA application requested restart");
    return 1;
  }
  if (!strcmp(name, "realloc")) {
    xt_fault("AKA realloc is not implemented");
    return 1;
  }
  for (unsigned i = 0; services[i]; i++)
    if (!strcmp(name, services[i]))
      return 1;
  return 0;
}
static struct {
  uint32_t buffer, a[16], pc, ps, sar, lbeg, lend, lcount;
  unsigned depth;
  uint32_t *windows;
} jumps[16];
static int xt_rom_call(uint32_t pc) {
  uint32_t *a = xt.a;
  if (pc == 0x40000ffc) {
    xt.halted = 2;
    snprintf(xt.msg, sizeof xt.msg, "AKA application returned");
    return 1;
  }
  /* Démarrage ESP-IDF : routines de la ROM S3 à adresses fixes. Le cache et
   * la MMU sont logiquement transparents ici (flash déjà plate en
   * 0x42000000), ils sont donc des no-ops. */
  int handled = 0;
  switch (pc) {
  case 0x4000057c: /* rtc_get_reset_reason : power-on reset */
    a[2] = 1;
    handled = 1;
    break;
  case 0x400011e8: /* memset */
  case 0x40001260: /* bzero */
    if (a[4] && !mem(a[2], a[4])) {
      xt_fault("AKA memset %08x + %u pc=%08x", a[2], a[4], xt.pc);
      return 0;
    }
    if (a[4])
      memset(mem(a[2], a[4]), pc == 0x40001260 ? 0 : a[3], a[4]);
    handled = 1;
    break;
  case 0x400011f4: /* memcpy */
  case 0x40001200: /* memmove */
    if (a[4] && (!mem(a[2], a[4]) || !mem(a[3], a[4]))) {
      xt_fault("AKA memcpy %08x <- %08x + %u pc=%08x", a[2], a[3], a[4], xt.pc);
      return 0;
    }
    if (a[4])
      memmove(mem(a[2], a[4]), mem(a[3], a[4]), a[4]);
    handled = 1;
    break;
  case 0x4000186c: /* Cache_Disable_ICache */
  case 0x40001878: /* Cache_Enable_ICache */
  case 0x40001884: /* Cache_Disable_DCache */
  case 0x40001890: /* Cache_Enable_DCache */
  case 0x4000189c: /* rom_Cache_Suspend_ICache */
  case 0x400018a8: /* Cache_Resume_ICache */
  case 0x400018b4: /* rom_Cache_Suspend_DCache */
  case 0x400018c0: /* Cache_Resume_DCache */
  case 0x40001914: /* Cache_Set_IDROM_MMU_Size */
  case 0x40001950: /* Cache_Set_IDROM_MMU_Info */
  case 0x40001974: /* Cache_Owner_Init */
  case 0x40001998: /* Cache_MMU_Init */
  case 0x400019a4: /* Cache_Ibus_MMU_Set */
  case 0x400019b0: /* Cache_Dbus_MMU_Set */
  case 0x40001a1c: /* rom_config_instruction_cache_mode */
  case 0x40001a28: /* rom_config_data_cache_mode */
    handled = 1;
    break;
  }
  if (!handled) {
    /* pas une routine ROM connue : les appels accrochés par symbole passent là */
    for (unsigned i = 0; i < nsymbols; i++)
    if (symbols[i].address == pc) {
      unsigned inc = (xt.ps >> 16) & 3;
      uint32_t ret = xt.a[0];
      if (inc)
        xt_entry(1, 0);
      const char *name = symbols[i].name;
      if (!strcmp(name, "longjmp")) {
        uint32_t buffer = xt.a[2], value = xt.a[3];
        for (unsigned j = 0; j < 16; j++)
          if (jumps[j].buffer == buffer) {
            memcpy(xt.a, jumps[j].a, sizeof xt.a);
            xt.pc = jumps[j].pc;
            xt.ps = jumps[j].ps;
            xt.sar = jumps[j].sar;
            xt.lbeg = jumps[j].lbeg;
            xt.lend = jumps[j].lend;
            xt.lcount = jumps[j].lcount;
            depth = jumps[j].depth;
            memcpy(saved, jumps[j].windows, depth * sizeof saved[0]);
            xt.a[10] = value ? value : 1;
            return 1;
          }
        xt_fault("AKA longjmp to unknown buffer %08x", buffer);
        return 0;
      }
      uint32_t jump_buffer = !strcmp(name, "setjmp") ? xt.a[2] : 0;
      if (jump_buffer)
        xt.a[2] = 0;
      else if (!service(name)) {
        xt_fault("AKA unknown service %s at %08x", symbols[i].name, pc);
        return 0;
      }
      xt.pc = inc ? xt_retw(pc) : ret;
      if (jump_buffer) {
        unsigned j = 0;
        while (j < 16 && jumps[j].buffer && jumps[j].buffer != jump_buffer)
          j++;
        if (j == 16) {
          xt_fault("AKA too many setjmp buffers");
          return 0;
        }
        jumps[j].buffer = jump_buffer;
        memcpy(jumps[j].a, xt.a, sizeof xt.a);
        jumps[j].pc = xt.pc;
        jumps[j].ps = xt.ps;
        jumps[j].sar = xt.sar;
        jumps[j].lbeg = xt.lbeg;
        jumps[j].lend = xt.lend;
        jumps[j].lcount = xt.lcount;
        jumps[j].depth = depth;
        free(jumps[j].windows);
        jumps[j].windows = malloc(depth * sizeof saved[0]);
        if (!jumps[j].windows && depth) {
          xt_fault("AKA setjmp allocation failed");
          return 0;
        }
        memcpy(jumps[j].windows, saved, depth * sizeof saved[0]);
      }
      xt.cycles += 12;
      return 1;
    }
    return 0;
  }
  unsigned inc = (xt.ps >> 16) & 3;
  uint32_t ret = a[0];
  if (inc)
    xt_entry(1, 0);
  xt.pc = inc ? xt_retw(pc) : ret;
  xt.cycles += 12;
  return 1;
}
static int bounds(size_t off, size_t n, size_t len) {
  return off <= len && n <= len - off;
}
static int load_elf(const uint8_t *e, size_t len) {
  if (len < 52 || e[4] != 1 || e[5] != 1 || half(e + 18) != 94)
    return -1;
  uint32_t phoff = word(e + 28), shoff = word(e + 32);
  unsigned phent = half(e + 42), phnum = half(e + 44), shent = half(e + 46),
           shnum = half(e + 48);
  if (phent < 32 || shent < 40 || !bounds(phoff, (size_t)phent * phnum, len) ||
      !bounds(shoff, (size_t)shent * shnum, len))
    return -1;
  for (unsigned i = 0; i < phnum; i++) {
    const uint8_t *p = e + phoff + i * phent;
    uint32_t off = word(p + 4), v = word(p + 8), n = word(p + 16),
             size = word(p + 20);
    if (word(p) != 1)
      continue;
    if (n > size || !bounds(off, n, len))
      return -1;
    uint8_t *dst = mem(v, size);
    if (!dst) { /* RTC data is unused by HLE startup. */
      if (v == 0x50000000)
        continue;
      return -1;
    }
    memset(dst, 0, size);
    memcpy(dst, e + off, n);
  }
  for (unsigned i = 0; i < shnum; i++) {
    const uint8_t *p = e + shoff + i * shent;
    if (word(p + 4) != 2)
      continue;
    unsigned link = word(p + 24);
    uint32_t off = word(p + 16), size = word(p + 20), ent = word(p + 36);
    if (link >= shnum || ent < 16 || !bounds(off, size, len))
      return -1;
    const uint8_t *str = e + shoff + link * shent;
    unsigned stroff = word(str + 16), strsize = word(str + 20);
    if (!bounds(stroff, strsize, len))
      return -1;
    for (unsigned k = 0; k + ent <= size; k += ent) {
      uint32_t n = word(e + off + k), v = word(e + off + k + 4);
      if (n >= strsize || !memchr(e + stroff + n, 0, strsize - n))
        return -1;
      add_symbol(v, (const char *)e + stroff + n);
    }
  }
  return 0;
}
int aka_is_image(const uint8_t *b, size_t n) {
  if (n > 24 && b[0] == 0xe9)
    return half(b + 12) == 9; /* esptool chip id ESP32-S3 */
  if (n > 52 && !memcmp(b, "\177ELF", 4)) {
    if (half(b + 18) != 94)
      return 0;
    uint32_t entry = word(b + 24);
    return entry >= AKA_IRAM && entry < 0x42800000;
  }
  return 0;
}
int aka_load(const uint8_t *b, size_t n, const char *syms, size_t slen) {
  memset(&xt, 0, sizeof xt);
  memset(dram, 0, sizeof dram);
  memset(iram, 0, sizeof iram);
  memset(screen, 0, sizeof screen);
  memset(rodata, 0, sizeof rodata);
  memset(xt_flash, 0, sizeof xt_flash);
  for (unsigned i = 0; i < 16; i++) {
    free(jumps[i].windows);
    memset(&jumps[i], 0, sizeof jumps[i]);
  }
  memset(fp_bits, 0, sizeof fp_bits);
  nsymbols = depth = 0;
  heap = AKA_PSRAM;
  internal_heap = AKA_DRAM + 0x50000;
  frames_drawn = sequence = 0;
  joyx_mv = joyy_mv = 1650;
  extra_keys = 0;
  if (!aka_is_image(b, n))
    return -1;
  uint32_t img_entry = 0;
  if (!memcmp(b, "\177ELF", 4)) {
    img_entry = word(b + 24);
    if (load_elf(b, n))
      return -1;
  } else {
    img_entry = word(b + 4); /* en-tête image ESP : point d'entrée à l'octet 4 */
    size_t off = 24;
    uint8_t checksum = 0xef; /* ESP application image segment checksum */
    for (unsigned i = 0; i < b[1]; i++) {
      if (!bounds(off, 8, n)) {
        xt_fault("AKA truncated ESP segment header");
        return -1;
      }
      uint32_t a = word(b + off), size = word(b + off + 4);
      off += 8;
      if (!bounds(off, size, n)) {
        xt_fault("AKA truncated ESP segment at %08x", a);
        return -1;
      }
      if (!mem(a, size) && a != 0x50000000) {
        xt_fault("AKA invalid ESP segment at %08x + %u", a, size);
        return -1;
      }
      for (uint32_t j = 0; j < size; j++) checksum ^= b[off + j];
      off += size;
    }
    size_t checkpos = off + (15 - (off & 15));
    if (!bounds(checkpos, 1, n) || (b[23] && !bounds(checkpos + 1, 32, n))) {
      xt_fault("AKA truncated ESP image checksum or SHA-256 trailer");
      return -1;
    }
    if (b[checkpos] != checksum) {
      xt_fault("AKA ESP image checksum mismatch");
      return -1;
    }
    off = 24;
    for (unsigned i = 0; i < b[1]; i++) {
      uint32_t a = word(b + off), size = word(b + off + 4);
      off += 8;
      uint8_t *dst = mem(a, size);
      if (dst) memcpy(dst, b + off, size);
      off += size;
    }
  }
  if (syms && slen) {
    char *text = malloc(slen + 1);
    if (!text)
      return -1;
    memcpy(text, syms, slen);
    text[slen] = 0;
    char *line = strtok(text, "\n");
    while (line) {
      unsigned a;
      char type[160], name[160];
      int fields = sscanf(line, "%x %159s %159s", &a, type, name);
      if (fields == 3 && strlen(type) == 1)
        add_symbol(a, name);
      else if (fields >= 2)
        add_symbol(a, type);
      line = strtok(NULL, "\n");
    }
    free(text);
  }
  ring_addr = symbol("_ZL10audio_ring");
  head_addr = symbol("_ZL10audio_head");
  tail_addr = symbol("_ZL10audio_tail");
  if (!ring_addr) {
    ring_addr = symbol("audio_ring");
    head_addr = symbol("audio_head");
    tail_addr = symbol("audio_tail");
  }
  audio_at = 0;
  last_sample = 0;
  entrypoint = symbol("m0boy_console_main");
  if (!entrypoint)
    entrypoint = symbol("app_main");
  degraded = 0;
  if (!entrypoint) {
    /* Pas de table de symboles (ou table d'un autre build) : boot au point
     * d'entrée de l'image, en mode dégradé — les services plateforme ne
     * peuvent pas être accrochés par nom, l'exécution s'arrêtera au premier
     * service non intercepté. */
    if (!mem(img_entry, 4)) {
      xt_fault("AKA needs matching ELF or firmware.sym (no usable entrypoint)");
      return -2;
    }
    entrypoint = img_entry;
    degraded = 1;
  }
  for (unsigned i = 0; services[i]; i++) {
    uint8_t *p = mem(symbol(services[i]), 3);
    if (p)
      memset(p, 0, 3);
  }
  /* All current m0boy ESP32 frontends feed a 160x128 panel surface.
   * Sega scaling (120 rows plus border) already happened in guest code. */
  source_rows = 128;
  xt.a[1] = AKA_DRAM + AKA_DRAM_SIZE - 32;
  xt.a[8] = 0x80000ffc;
  xt.ps = 2u << 16;
  xt.pc = entrypoint;
  xt.next_event = ~0ull;
  return 0;
}
void aka_run(uint64_t cycles) {
  uint64_t end = xt.cycles + cycles;
  if (!ring_addr) {
    if (!xt.halted)
      xt_run(end);
    return;
  }
  while (!xt.halted && xt.cycles < end) {
    if (audio_at <= xt.cycles)
      audio_at = xt.cycles + 21769;
    xt_run(audio_at < end ? audio_at : end);
    if (xt.cycles >= audio_at) {
      unsigned head = xt_load(head_addr, 2), tail = xt_load(tail_addr, 2);
      if (head != tail) {
        last_sample = ((int)xt_load(ring_addr + tail, 1) - 128) * 256;
        xt_store(tail_addr, (tail + 1) & 1023, 2);
      }
      aka_audio_sample(last_sample, audio_at);
      audio_at += 21769;
    }
  }
}
void aka_render(uint16_t *out) { memcpy(out, screen, sizeof screen); }
void aka_set_keys(uint8_t k) { keys = k; }
void aka_set_extra_keys(uint16_t k) { extra_keys = k; }
void aka_set_joystick(int16_t x, int16_t y) {
  joyx_mv = 1650 + (int32_t)x * 1650 / 32768;
  joyy_mv = 1650 - (int32_t)y * 1650 / 32768;
}
uint64_t aka_cycles(void) { return xt.cycles; }
const char *aka_error(void) { return xt.halted ? xt.msg : NULL; }
int aka_degraded(void) { return degraded; }
