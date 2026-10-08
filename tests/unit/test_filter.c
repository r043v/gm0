/* Tests des filtres d'affichage (F8) : rendu à l'échelle s, régressions contre
 * les algorithmes d'origine (cellule 3x3), couverture de la sortie, et passage
 * réel par SDL (renderer logiciel, relecture des pixels). */
#define main gm0_main
#include "../../gm0.c"
#undef main

static int fails, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "ÉCHEC %s:%d : ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

/* générateur déterministe (xorshift) */
static uint32_t rng = 0x2545f491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

#define NTEX (64 * MAX_SCREEN_W * MAX_SCREEN_H)
static uint16_t frame[MAX_SCREEN_W * MAX_SCREEN_H];
static uint32_t out[NTEX], ref[NTEX], readback[NTEX];
#define SENTINEL 0x12345678u

/* algorithmes d'origine, cellule 3x3 (référence de non-régression à s = 3) */
#define OLD_SHADE(c, num) (0xff000000u | \
    ((((c) >> 16) & 0xff) * (num) / 100) << 16 | \
    ((((c) >> 8) & 0xff) * (num) / 100) << 8 | \
    (((c) & 0xff) * (num) / 100))
static void old_dmg(const uint16_t *src, uint32_t *px) {
    for (unsigned ty = 0; ty < 3 * SCR_H; ty++) {
        uint32_t *o = px + ty * 3 * SCR_W;
        const uint16_t *line = src + (ty / 3) * SCR_W;
        unsigned j = ty % 3;
        for (unsigned x = 0; x < SCR_W; x++) {
            uint16_t p = line[x];
            uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
            uint32_t lum = (r * 616 + g * 604 + b * 224) >> 8;
            uint32_t c = dmgShade[lum >= 176 ? 3 : lum >= 120 ? 2 : lum >= 64 ? 1 : 0];
            uint32_t e = OLD_SHADE(c, 52), m = OLD_SHADE(c, 74);
            o[0] = j == 1 ? m : e;
            o[1] = j == 1 ? c : m;
            o[2] = j == 1 ? m : e;
            o += 3;
        }
    }
}
static void old_lcd_scan(const uint16_t *src, uint32_t *lcd, uint32_t *scan) {
    for (unsigned y = 0; y < SCR_H; y++)
        for (unsigned x = 0; x < SCR_W; x++) {
            uint32_t c = rgb565_argb(src[y * SCR_W + x]);
            for (unsigned j = 0; j < 3; j++)
                for (unsigned i = 0; i < 3; i++) {
                    uint32_t *l = lcd + ((y * 3 + j) * 3 * SCR_W + x * 3 + i);
                    uint32_t t = 0xff000000u;
                    for (unsigned ch = 0; ch < 3; ch++) {
                        uint32_t v = (c >> (16 - 8 * ch)) & 0xff;
                        v = v * (ch == i ? 100u : 25u) * (j == 2 ? 45u : 100u) / 10000u;
                        t |= v << (16 - 8 * ch);
                    }
                    *l = t;
                    scan[(y * 3 + j) * 3 * SCR_W + x * 3 + i] = argb_scale(c, j == 1 ? 100 : 55);
                }
        }
}

/* couverture : chaque texel de la sortie est écrit, rien au-delà */
static void check_coverage(int f, unsigned s) {
    const size_t n = (size_t)SCR_W * SCR_H * s * s;
    for (size_t i = 0; i < NTEX; i++) out[i] = SENTINEL;
    filter_render(frame, out, f, s);
    size_t holes = 0, spill = 0;
    for (size_t i = 0; i < n; i++) if (out[i] == SENTINEL) holes++;
    for (size_t i = n; i < NTEX; i++) if (out[i] != SENTINEL) spill++;
    CHECK(holes == 0, "%s s=%u : %zu texels non écrits", filter_name(f), s, holes);
    CHECK(spill == 0, "%s s=%u : %zu texels écrits hors de la sortie", filter_name(f), s, spill);
}

