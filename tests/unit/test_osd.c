/* Tests de l'OSD (messages temporaires) : pile, échéances indépendantes,
 * dessin en bas à gauche dans le tampon d'image.  Sans OSD (GM0_NO_OSD), le
 * test ne fait rien et réussit : la désactivation est ainsi testée aussi. */
#define main gm0_main
#include "../../gm0.c"
#undef main

static int fails, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "ÉCHEC %s:%d : ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

#ifdef GM0_NO_OSD
int main(void) {
    printf("OSD désactivé (GM0_NO_OSD) : rien à tester\n");
    return 0;
}
#else
static uint32_t buf[16 * MAX_SCREEN_W * MAX_SCREEN_H];
static uint32_t snap[16 * MAX_SCREEN_W * MAX_SCREEN_H];

static int text_is(unsigned k, const char *want) {
    return k < osdCount && strcmp(osdLines[k].text, want) == 0;
}

int main(void) {
    SCR_W = 160; SCR_H = 128;

    /* 1. chaque entrée expire selon sa propre échéance */
    osdCount = 0;
    osd_push_at(0, "A");
    osd_push_at(1000, "B");
    osd_expire(2999);
    CHECK(osdCount == 2 && text_is(0, "A") && text_is(1, "B"), "avant 3000 ms : %u entrées", osdCount);
    osd_expire(3000);
    CHECK(osdCount == 1 && text_is(0, "B"), "à 3000 ms, A doit être partie seule (reste %u)", osdCount);
    osd_expire(3999);
    CHECK(osdCount == 1, "B encore affichée à 3999 ms");
    osd_expire(4000);
    CHECK(osdCount == 0, "B partie à 4000 ms");

    /* 2. ordre : du plus ancien (index 0) au plus récent */
    osdCount = 0;
    osd_push_at(0, "X");
    osd_push_at(0, "Y");
    osd_push_at(0, "Z");
    CHECK(text_is(0, "X") && text_is(1, "Y") && text_is(2, "Z"), "ordre d'empilement");

    /* 3. pile pleine : la plus ancienne cède la place */
    osdCount = 0;
    for (unsigned i = 0; i < OSD_SLOTS + 2; i++) {
        char t[16];
        snprintf(t, sizeof t, "E%u", i);
        osd_push_at(0, "%s", t);
    }
    CHECK(osdCount == OSD_SLOTS, "pile pleine : %u entrées", osdCount);
    CHECK(text_is(0, "E2") && text_is(OSD_SLOTS - 1, "E7"), "la plus ancienne doit tomber");

    /* 4. casse et caractères inconnus */
    CHECK(osd_glyph('a') == osd_glyph('A'), "minuscule non ramenée en majuscule");
    CHECK(osd_glyph('@') == osd_glyph('?'), "caractère inconnu non remplacé par '?'");

    /* 5. dessin : la plus récente en bas à gauche, les autres au-dessus */
    for (unsigned cell = 1; cell <= 3; cell += 2) {
        const unsigned stride = SCR_W * cell;
        for (unsigned i = 0; i < SCR_W * SCR_H * cell * cell; i++) buf[i] = 0xff808080u;
        osdCount = 0;
        osd_push_at(0, "OLD");   /* plus ancienne : au-dessus */
        osd_push_at(0, "NEW");   /* plus récente : en bas */
        memcpy(snap, buf, sizeof(uint32_t) * SCR_W * SCR_H * cell * cell);
        osd_draw(buf, stride, cell, 0);
        int top0 = (int)SCR_H - OSD_MARGIN - OSD_BOX;      /* ligne la plus récente */
        int top1 = top0 - OSD_STEP;                        /* la précédente, au-dessus */
        /* 'N' ligne 0 "#...#" : pixel (3, top0+1) allumé */
        uint32_t t = buf[((top0 + 1) * cell) * stride + 3 * cell];
        CHECK(t == 0xffffffffu, "cell %u : 'N' de la ligne récente absente en bas à gauche", cell);
        /* 'O' ligne 0 ".###." : pixel (4, top1+1) allumé, au-dessus */
        t = buf[((top1 + 1) * cell) * stride + 4 * cell];
        CHECK(t == 0xffffffffu, "cell %u : 'O' de la ligne ancienne absente au-dessus", cell);
        /* fond de la boîte assombri à 35 % (x = 20 : dans la boîte, hors texte) */
        t = buf[(top0 * cell) * stride + 20 * cell];
        CHECK(t == argb_scale(0xff808080u, 35), "cell %u : fond pas assombri (%08x)", cell, t);
        /* hors de la boîte : intact */
        t = buf[(top0 * cell) * stride + 40 * cell];
        CHECK(t == 0xff808080u, "cell %u : pixel hors boîte modifié", cell);
        /* sous la boîte (marge du bas) : intact */
        t = buf[((SCR_H - 1) * cell) * stride + 10 * cell];
        CHECK(t == 0xff808080u, "cell %u : marge du bas modifiée", cell);
    }

    /* 6. pile vide ou échue : le tampon ne bouge pas */
    osdCount = 0;
    for (unsigned i = 0; i < SCR_W * SCR_H; i++) buf[i] = 0xff123456u;
    memcpy(snap, buf, sizeof(uint32_t) * SCR_W * SCR_H);
    osd_draw(buf, SCR_W, 1, 0);
    CHECK(memcmp(buf, snap, sizeof(uint32_t) * SCR_W * SCR_H) == 0, "pile vide : tampon modifié");
    osd_push_at(0, "GONE");
    osd_draw(buf, SCR_W, 1, OSD_MS);
    CHECK(osdCount == 0 && memcmp(buf, snap, sizeof(uint32_t) * SCR_W * SCR_H) == 0,
          "entrée échue encore dessinée");

    printf("%d vérifications, %d échec(s)\n", checks, fails);
    return fails ? 1 : 0;
}
#endif
