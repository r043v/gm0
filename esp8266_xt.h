/*
 * esp8266_xt.h — cœur Xtensa LX106 (ESP8266) : jeu d'instructions de base
 * de l'Arduino core (call0, instructions « density », MUL16, MULL, QUO/REM,
 * boucles matérielles, registres spéciaux), sans fenêtres de registres ni
 * FPU.  Inclus par esp8266.h (machine) ; autonome vis-à-vis de gm0.c : le
 * test de tests/esp/ le compile seul.
 *
 * Mémoire : DRAM 0x3FFE8000 (96 Ko), IRAM 0x40100000 (64 Ko), flash 4 Mio
 * dont le premier Mio est vu en 0x40200000, ROM 0x40000000 (vide : tout appel
 * y tombe sur un ILL que la machine traite comme une fonction émulée).
 * Comme sur la puce, les chargements/rangements 8 et 16 bits en IRAM, flash
 * et ROM lèvent LoadStoreError (cause 3), les accès non alignés
 * LoadStoreAlignment (cause 9) : ils arrêtent l'émulation avec un diagnostic.
 *
 * Temps (cycles du cœur) : 1 par instruction, 2 pour MULL, +2 par branche, saut
 * ou appel pris (RET : +3), +1 si l'instruction suivante utilise le registre
 * qu'un chargement vient de remplir, MEMW 4 après un rangement ; un chargement
 * (L32I, L32R...) en IRAM, ROM ou flash coûte 5 de plus, un registre de
 * périphérique 15 en lecture et 10 en écriture (à 160 MHz ; moitié à 80 MHz,
 * temps de bus), la DRAM rien.  Ces coûts sont ceux que l'émulateur d'ESPboy
 * de référence (ESPboy-edu/espboy_emulator, MIT) a mesurés sur une console
 * (tests/sketches/timing, boucles chaudes) ; ils ne disent rien du coût d'un
 * défaut du cache d'instructions : un modèle optionnel (xt_miss_cycles, par
 * ligne de 32 octets, cache direct de 32 Kio) est éteint par défaut et NON
 * validé.  Le reste du temps (flash, SPI) est estimé par la machine.
 */
#ifndef ESP8266_XT_H
#define ESP8266_XT_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef XT_EXTERNAL_MEMORY
#define XT_DRAM_BASE   0x3FFE8000u
#define XT_DRAM_SIZE   0x18000u
#define XT_IRAM_BASE   0x40100000u
#define XT_IRAM_SIZE   0x10000u
#define XT_IROM_BASE   0x40200000u
#define XT_MAPPED_SIZE 0x100000u
#define XT_FLASH_SIZE  0x400000u
#define XT_PERIPH_BASE 0x60000000u
#define XT_DPORT_BASE  0x3FF00000u

static uint8_t xt_dram[XT_DRAM_SIZE + 8];
static uint8_t xt_iram[XT_IRAM_SIZE + 8];
static uint8_t xt_flash[XT_FLASH_SIZE + 8];
static uint8_t xt_dport[0x1000];

#endif /* XT_EXTERNAL_MEMORY */

/* ---- état */
static struct {
    uint32_t a[16], pc;
    uint32_t sar, lbeg, lend, lcount, ps;
    uint32_t sr[256];            /* registres spéciaux sans traitement propre */
    uint64_t cycles;
    uint64_t next_event;         /* à ce cycle : événements de la machine */
    int halted;                  /* 0 en marche, 1 fautif, 2 demandé par la machine */
    char msg[160];
} xt;

#define XT_PS_INTLEVEL 0xFu
#define XT_PS_EXCM     0x10u
#define SR_LBEG 0
#define SR_LEND 1
#define SR_LCOUNT 2
#define SR_SAR 3
#define SR_SCOMPARE1 12
#define SR_ACCLO 16
#define SR_ACCHI 17
#define SR_EPC1 177
#define SR_DEPC 192
#define SR_EXCSAVE1 209
#define SR_INTERRUPT 226
#define SR_INTCLEAR 227
#define SR_INTENABLE 228
#define SR_PS 230
#define SR_VECBASE 231
#define SR_EXCCAUSE 232
#define SR_CCOUNT 234
#define SR_EXCVADDR 238
#define SR_CCOMPARE0 240

/* modèle de cache de flash (32 Kio, direct, lignes de 32 octets) et coûts */
static uint32_t xt_ctag[1024];
static uint32_t xt_miss_cycles = 0;    /* défaut de cache d'instructions : modèle optionnel, éteint */
static uint32_t xt_extra;              /* cycles ajoutés par la mémoire que l'instruction touche */
static uint32_t xt_imem_extra = 5, xt_p_read = 15, xt_p_write = 10; /* mesurés à 160 MHz ; la machine les règle */
static uint32_t xt_flash_misses, xt_lastline = ~0u;

