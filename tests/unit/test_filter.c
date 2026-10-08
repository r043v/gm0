/* Tests des filtres d'affichage (F8) : rendu hors écran, puis passage par
 * blit() avec un vrai renderer SDL (pilote vidéo factice).  Le filtre vert
 * est comparé octet à octet à l'algorithme écrit avant le cycle de filtres. */
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

static uint16_t frame[MAX_SCREEN_W * MAX_SCREEN_H];
static uint32_t out[16 * MAX_SCREEN_W * MAX_SCREEN_H];
static uint32_t ref[16 * MAX_SCREEN_W * MAX_SCREEN_H];

/* filtre vert tel qu'écrit avant le cycle (référence de non-régression) */
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

int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    SCR_W = 160; SCR_H = 128;
    for (unsigned i = 0; i < SCR_W * SCR_H; i++) frame[i] = (uint16_t)rnd();

    /* 1. vert : identique à l'ancien algorithme sur une image aléatoire */
    old_dmg(frame, ref);
    filter_render(frame, out, FILT_DMG);
    CHECK(memcmp(out, ref, sizeof(uint32_t) * 3 * SCR_W * 3 * SCR_H) == 0,
          "filtre vert différent de l'ancien algorithme");

    /* 2. pixel : centre plein, bords 60 %, coins 40 % (cellule 4x4) */
    filter_render(frame, out, FILT_PIXEL);
    for (unsigned y = 0; y < SCR_H; y++)
        for (unsigned x = 0; x < SCR_W; x++) {
            uint32_t c = rgb565_argb(frame[y * SCR_W + x]);
            const uint32_t *cellp = out + (y * 4) * (4 * SCR_W) + x * 4;
            CHECK(cellp[1 * 4 * SCR_W + 1] == c && cellp[2 * 4 * SCR_W + 2] == c,
                  "pixel (%u,%u) : centre pas plein", x, y);
            CHECK(cellp[0 * 4 * SCR_W + 1] == argb_scale(c, 60),
                  "pixel (%u,%u) : bord haut pas à 60 %%", x, y);
            CHECK(cellp[1 * 4 * SCR_W + 0] == argb_scale(c, 60),
                  "pixel (%u,%u) : bord gauche pas à 60 %%", x, y);
            CHECK(cellp[0] == argb_scale(c, 40), "pixel (%u,%u) : coin pas à 40 %%", x, y);
        }

    /* 3. LCD RGB : canal i plein en colonne i, les deux autres à 25 %, ligne basse à 45 % */
    filter_render(frame, out, FILT_LCD);
    for (unsigned y = 0; y < SCR_H; y++)
        for (unsigned x = 0; x < SCR_W; x++) {
            uint32_t c = rgb565_argb(frame[y * SCR_W + x]);
            const uint32_t *cellp = out + (y * 3) * (3 * SCR_W) + x * 3;
            for (unsigned i = 0; i < 3; i++) {
                for (unsigned ch = 0; ch < 3; ch++) {
                    uint32_t v = (c >> (16 - 8 * ch)) & 0xff;
                    uint32_t want = v * (ch == i ? 100u : 25u) / 100u;
                    uint32_t got = (cellp[1 * 3 * SCR_W + i] >> (16 - 8 * ch)) & 0xff;
                    CHECK(got == want, "lcd (%u,%u) col %u canal %u : %u au lieu de %u",
                          x, y, i, ch, got, want);
                    /* ligne basse : même valeur, assombrie à 45 % */
                    uint32_t low = (cellp[2 * 3 * SCR_W + i] >> (16 - 8 * ch)) & 0xff;
                    CHECK(low == v * (ch == i ? 100u : 25u) * 45u / 10000u,
                          "lcd (%u,%u) col %u canal %u : ligne basse %u", x, y, i, ch, low);
                }
            }
        }

    /* 4. scanlines : ligne centrale pleine, les deux autres à 55 % */
    filter_render(frame, out, FILT_SCAN);
    for (unsigned y = 0; y < SCR_H; y++)
        for (unsigned x = 0; x < SCR_W; x++) {
            uint32_t c = rgb565_argb(frame[y * SCR_W + x]);
            const uint32_t *cellp = out + (y * 3) * (3 * SCR_W) + x * 3;
            for (unsigned i = 0; i < 3; i++) {
                CHECK(cellp[1 * 3 * SCR_W + i] == c, "scan (%u,%u) : ligne centrale", x, y);
                CHECK(cellp[0 * 3 * SCR_W + i] == argb_scale(c, 55)
                      && cellp[2 * 3 * SCR_W + i] == argb_scale(c, 55),
                      "scan (%u,%u) : lignes sombres pas à 55 %%", x, y);
            }
        }

    /* 5. passage par blit() avec un vrai renderer : la texture a la taille de la cellule */
    CHECK(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL_Init : %s", SDL_GetError());
    SDL_Window *win = SDL_CreateWindow("test", 0, 0, 160, 128, SDL_WINDOW_HIDDEN);
    CHECK(win != NULL, "fenêtre : %s", SDL_GetError());
    emuRen = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    CHECK(emuRen != NULL, "renderer : %s", SDL_GetError());
    memcpy(pix, frame, sizeof frame);
    for (int f = 0; f < FILT_COUNT; f++) {
        dispFilter = f;
        scale_apply();
        blit(emuRen);
        int w = 0, h = 0; Uint32 fmt; int acc;
        SDL_QueryTexture(tex, &fmt, &acc, &w, &h);
        unsigned cell = filter_cell(f);
        CHECK(w == (int)(SCR_W * cell) && h == (int)(SCR_H * cell),
              "filtre %s : texture %dx%d au lieu de %ux%u", filter_name(f), w, h,
              SCR_W * cell, SCR_H * cell);
    }
    SDL_DestroyRenderer(emuRen);
    SDL_DestroyWindow(win);
    SDL_Quit();

    printf("%d vérifications, %d échec(s)\n", checks, fails);
    return fails ? 1 : 0;
}
