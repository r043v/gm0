/* Programme de test : le même code tourne sur l'hôte (cc) et sur le cœur
 * Xtensa (xtensa-lx106-elf-gcc), les empreintes doivent être identiques. */
#include <stdint.h>
#include <stddef.h>

static uint32_t h;
static void mix(uint32_t v) { h = (h ^ v) * 16777619u; h = (h << 7) | (h >> 25); }

static uint8_t buf8[257];
static uint16_t buf16[130];
static int32_t buf32[67];
static const uint8_t tbl[16] = {3, 141, 59, 26, 53, 58, 97, 93, 23, 84, 62, 64, 33, 83, 27, 95};

static int only = -1;
#define OP(e) do { if (only < 0 || only == __COUNTER__) mix(e); } while (0)
static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }

static int sw(int x) {
    switch (x & 15) {
    case 0: return 11; case 1: return 22; case 2: return 33; case 3: return x * 3;
    case 4: return x ^ 0x55; case 5: return -x; case 6: return x << 3; case 7: return x >> 1;
    case 8: return 1000; case 9: return 2000; case 10: return x * x; case 11: return 7;
    case 12: return x - 5; case 13: return ~x; case 14: return x | 0x100; default: return 0;
    }
}
static unsigned fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
typedef struct { int16_t a; int8_t b; uint8_t c; int32_t d; uint16_t e; } rec;
static rec recs[8];
static int cmp_u(const void *x, const void *y) { return (int)(*(const uint32_t *)x - *(const uint32_t *)y); }
static void isort(uint32_t *v, int n) {
    for (int i = 1; i < n; i++) { uint32_t k = v[i]; int j = i - 1; while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; } v[j + 1] = k; }
}
static uint64_t u64s[8];

uint32_t compute(uint32_t stage) {
    only = stage >= 100 ? (int)stage - 100 : -1;
    if (stage >= 100) stage = 1;
    uint32_t s = 12345;
    h = 2166136261u;
    /* arithmétique 32 bits, signée et non signée */
    for (int i = 0; i < 400; i++) {
        uint32_t a = lcg(&s), b = lcg(&s) | 1u;
        int32_t sa = (int32_t)a, sb = (int32_t)b;
        OP(a + b); OP(a - b); OP(a * b); OP(a & b); OP(a | b); OP(a ^ b);
        OP(a / b); OP(a % b);
        if (sb != -1) { OP((uint32_t)(sa / sb)); OP((uint32_t)(sa % sb)); }
        OP(a << (b & 31)); OP(a >> (b & 31)); OP((uint32_t)(sa >> (b & 31)));
        OP((uint32_t)(sa < sb)); OP((uint32_t)(a < b)); OP((uint32_t)(sa >= (int32_t)(b >> 3)));
        OP((uint32_t)(int16_t)a); OP((uint32_t)(int8_t)b); OP((uint16_t)a); OP((uint8_t)b);
        OP((a >> 13) & 0x3ff); OP((a >> 3) & 0xf); OP(a >> 31);
        OP((uint32_t)__builtin_clz(b)); OP((uint32_t)(sa < 0 ? -sa : sa));
        OP(sa < sb ? a : b); OP(a > b ? a : b); OP((uint32_t)(sa > sb ? sa : sb));
        OP(a * 5u); OP(a * 9u + b); OP((a << 2) - b); OP(a * 7u); OP(a * 100u);
    }
    if (stage == 1) return h;
    /* 64 bits */
    for (int i = 0; i < 8; i++) { uint64_t x = ((uint64_t)lcg(&s) << 32) | lcg(&s); u64s[i] = x; }
    for (int i = 0; i < 8; i++) {
        uint64_t x = u64s[i], y = u64s[(i + 3) & 7] | 1u;
        uint64_t m = x * y, d = x / (y >> 20 | 1u), r = x % (y >> 20 | 1u);
        mix((uint32_t)m); mix((uint32_t)(m >> 32)); mix((uint32_t)d); mix((uint32_t)(d >> 32)); mix((uint32_t)r);
        mix((uint32_t)(x << (i * 5 + 1))); mix((uint32_t)((x << (i * 5 + 1)) >> 32));
        mix((uint32_t)(x >> (i * 7 + 2))); mix((uint32_t)(((int64_t)x >> (i * 7 + 2)) >> 32));
        mix((uint32_t)(x + y)); mix((uint32_t)((x + y) >> 32)); mix((uint32_t)(x < y));
        int64_t sx = (int64_t)x, sy = (int64_t)(y >> 11) | 1;
        mix((uint32_t)(sx / sy)); mix((uint32_t)(sx % sy));
    }
    if (stage == 2) return h;
    /* mémoire : octets, demi-mots, mots, structures */
    for (unsigned i = 0; i < sizeof buf8; i++) buf8[i] = (uint8_t)(lcg(&s) >> 11);
    for (unsigned i = 0; i < 130; i++) buf16[i] = (uint16_t)(lcg(&s) >> 9);
    for (unsigned i = 0; i < 67; i++) buf32[i] = (int32_t)lcg(&s);
    for (unsigned i = 0; i < sizeof buf8; i++) mix(buf8[i] + tbl[i & 15]);
    for (unsigned i = 0; i < 130; i++) { mix(buf16[i]); mix((uint32_t)(int16_t)buf16[i]); }
    for (unsigned i = 0; i < 67; i++) mix((uint32_t)buf32[i]);
    for (unsigned i = 0; i < 8; i++) {
        recs[i].a = (int16_t)lcg(&s); recs[i].b = (int8_t)lcg(&s); recs[i].c = (uint8_t)lcg(&s);
        recs[i].d = (int32_t)lcg(&s); recs[i].e = (uint16_t)lcg(&s);
    }
    for (unsigned i = 0; i < 8; i++) {
        rec r = recs[(i + 1) & 7];
        mix((uint32_t)r.a); mix((uint32_t)r.b); mix(r.c); mix((uint32_t)r.d); mix(r.e);
    }
    if (stage == 3) return h;
    /* commutateurs, récursion, tri, pointeurs de fonctions */
    for (int i = 0; i < 64; i++) mix((uint32_t)sw(i * 7 + 1));
    mix(fib(15));
    {
        uint32_t v[40];
        for (int i = 0; i < 40; i++) v[i] = lcg(&s) >> 5;
        isort(v, 40);
        for (int i = 0; i < 40; i++) mix(v[i]);
        (void)cmp_u;
    }
    if (stage == 4) return h;
    /* flottants (libgcc logicielle) */
    {
        float f = 1.5f; double d = 2.25;
        for (int i = 1; i < 30; i++) {
            f = f * 1.0625f + (float)i; d = d / 1.125 - (double)i * 0.5;
            mix((uint32_t)(int32_t)f); mix((uint32_t)(int32_t)d); mix((uint32_t)(f > (float)d));
        }
        union { float f; uint32_t u; } fu = { f }; mix(fu.u);
        union { double d; uint64_t u; } du = { d }; mix((uint32_t)du.u); mix((uint32_t)(du.u >> 32));
    }
    if (stage == 5) return h;
    return h;
}