/* rappels de la machine */
static uint32_t xt_periph_read(uint32_t a);
static void     xt_periph_write(uint32_t a, uint32_t v);
static int      xt_rom_call(uint32_t pc);        /* fonction émulée à pc : 0 = inconnue */
static void     xt_events(void);                 /* échéance atteinte */
static void     xt_wait_for_interrupt(void);     /* WAITI */

static void xt_fault(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void xt_fault(const char *fmt, ...) {
    va_list ap;
    if (xt.halted) return;
    va_start(ap, fmt);
    vsnprintf(xt.msg, sizeof xt.msg, fmt, ap);
    va_end(ap);
    xt.halted = 1;
}

static inline void xt_flash_touch(uint32_t off) {
    if (!xt_miss_cycles) return;
    uint32_t line = off >> 5, i = line & 1023u;
    if (xt_ctag[i] != line) {
        xt_ctag[i] = line;
        xt.cycles += xt_miss_cycles;
        xt_flash_misses++;
    }
}

/* ---- accès mémoire.  size 4 : le cas courant en premier. */
#ifndef XT_EXTERNAL_MEMORY
static uint32_t xt_load(uint32_t a, unsigned size) {
    uint32_t off;
    if ((off = a - XT_DRAM_BASE) < XT_DRAM_SIZE) {
        if (a & (size - 1)) { xt_fault("LoadStoreAlignment : lecture %u octets en %08x, pc=%08x", size, a, xt.pc); return 0; }
        if (size == 4) { uint32_t v; memcpy(&v, xt_dram + off, 4); return v; }
        if (size == 2) { uint16_t v; memcpy(&v, xt_dram + off, 2); return v; }
        return xt_dram[off];
    }
    if ((off = a - XT_IROM_BASE) < XT_MAPPED_SIZE) {
        if (size != 4) { xt_fault("LoadStoreError : lecture %u octets en flash %08x, pc=%08x", size, a, xt.pc); return 0; }
        if (a & 3) { xt_fault("LoadStoreAlignment : lecture 32 bits en %08x, pc=%08x", a, xt.pc); return 0; }
        xt_flash_touch(off);
        xt_extra += xt_imem_extra;
        uint32_t v; memcpy(&v, xt_flash + off, 4); return v;
    }
    if ((off = a - XT_IRAM_BASE) < XT_IRAM_SIZE) {
        if (size != 4) { xt_fault("LoadStoreError : lecture %u octets en IRAM %08x, pc=%08x", size, a, xt.pc); return 0; }
        if (a & 3) { xt_fault("LoadStoreAlignment : lecture 32 bits en %08x, pc=%08x", a, xt.pc); return 0; }
        xt_extra += xt_imem_extra;
        uint32_t v; memcpy(&v, xt_iram + off, 4); return v;
    }
    if (a >= XT_PERIPH_BASE && a < XT_PERIPH_BASE + 0x10000u) {
        if (size != 4) { xt_fault("lecture périphérique %u octets en %08x, pc=%08x", size, a, xt.pc); return 0; }
        xt_extra += xt_p_read;
        return xt_periph_read(a);
    }
    if (a - 0x3FF20000u < 0x20000u) xt_extra += 3; /* compteur WDEV, ROM de données */
    if (a - 0x3FF20000u < 0x20000u) return size == 4 ? xt_periph_read(a) : xt_periph_read(a & ~3u) >> (8 * (a & 3u)); /* ROM de données, WDEV (aléa) */
    if ((off = a - XT_DPORT_BASE) < sizeof xt_dport) {
        uint32_t v = 0;
        memcpy(&v, xt_dport + (off & ~3u), 4);
        return size == 4 ? v : size == 2 ? (v >> (8 * (a & 2))) & 0xffffu : (v >> (8 * (a & 3))) & 0xffu;
    }
    if (a < 0x40010000u) { xt_fault("lecture en ROM %08x, pc=%08x", a, xt.pc); return 0; }
    xt_fault("lecture hors mémoire : %08x, pc=%08x", a, xt.pc);
    return 0;
}

static void xt_store(uint32_t a, uint32_t v, unsigned size) {
    uint32_t off;
    if ((off = a - XT_DRAM_BASE) < XT_DRAM_SIZE) {
        if (a & (size - 1)) { xt_fault("LoadStoreAlignment : écriture %u octets en %08x, pc=%08x", size, a, xt.pc); return; }
        if (size == 4) memcpy(xt_dram + off, &v, 4);
        else if (size == 2) { uint16_t h = (uint16_t)v; memcpy(xt_dram + off, &h, 2); }
        else xt_dram[off] = (uint8_t)v;
        return;
    }
    if ((off = a - XT_IRAM_BASE) < XT_IRAM_SIZE) {
        if (size != 4 || (a & 3)) { xt_fault("LoadStoreError : écriture %u octets en IRAM %08x, pc=%08x", size, a, xt.pc); return; }
        memcpy(xt_iram + off, &v, 4);
        return;
    }
    if (a >= XT_PERIPH_BASE && a < XT_PERIPH_BASE + 0x10000u) {
        if (size != 4) { xt_fault("écriture périphérique %u octets en %08x, pc=%08x", size, a, xt.pc); return; }
        xt_extra += xt_p_write;
        xt_periph_write(a, v);
        return;
    }
    if ((off = a - XT_DPORT_BASE) < sizeof xt_dport) {
        uint32_t w = 0;
        memcpy(&w, xt_dport + (off & ~3u), 4);
        if (size == 4) w = v;
        else if (size == 2) w = (w & ~(0xffffu << (8 * (a & 2)))) | ((v & 0xffffu) << (8 * (a & 2)));
        else w = (w & ~(0xffu << (8 * (a & 3)))) | ((v & 0xffu) << (8 * (a & 3)));
        memcpy(xt_dport + (off & ~3u), &w, 4);
        return;
    }
    if ((a - XT_IROM_BASE) < 0x10000000u) { xt_fault("écriture en flash/ROM %08x, pc=%08x", a, xt.pc); return; }
    xt_fault("écriture hors mémoire : %08x, pc=%08x", a, xt.pc);
}

/* pointeur d'exécution : IRAM ou flash mappée ; NULL ailleurs (ROM : ILL) */
static inline const uint8_t *xt_code(uint32_t pc, uint32_t *base, uint32_t *end) {
    uint32_t off;
    if ((off = pc - XT_IRAM_BASE) < XT_IRAM_SIZE) { *base = XT_IRAM_BASE; *end = XT_IRAM_BASE + XT_IRAM_SIZE; return xt_iram; }
    if ((off = pc - XT_IROM_BASE) < XT_MAPPED_SIZE) { *base = XT_IROM_BASE; *end = XT_IROM_BASE + XT_MAPPED_SIZE; return xt_flash; }
    *base = *end = 0;
    return NULL;
}

#else
static uint32_t xt_load(uint32_t a, unsigned size);
static void xt_store(uint32_t a, uint32_t v, unsigned size);
static const uint8_t *xt_code(uint32_t pc, uint32_t *base, uint32_t *end);
#endif

#ifdef XT_WINDOWED
static int xt_fp(unsigned op1,unsigned op2,unsigned r,unsigned s,unsigned t);
static uint32_t xt_fp_mem(unsigned r,unsigned s,unsigned t,unsigned offset);
static void xt_entry(unsigned s, unsigned bytes);
static uint32_t xt_retw(uint32_t pc);
#endif

/* ---- registres spéciaux */
static uint32_t xt_ccount(void) { return (uint32_t)xt.cycles; }
static void xt_sr_changed(unsigned n);            /* machine : PS, INTENABLE... */

static uint32_t xt_rsr(unsigned n) {
    switch (n) {
        case SR_LBEG: return xt.lbeg;
        case SR_LEND: return xt.lend;
        case SR_LCOUNT: return xt.lcount;
        case SR_SAR: return xt.sar;
        case SR_PS: return xt.ps;
        case SR_CCOUNT: return xt_ccount();
        case 235: return 0; /* PRID */
    }
    return xt.sr[n & 255];
}
static void xt_wsr(unsigned n, uint32_t v) {
    switch (n) {
        case SR_LBEG: xt.lbeg = v; return;
        case SR_LEND: xt.lend = v; return;
        case SR_LCOUNT: xt.lcount = v; return;
        case SR_SAR: xt.sar = v & 63u; return;
        case SR_PS: xt.ps = v & 0x1ffu; xt_sr_changed(n); return;
        case SR_CCOUNT: xt.cycles = (xt.cycles & ~0xffffffffull) | v; return;
        case SR_INTCLEAR: xt.sr[SR_INTERRUPT] &= ~v; return;
        case SR_INTERRUPT: xt.sr[SR_INTERRUPT] |= v; xt_sr_changed(n); return; /* INTSET */
        case SR_INTENABLE: xt.sr[n] = v; xt_sr_changed(n); return;
    }
    xt.sr[n & 255] = v;
}

static const int32_t xt_b4const[16]  = {-1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256};
static const uint32_t xt_b4constu[16] = {32768, 65536, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256};

static inline int32_t xt_sext(uint32_t v, unsigned bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }
static inline unsigned xt_clz(uint32_t v) { return v ? (unsigned)__builtin_clz(v) : 32u; }

/* appel de la fonction émulée située à pc (ROM, ou fonction accrochée dont
 * le premier mot est un ILL) : sinon vraie instruction illégale */
static int xt_illegal(uint32_t pc) {
    if (xt_rom_call(pc)) return 1;
    xt_fault("instruction illégale en %08x", pc);
    return 0;
}

/* registres lus par l'instruction (masque de a0-a15), pour l'interverrouillage
 * chargement -> utilisation : un registre qu'un chargement vient de remplir
 * coûte un cycle de plus à l'instruction suivante qui le lit */
static unsigned xt_reads(uint32_t b0, uint32_t b1, uint32_t b2) {
    const uint32_t op0 = b0 & 15u, t = b0 >> 4, s = b1 & 15u, r = b1 >> 4;
    const unsigned S = 1u << s, T = 1u << t;
    if (op0 >= 8) {
        switch (op0) {
        case 8: case 11: return S;                       /* L32I.N, ADDI.N */
        case 9: case 10: return S | T;                   /* S32I.N, ADD.N */
        case 12: return (t & 8) ? S : 0;                 /* BEQZ.N / BNEZ.N */
        case 13: return r == 0 ? S : r == 15 && t == 0 ? 1u : 0; /* MOV.N, RET.N */
        }
        return 0;
    }
    const uint32_t op1 = b2 & 15u, op2 = b2 >> 4;
    switch (op0) {
    case 0:
        switch (op1) {
        case 0:
            if (op2 == 0) return r == 0 ? (t == 8 ? 1u : S) : r == 1 || r == 6 ? S : 0;
            if (op2 == 4) return r <= 3 || r >= 14 ? S : 0;
            if (op2 == 6) return T;
            if (op2 == 5) return 0;
            return S | T;
        case 1:
            if (op2 <= 1 || op2 == 10) return S;
            if (op2 == 2 || op2 == 3 || op2 == 4 || op2 == 6 || op2 == 9 || op2 == 11) return T;
            return S | T;
        case 2: return S | T;
        case 3: return op2 == 0 ? 0 : op2 == 1 ? T : op2 == 2 || op2 == 3 ? S : S | T;
        case 4: case 5: return T;
        }
        return 0;
    case 2:
        if (r == 10) return 0;
        if (r == 4 || r == 5 || r == 6 || r == 14 || r == 15) return S | T;
        return S;
    case 6: return (t & 3) == 0 ? 0 : S;
    case 7: return (r == 6 || r == 7 || r == 14 || r == 15) ? S : S | T;
    }
    return 0;
}

/* exécute jusqu'au cycle `until` (ou la fin demandée par la machine) */
static void xt_run(uint64_t until) {
    uint32_t cbase = 0, cend = 0;
    const uint8_t *cmem = NULL;
    uint32_t pc = xt.pc;
    uint32_t *a = xt.a;
    unsigned lastload = 16;   /* registre rempli par l'instruction précédente (16 : aucun) */
    int after_store = 0;      /* l'instruction précédente a rangé : MEMW attend */

    while (!xt.halted && xt.cycles < until) {
        if (xt.cycles >= xt.next_event) {
            xt.pc = pc;
            xt_events();
            pc = xt.pc;
            if (xt.halted || xt.cycles >= until) break;
            cmem = NULL; cbase = cend = 0;
        }
        if (pc - cbase >= cend - cbase || !cmem) {
            cmem = xt_code(pc, &cbase, &cend);
            if (!cmem) { xt.pc = pc; if (!xt_illegal(pc)) break; pc = xt.pc; continue; }
        }
        const uint8_t *ip = cmem + (pc - cbase);
        if (cmem == xt_flash) {
            uint32_t line = (pc - XT_IROM_BASE) >> 5;
            if (line != xt_lastline) { xt_lastline = line; xt_flash_touch(pc - XT_IROM_BASE); }
        }
        const uint32_t b0 = ip[0], b1 = ip[1];
        const uint32_t op0 = b0 & 15u;
#ifdef XT_TRACE
        fprintf(stderr, "%08x %02x%02x%02x  a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x\n", pc, b0, b1, ip[2], a[0], a[1], a[2], a[3], a[4]);
#endif
        uint32_t next, cost = 1;
        unsigned loaded = 16;
        int stored = 0;
        xt_extra = 0;
        if (lastload < 16 && (xt_reads(b0, b1, ip[2]) >> lastload & 1u)) cost++;
        xt.pc = pc;

        if (op0 >= 8) { /* instructions 16 bits (density) */
            const uint32_t t = b0 >> 4, s = b1 & 15u, r = b1 >> 4;
            next = pc + 2;
            switch (op0) {
            case 8: { /* L32I.N */
                uint32_t v = xt_load(a[s] + (r << 2), 4);
                if (xt.halted) goto out;
                a[t] = v; loaded = t; break; }
            case 9: /* S32I.N */
                xt_store(a[s] + (r << 2), a[t], 4); stored = 1; break;
            case 10: a[r] = a[s] + a[t]; break; /* ADD.N */
            case 11: a[r] = a[s] + (t ? t : (uint32_t)-1); break; /* ADDI.N */
            case 12: /* ST2 */
                if (!(t & 8)) { /* MOVI.N */
                    uint32_t imm = ((t & 7u) << 4) | r;
                    a[s] = imm >= 96 ? imm - 128u : imm;
                } else { /* BEQZ.N / BNEZ.N */
                    uint32_t imm = ((t & 3u) << 4) | r;
                    int taken = (t & 4) ? a[s] != 0 : a[s] == 0;
                    if (taken) { next = pc + 4 + imm; cost += 2; }
                }
                break;
            case 13: /* ST3 */
                if (r == 0) a[t] = a[s]; /* MOV.N */
                else if (r == 15) {
                    if (t == 0) { next = a[0]; cost += 3; } /* RET.N */
#ifdef XT_WINDOWED
                    else if (t == 1) { next = xt_retw(pc); cost += 3; }
#endif
                    else if (t == 3) { /* NOP.N */ }
                    else if (t == 2) { xt.halted = 2; snprintf(xt.msg, sizeof xt.msg, "BREAK.N %u en %08x", s, pc); goto out; }
                    else { xt_fault("instruction narrow %02x%02x illégale en %08x", b1, b0, pc); goto out; }
                } else { xt_fault("instruction narrow %02x%02x illégale en %08x", b1, b0, pc); goto out; }
                break;
            default: xt_fault("instruction %02x%02x illégale en %08x", b1, b0, pc); goto out;
            }
        } else {
            const uint32_t b2 = ip[2];
            const uint32_t t = b0 >> 4, s = b1 & 15u, r = b1 >> 4, op1 = b2 & 15u, op2 = b2 >> 4;
            next = pc + 3;
            switch (op0) {
            case 0: /* QRST */
                switch (op1) {
                case 0: /* RST0 */
                    switch (op2) {
                    case 0: /* ST0 */
                        switch (r) {
                        case 0: /* SNM0 : CALLX/JX/RET */
                            if (t == 0xC) { const uint32_t tgt = a[s]; a[0] = pc + 3;
#ifdef XT_WINDOWED
                                xt.ps &= ~(3u<<16);
#endif
                                next = tgt; cost += 2; } /* CALLX0 */
                            #ifdef XT_WINDOWED
                            else if (t >= 0xD) { unsigned n = t & 3; uint32_t dest = a[s]; a[n*4] = ((pc+3)&0x3fffffff) | (n<<30); xt.ps = (xt.ps & ~(3u<<16)) | (n<<16); next = dest; cost += 2; }
                            else if (t == 9) { next = xt_retw(pc); cost += 3; }
#endif
                            else if (t == 0xA) { next = a[s]; cost += 2; }                  /* JX */
                            else if (t == 8) { next = a[0]; cost += 3; }                    /* RET */
                            else if (t == 0 && s == 0) { /* ILL : fonction émulée (ROM, accrochée) ou vraie faute */
                                xt.pc = pc;
                                if (!xt_illegal(pc)) goto out;
                                next = xt.pc; cost = 0;
                            } else { xt_fault("SNM0 t=%u illégal (fenêtres ?) en %08x", t, pc); goto out; }
                            break;
                        case 1: a[t] = a[s]; break; /* MOVSP */
                        case 2: if (t == 12 && after_store) cost = 4; break; /* SYNC : ISYNC... MEMW (attend le rangement) */
                        case 4: xt.halted = 2; snprintf(xt.msg, sizeof xt.msg, "BREAK %u,%u en %08x", s, t, pc); goto out;
                        case 5: xt_fault("SYSCALL en %08x", pc); goto out;
                        case 6: /* RSIL */
                            a[t] = xt.ps;
                            xt.ps = (xt.ps & ~XT_PS_INTLEVEL) | s;
                            xt_sr_changed(SR_PS);
                            break;
                        case 7: /* WAITI */
                            xt.ps = (xt.ps & ~XT_PS_INTLEVEL) | s;
                            xt_sr_changed(SR_PS);
                            xt.pc = next;
                            xt_wait_for_interrupt();
                            next = xt.pc;
                            break;
                        default: xt_fault("ST0 r=%u illégal en %08x", r, pc); goto out;
                        }
                        break;
                    case 1: a[r] = a[s] & a[t]; break;
                    case 2: a[r] = a[s] | a[t]; break;
                    case 3: a[r] = a[s] ^ a[t]; break;
                    case 4: /* ST1 */
                        switch (r) {
                        case 0: xt.sar = a[s] & 31u; break;                    /* SSR */
                        case 1: xt.sar = 32u - (a[s] & 31u); break;            /* SSL */
                        case 2: xt.sar = 8u * (a[s] & 3u); break;              /* SSA8L */
                        case 3: xt.sar = 32u - 8u * (a[s] & 3u); break;        /* SSA8B */
                        case 4: xt.sar = s | ((t & 1u) << 4); break;           /* SSAI */
                        case 14: { /* NSA */
                            int32_t v = (int32_t)a[s];
                            a[t] = v == 0 || v == -1 ? 31u : xt_clz((uint32_t)(v < 0 ? ~v : v)) - 1u; break; }
                        case 15: a[t] = xt_clz(a[s]); break;                   /* NSAU */
                        default: xt_fault("ST1 r=%u illégal en %08x", r, pc); goto out;
                        }
                        break;
                    case 5: break; /* TLB : sans effet */
                    case 6: /* RT0 */
                        if (s == 0) a[r] = 0u - a[t];
                        else if (s == 1) a[r] = (int32_t)a[t] < 0 ? 0u - a[t] : a[t];
                        else { xt_fault("RT0 s=%u illégal en %08x", s, pc); goto out; }
                        break;
                    case 8: a[r] = a[s] + a[t]; break;
                    case 9: a[r] = (a[s] << 1) + a[t]; break;
                    case 10: a[r] = (a[s] << 2) + a[t]; break;
                    case 11: a[r] = (a[s] << 3) + a[t]; break;
                    case 12: a[r] = a[s] - a[t]; break;
                    case 13: a[r] = (a[s] << 1) - a[t]; break;
                    case 14: a[r] = (a[s] << 2) - a[t]; break;
                    case 15: a[r] = (a[s] << 3) - a[t]; break;
                    default: xt_fault("RST0 op2=%u illégal en %08x", op2, pc); goto out;
                    }
                    break;
                case 1: /* RST1 */
                    switch (op2) {
                    case 0: case 1: a[r] = a[s] << (32u - (((op2 & 1u) << 4) | t)); break; /* SLLI */
                    case 2: case 3: a[r] = (uint32_t)((int32_t)a[t] >> (((op2 & 1u) << 4) | s)); break; /* SRAI */
                    case 4: a[r] = a[t] >> s; break; /* SRLI */
                    case 6: { /* XSR */
                        unsigned sr = (r << 4) | s;
                        uint32_t old = xt_rsr(sr);
                        xt_wsr(sr, a[t]);
                        a[t] = old;
                        break; }
                    case 8: { /* SRC */
                        uint64_t v = ((uint64_t)a[s] << 32) | a[t];
                        a[r] = (uint32_t)(v >> (xt.sar & 63u));
                        break; }
                    case 9: a[r] = (xt.sar & 63u) >= 32u ? 0u : a[t] >> (xt.sar & 63u); break; /* SRL */
                    case 10: { const uint32_t sh = 32u - (xt.sar & 63u); a[r] = sh >= 32u ? 0u : a[s] << sh; break; } /* SLL */
                    case 11: a[r] = (uint32_t)((int32_t)a[t] >> ((xt.sar & 63u) > 31u ? 31u : (xt.sar & 63u))); break; /* SRA */
                    case 12: a[r] = (a[s] & 0xffffu) * (a[t] & 0xffffu); break; /* MUL16U */
                    case 13: a[r] = (uint32_t)((int32_t)(int16_t)a[s] * (int32_t)(int16_t)a[t]); break; /* MUL16S */
                    default: xt_fault("RST1 op2=%u illégal en %08x", op2, pc); goto out;
                    }
                    break;
                case 2: /* RST2 */
                    switch (op2) {
                    #ifdef XT_WINDOWED
                    case 6: a[r] = a[s] < a[t]; break;
                    case 7: a[r] = (int32_t)a[s] < (int32_t)a[t]; break;
#endif
                    case 8: a[r] = a[s] * a[t]; cost = 2; break; /* MULL */
                    case 10: a[r] = (uint32_t)(((uint64_t)a[s] * a[t]) >> 32); break; /* MULUH */
                    case 11: a[r] = (uint32_t)(((int64_t)(int32_t)a[s] * (int32_t)a[t]) >> 32); break; /* MULSH */
                    case 12: case 13: case 14: case 15:
                        if (!a[t]) { xt_fault("IntegerDivideByZero en %08x", pc); goto out; }
                        if (op2 == 12) a[r] = a[s] / a[t];
                        else if (op2 == 14) a[r] = a[s] % a[t];
                        else if (op2 == 13) a[r] = (int32_t)a[s] == INT32_MIN && (int32_t)a[t] == -1 ? a[s] : (uint32_t)((int32_t)a[s] / (int32_t)a[t]);
                        else a[r] = (int32_t)a[t] == -1 ? 0u : (uint32_t)((int32_t)a[s] % (int32_t)a[t]);
                        break;
                    default: xt_fault("RST2 op2=%u illégal en %08x (booléens/MAC ?)", op2, pc); goto out;
                    }
                    break;
                case 3: /* RST3 */
                    switch (op2) {
                    case 0: a[t] = xt_rsr((r << 4) | s); break; /* RSR */
                    case 1: xt_wsr((r << 4) | s, a[t]); break;  /* WSR */
                    case 2: a[r] = (uint32_t)xt_sext(a[s], t + 8u); break; /* SEXT */
                    case 3: { /* CLAMPS */
                        int32_t v = (int32_t)a[s], hi = (int32_t)((1u << (t + 7u)) - 1u), lo = -hi - 1;
                        a[r] = (uint32_t)(v < lo ? lo : v > hi ? hi : v); break; }
                    case 4: a[r] = (int32_t)a[s] < (int32_t)a[t] ? a[s] : a[t]; break;
                    case 5: a[r] = (int32_t)a[s] > (int32_t)a[t] ? a[s] : a[t]; break;
                    case 6: a[r] = a[s] < a[t] ? a[s] : a[t]; break;
                    case 7: a[r] = a[s] > a[t] ? a[s] : a[t]; break;
                    case 8: if (!a[t]) a[r] = a[s]; break;
                    case 9: if (a[t]) a[r] = a[s]; break;
                    case 10: if ((int32_t)a[t] < 0) a[r] = a[s]; break;
                    case 11: if ((int32_t)a[t] >= 0) a[r] = a[s]; break;
                    case 14: a[r] = 0; break; /* RUR (THREADPTR) */
                    case 15: break;           /* WUR */
                    default: xt_fault("RST3 op2=%u illégal en %08x", op2, pc); goto out;
                    }
                    break;
                #ifdef XT_WINDOWED
                case 10: case 11:
                    if(!xt_fp(op1,op2,r,s,t)) { xt_fault("AKA FP opcode %06x pc=%08x",b0|(b1<<8)|(b2<<16),pc);goto out; }break;
#endif
                case 4: case 5: { /* EXTUI */
                    uint32_t shift = s | ((op1 & 1u) << 4), mask = (1u << (op2 + 1u)) - 1u;
                    if (op2 == 15) mask = 0xffffu;
                    a[r] = (a[t] >> shift) & mask;
                    break; }
                default: xt_fault("QRST op1=%u illégal en %08x", op1, pc); goto out;
                }
                break;
            case 1: { /* L32R */
                uint32_t addr = ((pc + 3) & ~3u) + (0xFFFC0000u | (((b2 << 8) | b1) << 2));
                uint32_t v = xt_load(addr, 4);
                if (xt.halted) goto out;
                a[t] = v; loaded = t; break; }
            case 2: { /* LSAI */
                const uint32_t imm8 = b2;
                switch (r) {
                case 0: { uint32_t v = xt_load(a[s] + imm8, 1); if (xt.halted) goto out; a[t] = v; loaded = t; break; }
                case 1: { uint32_t v = xt_load(a[s] + (imm8 << 1), 2); if (xt.halted) goto out; a[t] = v; loaded = t; break; }
                case 2: { uint32_t v = xt_load(a[s] + (imm8 << 2), 4); if (xt.halted) goto out; a[t] = v; loaded = t; break; }
                case 4: xt_store(a[s] + imm8, a[t], 1); stored = 1; break;
                case 5: xt_store(a[s] + (imm8 << 1), a[t], 2); stored = 1; break;
                case 6: xt_store(a[s] + (imm8 << 2), a[t], 4); stored = 1; break;
                case 7: break; /* cache : sans effet */
                case 9: { uint32_t v = xt_load(a[s] + (imm8 << 1), 2); if (xt.halted) goto out; a[t] = (uint32_t)(int32_t)(int16_t)v; loaded = t; break; }
                case 10: a[t] = (uint32_t)xt_sext((s << 8) | imm8, 12); break; /* MOVI */
                case 11: { uint32_t v = xt_load(a[s] + (imm8 << 2), 4); if (xt.halted) goto out; a[t] = v; loaded = t; break; } /* L32AI */
                case 12: a[t] = a[s] + (uint32_t)xt_sext(imm8, 8); break; /* ADDI */
                case 13: a[t] = a[s] + ((uint32_t)xt_sext(imm8, 8) << 8); break; /* ADDMI */
                case 14: { /* S32C1I */
                    uint32_t ad = a[s] + (imm8 << 2), old = xt_load(ad, 4);
                    if (xt.halted) goto out;
                    if (old == xt.sr[SR_SCOMPARE1]) xt_store(ad, a[t], 4);
                    a[t] = old; loaded = t; stored = 1; break; }
                case 15: xt_store(a[s] + (imm8 << 2), a[t], 4); stored = 1; break; /* S32RI */
                default: xt_fault("LSAI r=%u illégal en %08x", r, pc); goto out;
                }
                break; }
            #ifdef XT_WINDOWED
            case 3: xt_fp_mem(r,s,t,b2);break;
#endif
            case 5: { /* CALLN */
                const uint32_t n = (b0 >> 4) & 3u;
                #ifndef XT_WINDOWED
                if (n) { xt_fault("CALL%u (fenêtres) en %08x", 4 * n, pc); goto out; }
#endif
                int32_t off = xt_sext((b0 >> 6) | (b1 << 2) | (b2 << 10), 18);
                #ifdef XT_WINDOWED
                a[n*4] = ((pc+3)&0x3fffffff) | (n<<30);
                xt.ps = (xt.ps & ~(3u<<16)) | (n<<16);
#else
                a[0] = pc + 3;
#endif
                next = ((pc & ~3u) + 4u) + ((uint32_t)off << 2);
                cost += 2;
                break; }
            case 6: { /* SI */
                const uint32_t n = (b0 >> 4) & 3u, m = b0 >> 6;
                if (n == 0) { /* J */
                    next = pc + 4u + (uint32_t)xt_sext((b0 >> 6) | (b1 << 2) | (b2 << 10), 18);
                    cost += 2;
                } else if (n == 1) { /* BZ */
                    int32_t v = (int32_t)a[s];
                    int taken = m == 0 ? v == 0 : m == 1 ? v != 0 : m == 2 ? v < 0 : v >= 0;
                    if (taken) { next = pc + 4u + (uint32_t)xt_sext((b2 << 4) | r, 12); cost += 2; }
                } else if (n == 2) { /* BI0 */
                    int32_t v = (int32_t)a[s], c = xt_b4const[r];
                    int taken = m == 0 ? v == c : m == 1 ? v != c : m == 2 ? v < c : v >= c;
                    if (taken) { next = pc + 4u + (uint32_t)xt_sext(b2, 8); cost += 2; }
                } else if (m == 0) {
#ifdef XT_WINDOWED
                    xt_entry(s, ((b2<<4)|r)*8);
#else
                    xt_fault("ENTRY (fenêtres) en %08x", pc); goto out;
#endif
                }
                else if (m == 1) { /* B1 : BF/BT (booléens), LOOP */
                    if (r >= 8 && r <= 10) {
                        const uint32_t cnt = a[s];
                        xt.lcount = cnt - 1u;
                        xt.lbeg = pc + 3;
                        xt.lend = pc + 4u + b2;
                        if ((r == 9 && cnt == 0) || (r == 10 && (int32_t)cnt <= 0)) next = xt.lend;
                    } else {
#ifdef XT_WINDOWED
                        if(r>1){xt_fault("AKA invalid boolean branch");goto out;}
                        int taken=((xt.sr[4]>>s)&1)==r;
                        if(taken){next=pc+4u+(uint32_t)xt_sext(b2,8);cost+=2;}
#else
                        xt_fault("BF/BT (booléens) en %08x", pc); goto out;
#endif
                    }
                } else { /* BLTUI / BGEUI */
                    uint32_t c = xt_b4constu[r];
                    int taken = m == 2 ? a[s] < c : a[s] >= c;
                    if (taken) { next = pc + 4u + (uint32_t)xt_sext(b2, 8); cost += 2; }
                }
                break; }
            case 7: { /* B */
                int taken = 0;
                switch (r) {
                case 0: taken = !(a[s] & a[t]); break;                       /* BNONE */
                case 1: taken = a[s] == a[t]; break;
                case 2: taken = (int32_t)a[s] < (int32_t)a[t]; break;
                case 3: taken = a[s] < a[t]; break;
                case 4: taken = !(~a[s] & a[t]); break;                      /* BALL */
                case 5: taken = !((a[s] >> (a[t] & 31u)) & 1u); break;       /* BBC */
                case 6: case 7: taken = !((a[s] >> (t | ((r & 1u) << 4))) & 1u); break; /* BBCI */
                case 8: taken = (a[s] & a[t]) != 0; break;                   /* BANY */
                case 9: taken = a[s] != a[t]; break;
                case 10: taken = (int32_t)a[s] >= (int32_t)a[t]; break;
                case 11: taken = a[s] >= a[t]; break;
                case 12: taken = (~a[s] & a[t]) != 0; break;                 /* BNALL */
                case 13: taken = (a[s] >> (a[t] & 31u)) & 1u; break;         /* BBS */
                case 14: case 15: taken = (a[s] >> (t | ((r & 1u) << 4))) & 1u; break; /* BBSI */
                }
                if (taken) { next = pc + 4u + (uint32_t)xt_sext(b2, 8); cost += 2; }
                break; }
            default: /* 3 : FP, 4 : MAC16 */
                xt_fault("opcode %02x%02x%02x non géré en %08x", b2, b1, b0, pc);
                goto out;
            }
        }
        if (xt.halted) break;
        if (xt.lcount && next == xt.lend && !(xt.ps & XT_PS_EXCM)) { xt.lcount--; next = xt.lbeg; }
        xt.cycles += cost + xt_extra;
        lastload = loaded;
        after_store = stored;
        pc = next;
        continue;
    out:
        break;
    }
    xt.pc = pc;
}

#endif /* ESP8266_XT_H */