int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    SCR_W = 160; SCR_H = 128;
    for (unsigned i = 0; i < SCR_W * SCR_H; i++) frame[i] = (uint16_t)rnd();

    /* 1. régressions à s = 3 : DMG, LCD, scanlines identiques aux anciens */
    old_dmg(frame, ref);
    filter_render(frame, out, FILT_DMG, 3);
    CHECK(memcmp(out, ref, sizeof(uint32_t) * 3 * SCR_W * 3 * SCR_H) == 0,
          "vert à s=3 différent de l'ancien algorithme");
    static uint32_t lcd_ref[NTEX], scan_ref[NTEX];
    old_lcd_scan(frame, lcd_ref, scan_ref);
    filter_render(frame, out, FILT_LCD, 3);
    CHECK(memcmp(out, lcd_ref, sizeof(uint32_t) * 3 * SCR_W * 3 * SCR_H) == 0,
          "LCD à s=3 différent de l'ancien algorithme");
    filter_render(frame, out, FILT_SCAN, 3);
    CHECK(memcmp(out, scan_ref, sizeof(uint32_t) * 3 * SCR_W * 3 * SCR_H) == 0,
          "scanlines à s=3 différentes de l'ancien algorithme");

    /* 2. pixel à s = 4 : centre plein, bords 60 %, coins 40 % */
    filter_render(frame, out, FILT_PIXEL, 4);
    for (unsigned y = 0; y < SCR_H; y++)
        for (unsigned x = 0; x < SCR_W; x++) {
            uint32_t c = rgb565_argb(frame[y * SCR_W + x]);
            const uint32_t *b = out + (y * 4) * (4 * SCR_W) + x * 4;
            CHECK(b[1 * 4 * SCR_W + 1] == c && b[2 * 4 * SCR_W + 2] == c, "pixel (%u,%u) : centre", x, y);
            CHECK(b[0 * 4 * SCR_W + 1] == argb_scale(c, 60), "pixel (%u,%u) : bord haut", x, y);
            CHECK(b[1 * 4 * SCR_W + 0] == argb_scale(c, 60), "pixel (%u,%u) : bord gauche", x, y);
            CHECK(b[0] == argb_scale(c, 40), "pixel (%u,%u) : coin", x, y);
        }

    /* 3. grille : dernière ligne et colonne en noir, le reste à pleine luminosité */
    filter_render(frame, out, FILT_GRID, 4);
    for (unsigned y = 0; y < SCR_H; y++)
        for (unsigned x = 0; x < SCR_W; x++) {
            uint32_t c = rgb565_argb(frame[y * SCR_W + x]);
            const uint32_t *b = out + (y * 4) * (4 * SCR_W) + x * 4;
            for (unsigned j = 0; j < 4; j++)
                for (unsigned i = 0; i < 4; i++) {
                    uint32_t want = (i == 3 || j == 3) ? 0xff000000u : c;
                    CHECK(b[j * 4 * SCR_W + i] == want, "grille (%u,%u) texel %u,%u", x, y, i, j);
                }
        }

    /* 4. scanlines à 2x : lignes alternées sombre / pleine */
    filter_render(frame, out, FILT_SCAN, 2);
    for (unsigned x = 0; x < SCR_W; x++) {
        uint32_t c = rgb565_argb(frame[x]);
        CHECK(out[0 * 2 * SCR_W + 2 * x] == argb_scale(c, 55), "scan 2x : ligne 0");
        CHECK(out[1 * 2 * SCR_W + 2 * x] == c, "scan 2x : ligne 1 pleine");
    }

    /* 5. couverture de la sortie, toutes les échelles et tous les filtres */
    for (int f = FILT_PIXEL; f < FILT_RAW; f++)
        for (unsigned s = 2; s <= FILT_SCALE_MAX; s++)
            check_coverage(f, s);

    /* 6. SDL : la texture est à l'échelle de la fenêtre, et la sortie est recopiée
     * à l'identique (aucun rééchantillonnage). Renderer logiciel, fenêtre cachée. */
    CHECK(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL_Init : %s", SDL_GetError());
    SDL_Window *win = SDL_CreateWindow("test", 0, 0, 320, 256, SDL_WINDOW_HIDDEN);
    CHECK(win != NULL, "fenêtre : %s", SDL_GetError());
    emuRen = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    CHECK(emuRen != NULL, "renderer : %s", SDL_GetError());
    memcpy(pix, frame, sizeof frame);
    static const unsigned winSizes[][2] = { {320, 256}, {480, 384}, {640, 512}, {200, 150} };
    for (unsigned w = 0; w < sizeof winSizes / sizeof winSizes[0]; w++) {
        SDL_SetWindowSize(win, (int)winSizes[w][0], (int)winSizes[w][1]);
        for (int f = FILT_PIXEL; f < FILT_COUNT; f++) {
            dispFilter = f;
            scale_apply();
            blit(emuRen);
            unsigned s = f == FILT_RAW ? 1u : output_scale(emuRen);
            if (s < 2) s = 1;
            int tw = 0, th = 0; Uint32 fmt; int acc;
            SDL_QueryTexture(tex, &fmt, &acc, &tw, &th);
            CHECK(tw == (int)(SCR_W * s) && th == (int)(SCR_H * s),
                  "%dx%d %s : texture %dx%d au lieu de %ux%u", (int)winSizes[w][0], (int)winSizes[w][1],
                  filter_name(f), tw, th, SCR_W * s, SCR_H * s);
            /* relecture : la sortie (texture copiée sans mise à l'échelle) doit être
             * exactement l'image calculée dans px32 */
            if (s >= 1 && tw == (int)(SCR_W * s)) {
                SDL_SetRenderDrawColor(emuRen, 0, 0, 0, 255);
                SDL_RenderClear(emuRen);
                SDL_RenderCopy(emuRen, tex, NULL, NULL);
                int ow = 0, oh = 0;
                SDL_GetRendererOutputSize(emuRen, &ow, &oh);
                CHECK(SDL_RenderReadPixels(emuRen, NULL, SDL_PIXELFORMAT_ARGB8888,
                                           readback, ow * 4) == 0, "relecture : %s", SDL_GetError());
                if (ow == tw && oh == th) {
                    size_t diff = 0;
                    for (int i = 0; i < tw * th; i++)
                        if ((readback[i] | 0xff000000u) != (px32[i] | 0xff000000u)) diff++;
                    CHECK(diff == 0, "%dx%d %s : %zu texels modifiés par la sortie",
                          (int)winSizes[w][0], (int)winSizes[w][1], filter_name(f), diff);
                }
            }
        }
    }
    SDL_DestroyRenderer(emuRen);
    SDL_DestroyWindow(win);
    SDL_Quit();

    printf("%d vérifications, %d échec(s)\n", checks, fails);
    return fails ? 1 : 0;
}
