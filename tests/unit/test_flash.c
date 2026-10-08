/* Test unitaire de la programmation flash META (NVMCTRL, datasheet 22.8) :
 * l'écriture ne peut que faire passer des bits de 1 à 0 (ET avec le contenu
 * de la flash) ; seul un effacement de rangée (ER) remet des 1.  Le fichier
 * inclut gm0.c pour atteindre flash_store et nvmctrl_write (fonctions
 * statiques, hors de l'API publique).  Lancer tests/unit/run.sh. */
#define main gm0_main
#include "../../gm0.c"
#undef main

static int fails, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "ÉCHEC %s:%d : ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

static void erase_row(uint32_t addr) {
    nvmctrl_write(0x1c, addr >> 1, 0xffffffffu);          /* ADDR, en demi-mots */
    nvmctrl_write(0x00, 0xa502u, 0xffffu);                 /* CTRLA : CMDEX 0xA5 | ER */
}

int main(void) {
    /* 1. ER remet la rangée à 0xFF, les rangées voisines sont intactes */
    memset(flash, 0x00, FLASH_SIZE);
    erase_row(0);
    for (uint32_t i = 0; i < 256; i++) CHECK(flash[i] == 0xff, "ER : flash[%u] = %02x", i, flash[i]);
    CHECK(flash[256] == 0x00, "ER déborde sur la rangée suivante : flash[256] = %02x", flash[256]);

    /* 2. écriture mot sur une zone effacée : valeur écrite telle quelle */
    flash_store(0, 0x1234, 2);
    CHECK(flash[0] == 0x34 && flash[1] == 0x12, "mot 0x1234 : %02x %02x", flash[0], flash[1]);

    /* 3. écriture sans effacement : seuls les 1 → 0 passent (0x1234 & 0xFF00) */
    flash_store(0, 0xff00, 2);
    CHECK(flash[0] == 0x00 && flash[1] == 0x12,
          "écriture sans ER : attendu 00 12 (ET), obtenu %02x %02x", flash[0], flash[1]);

    /* 4. octet : 0x0F puis 0xF0 sur 0xFF → 0x0F puis 0x00 */
    erase_row(0x100);
    flash_store(0x102, 0x0f, 1);
    CHECK(flash[0x102] == 0x0f, "octet 0x0F : %02x", flash[0x102]);
    flash_store(0x102, 0xf0, 1);
    CHECK(flash[0x102] == 0x00, "octet 0xF0 sur 0x0F : attendu 00, obtenu %02x", flash[0x102]);

    /* 5. écriture à cheval sur la fin de la flash : ignorée */
    flash[FLASH_SIZE - 1] = 0x77;
    flash_store(FLASH_SIZE - 1, 0x1234, 2);
    CHECK(flash[FLASH_SIZE - 1] == 0x77, "écriture à cheval sur la fin : %02x", flash[FLASH_SIZE - 1]);

    printf("%d vérifications, %d échec(s)\n", checks, fails);
    return fails ? 1 : 0;
}
