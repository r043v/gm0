/*
 * meta_emu.c — émulateur Gamebuino META en C/SDL2, port du fork TypeScript
 * (gamebuino-emulator + nos ajouts : exécution SRAM, carte SD, TC4/DAC).
 *
 * Fidèle à meta_audio.cpp / meta_main.cpp / meta_sd.cpp du firmware gbl :
 *  - TC4 en 0x42003000, interruption IRQ19 toutes les 907 instructions
 *    (369 tirs par frame, comme la carte attend) ;
 *  - DAC DATA en 0x42004808 -> sortie audio SDL (22 049 Hz) ;
 *  - carte SD SPI en PA27, image brute (.img) ou dossier… non : image
 *    brute uniquement ici (le C n'a pas besoin de plus pour tester) ;
 *  - écran ST7735 160x128 rendu dans une fenêtre SDL2 x2 ;
 *  - boutons sur PB03 : flèches, J=A, K=B, U=MENU, I=HOME, Entrée=Start
 *    (HOME tenu 3 s = reset du jeu, comme sur la console).
 *
 * Usage : meta_emu <firmware.bin> [carte.img] [--wav out.wav]
 *
 * La cible est aussi détectée automatiquement : un binaire dont le mot 0
 * (SP initial) pointe dans la SRAM LPC (0x1000xxxx) démarre en mode
 * POKITTO (LPC11U68, port C du PokittoEmu de felipemanga) — conteneur
 * .pop du loader géré.  --target force la cible.
 */
#include <SDL.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>

/* ---------------------------------------------------------------- état */

#define FLASH_SIZE 0x40000u
/* 40 Ko : la SRAM du SAMD21 fait 32 Ko mais les jeux lib récente placent
 * leur buffer de réception SD (SdFat) vers 0x20008860 — hors des 32 Ko —
 * et attendent des écritures fonctionnelles à cette adresse ; ce surplus
 * reste inutilisé par les autres jeux */
#define SRAM_SIZE  0xa000u

/* Le tampon d'écran doit contenir la plus grande des deux cibles
 * (160x128 META, 220x176 Pokitto) ; SCR_W/SCR_H suivent la cible. */
#define MAX_SCREEN_W 220u
#define MAX_SCREEN_H 176u

static uint8_t  flash[FLASH_SIZE];
static uint8_t  sram[SRAM_SIZE];
static uint32_t regs[16];
/* ombres flottantes des registres : le TS ne masque pas le résultat de MUL
 * (les registres JS deviennent des doubles, arrondis au-delà de 2^53), ce
 * qui change les bits bas des checksums bouclés.  regD porte la valeur
 * brute, regs la valeur modulo 2^32 utilisée partout ailleurs. */
static double regD[16];
static int fN, fZ, fC, fV;
static uint32_t tickCount;
static int sysTickTrigger;
static uint32_t vectorBase;
static uint32_t sysTickVector, dmacVector, tc4Vector;
static int dmacInterrupt, tc4Interrupt;
static long sysTickEntries;

/* périphériques — PA27 (CS carte SD) et PA25 (CS boutons) hauts dès le
 * boot : pull-ups réelles de la carte, les périphériques n'écoutent le
 * SPI que si le firmware les sélectionne (le boot initial ne passe PAS
 * par reset_core ; les jeux sans lib standard ne sélectionnent jamais
 * ce qu'ils n'utilisent pas — sinon leur trafic SPI écran partait dans
 * la machine SD) */
static uint32_t portA_out = (1u << 27) | (1u << 25), portB_out, portA_dir, portB_dir;
static uint8_t  ser4_data = 0x80;
static uint8_t  buttonData = 0xff;

/* TC4 + DAC */
static int      tc4Enabled, tc4Armed;
static uint8_t  tc4IntEnMask, tc4IntFlagMask; /* INTENSET/INTFLAG lisibles (jeux maison) */
static uint32_t tc4CtrlA; /* valeur complète de CTRLA (prescaler bits 8-10) */
static uint32_t tc4Top, tc4Counter, tc4Period = 907;
static uint32_t tc4Window, tc4Fires, tc4Writes;
static int      tc4Interrupt;

/* ST7735 */
static uint16_t pix[MAX_SCREEN_W * MAX_SCREEN_H];
static int lcd_xStart, lcd_xEnd, lcd_yStart, lcd_yEnd, lcd_x, lcd_y;
static int lcd_argIndex, lcd_lastCommand, lcd_tmp;
static int ramwrCount; /* compte les RAMWR : ~32 par frame rendue */

/* carte SD (PA27) */
static uint8_t *sd_image = NULL;
static size_t   sd_size = 0;
static uint8_t  sd_pending = 0xff;
/* file de réponse : le TS utilise un tableau JS non borné — une commande
 * peut arriver avant que la réponse précédente soit drainée (jeu avec son
 * propre pilote SD) ; on réplique avec un tampon qui grandit, plafonné */
static uint8_t *sd_out = NULL;
static int      sd_outCap = 0;
static int      sd_outLen = 0, sd_outPos = 0;
static uint8_t  sd_cmdBuf[6];
static int      sd_cmdIdx = 0;
static int      sd_writing = 0, sd_writeIdx = 0, sd_writeLba = 0;

#define SD_OUT_MAX (1u << 20)
static void sd_out_push(uint8_t b) {
    if (sd_outLen >= SD_OUT_MAX) return; /* déraille : on n'accumule plus */
    if (sd_outLen >= sd_outCap) {
        int cap = sd_outCap ? sd_outCap * 2 : 512;
        if (cap > SD_OUT_MAX) cap = SD_OUT_MAX;
        uint8_t *n = realloc(sd_out, (size_t)cap);
        if (!n) return;
        sd_out = n; sd_outCap = cap;
    }
    sd_out[sd_outLen++] = b;
}
static void sd_out_append(const uint8_t *p, int n) {
    if (sd_outLen + n > SD_OUT_MAX) return;
    if (sd_outLen + n > sd_outCap) {
        int cap = sd_outCap ? sd_outCap : 512;
        while (cap < sd_outLen + n) cap *= 2;
        if (cap > SD_OUT_MAX) cap = SD_OUT_MAX;
        uint8_t *b = realloc(sd_out, (size_t)cap);
        if (!b) return;
        sd_out = b; sd_outCap = cap;
    }
    memcpy(sd_out + sd_outLen, p, (size_t)n);
    sd_outLen += n;
}
static int      sd_initialized = 0;
static uint8_t  sd_writeBuf[515];

/* audio SDL : anneau producteur (ISR) -> consommateur (callback) */
#define AQ_SIZE 65536
/* plafond de latence son/image : au-delà, on jette le plus ancien.  Sans
 * ça, chaque burst du rAF (onglet réveillé, stall) accumule un retard
 * définitif — l'anneau ne se corrige jamais tout seul (3 s = 65536). */
#define AQ_LATENCY 900
static int16_t aq[AQ_SIZE];
static volatile int aq_head, aq_tail; /* tail = écrit, head = lu */
static int16_t audioHold = 0;
static int audioPending; /* device armé en pause : attend le pré-buffer */

static SDL_AudioDeviceID audioDev;
static int audioOk;
static FILE *wavFile;
static uint32_t wavSamples;
static char wavPathStr[512];
static char shotPath[512];
static uint32_t maxFrames;

/* ------------------------------------------------------- cibles ------- */

#define TGT_META    0
#define TGT_POKITTO 1
static int emuTarget = TGT_META;   /* fixé par --target ou détection */
static int targetForced;           /* --target explicite */
static int armIrqEnable = 1;       /* PRIMASK inversé (CPSIE/CPSID, Pokitto) */

static unsigned SCR_W = 160, SCR_H = 128;

/* ticks émulés par seconde : hack TS (20 M ticks/s) pour la META ; pour la
 * Pokitto, horloge réelle de la référence (SYSPLLCTRL 0x23 -> 45 MHz) */
static uint32_t frame_ticks(void) {
    return emuTarget == TGT_POKITTO ? 753431u : 334860u;
}
static double ticks_per_sec(void) {
    return emuTarget == TGT_POKITTO ? 45000000.0 : 20000000.0;
}

/* prototypes du bloc Pokitto (défini après la section FAT/zip) */
static uint32_t pk_read_word(uint32_t a);
static uint16_t pk_read_half(uint32_t a);
static uint8_t  pk_read_byte(uint32_t a);
static void     pk_write_word(uint32_t a, uint32_t v);
static void     pk_write_half(uint32_t a, uint16_t v);
static void     pk_write_byte(uint32_t a, uint8_t v);
static void     pk_reset_core(void);
static void     pk_machine_step(void);
static void     pk_blx(uint32_t opcode);
static void     pk_interrupt(uint32_t id);
static void     pk_adc_frame(void);
static void     pk_eeprom_save(void);
static void     pk_card_export(void);
static void     pk_btn_gpio(uint8_t mask, int pressed);
static int      pk_audio_ready(void);
static void     pk_sd_machine_reset(void);
static void     pk_screen_reconfig(void);

/* ----------------------------------------------------------- mémoire */

static uint32_t fetchWord(uint32_t a);
static uint16_t fetchHalf(uint32_t a);
static uint8_t  fetchByte(uint32_t a);
static void     writeWord(uint32_t a, uint32_t v);
static void     writeHalf(uint32_t a, uint16_t v);
static void     writeByte(uint32_t a, uint8_t v);
static void     pushStack(uint32_t v);
static uint32_t popStack(void);
static void     setReg(int i, uint32_t v);
static void     incrementPc(void);
static void     irq_inject(uint32_t vector);
static void     sercom4_write(uint8_t v);
static uint8_t  st7735_byte(uint8_t v);

/* ports : OUT/OUTSET/OUTCLR/OUTTGL/DIR* (comme port-register.ts) */
static void port_write(int group, uint32_t off, uint32_t v) {
    uint32_t *outp = group ? &portB_out : &portA_out;
    uint32_t *dirp = group ? &portB_dir : &portA_dir;
    switch (off) {
        case 0x00: *dirp ^= v; break;
        case 0x04: *dirp &= ~v; break;
        case 0x08: *dirp |= v; break;
        case 0x0c: *dirp ^= v; break;
        case 0x10: *outp = v; break;
        case 0x14: *outp &= ~v; break;
        case 0x18: *outp |= v; break;
        case 0x1c: *outp ^= v; break;
    }
}

static uint32_t port_read(int group, uint32_t off) {
    uint32_t out = group ? portB_out : portA_out;
    uint32_t dir = group ? portB_dir : portA_dir;
    switch (off) {
        case 0x00: case 0x04: case 0x08: case 0x0c: return dir;
        case 0x10: case 0x14: case 0x18: case 0x1c: return out;
        case 0x20: return 0xffffffff; /* IN : entrées hautes */
    }
    return 0;
}

/* ------------------------------------------------------------- DAC/audio */

static void audio_push(int16_t s) {
    int next = (aq_tail + 1) % AQ_SIZE;
    if (next == aq_head) aq_head = (aq_head + 1) % AQ_SIZE; /* plein : jette le plus ancien */
    aq[aq_tail] = s;
    aq_tail = next;
    int ahead = aq_tail - aq_head;
    if (ahead < 0) ahead += AQ_SIZE;
    if (ahead > AQ_LATENCY) aq_head = (aq_head + ahead - AQ_LATENCY) % AQ_SIZE;
}

/* à appeler chaque itération de boucle : ouvre le gate quand le
 * pré-buffer est atteint (aucun appel SDL : tout passe par le callback) */
static void audio_resume_when_ready(void) {
    if (!audioPending) return;
    if (emuTarget == TGT_POKITTO) {
        if (pk_audio_ready()) audioPending = 0;
        return;
    }
    int ahead = aq_tail - aq_head;
    if (ahead < 0) ahead += AQ_SIZE;
    if (ahead >= 600) audioPending = 0;
}

static void wav_put(int16_t s) {
    if (!wavFile) return;
    uint8_t b[2] = { (uint8_t)(s & 0xff), (uint8_t)((s >> 8) & 0xff) };
    fwrite(b, 1, 2, wavFile);
    wavSamples++;
}

static void wav_finish(void) {
    if (!wavFile) return;
    uint32_t bsize = wavSamples * 2, rsize = 36 + bsize;
    uint8_t w4[4];
    fseek(wavFile, 4, SEEK_SET);
    w4[0] = (uint8_t)rsize; w4[1] = (uint8_t)(rsize >> 8); w4[2] = (uint8_t)(rsize >> 16); w4[3] = (uint8_t)(rsize >> 24);
    fwrite(w4, 1, 4, wavFile);
    fseek(wavFile, 40, SEEK_SET);
    w4[0] = (uint8_t)bsize; w4[1] = (uint8_t)(bsize >> 8); w4[2] = (uint8_t)(bsize >> 16); w4[3] = (uint8_t)(bsize >> 24);
    fwrite(w4, 1, 4, wavFile);
    fclose(wavFile);
    wavFile = NULL;
    printf("WAV : %u échantillons (%.1f s)\n", wavSamples, (double)wavSamples / 22049.0);
}

static void dac_write(uint16_t v) {
    tc4Writes++;
    v &= 0x3ffu; /* le TS masque sur 10 bits (DAC->DATA & 0x3ff) */
    /* v : 256..766 (milieu 512) -> s16 */
    int16_t s = (int16_t)((v - 511) * 96);
    audio_push(s);
    wav_put(s);
}

/* --------------------------------------------------- carte SD (PA27) */

static int sd_selected(void) { return (portA_out & (1u << 27)) == 0; }

/* même modèle que sdcard.ts : l'octet que la carte pilote pendant l'échange
 * N a été décidé par l'octet reçu pendant l'échange N-1.  sd_pending tient
 * ce décalage, sd_out est la file des octets à venir. */
static void sd_reset_state(void) {
    sd_pending = 0xff;
    sd_outLen = 0; sd_outPos = 0;
    sd_cmdIdx = 0;
    sd_writing = 0;
}

/* carte courante : image brute (.img) ou image FAT construite d'un dossier
 * (fatImage/fatImageSize sont définis avec le constructeur FAT plus bas) */
static uint8_t *fatImage;
static size_t   fatImageSize;
static uint8_t *sd_card_data(void) {
    return sd_image ? sd_image : fatImage;
}
static size_t sd_card_size(void) {
    return sd_image ? sd_size : (fatImage ? fatImageSize : 0);
}

static int sdDbg = -1;
static void sd_command(uint8_t cmd, uint32_t arg) {
    if (sdDbg < 0) sdDbg = getenv("SD_DEBUG") ? 1 : 0;
    if (sdDbg && cmd != 55)
        fprintf(stderr, "[sd] CMD%u arg=%u (%#x) @tick=%u\n", cmd, arg, arg, tickCount);
    switch (cmd) {
        case 0:  sd_out_push(sd_initialized ? 0x00 : 0x01); break; /* idle */
        case 8:  sd_out_push(0x01); sd_out_push(0x00);
                 sd_out_push(0x00); sd_out_push(0x01);
                 sd_out_push(0xaa); break;
        case 55: sd_out_push(0x01); break;
        case 41: sd_initialized = 1; sd_out_push(0x00); break;
        case 58: sd_out_push(0x00); sd_out_push(0xc0);
                 sd_out_push(0x00); sd_out_push(0x00);
                 sd_out_push(0x00); break;
        case 16: sd_out_push(0x00); break;
        case 17: { /* lecture d'un secteur */
            size_t base = (size_t)arg * 512;
            uint8_t *card = sd_card_data();
            if (card && base + 512 <= sd_card_size()) {
                sd_out_push(0x00);
                sd_out_push(0xfe);
                sd_out_append(card + base, 512);
                sd_out_push(0xff); sd_out_push(0xff);
            } else {
                sd_out_push(0x04);
            }
            break;
        }
        case 24: /* écriture : token 0xfe + 512 + crc2 puis réponse/busy */
            sd_writeLba = arg;
            sd_writeIdx = 0;
            sd_writing = 1;
            sd_out_push(0x00);
            break;
        default: sd_out_push(0x04); break;
    }
}

static void sd_write_persist(uint32_t lba, const uint8_t *data);

static void sd_process(uint8_t v) {
    if (sd_writing) {
        if (sd_writeIdx == 0 && v == 0xff) return; /* dummy avant le token */
        sd_writeBuf[sd_writeIdx++] = v;
        if (sd_writeIdx >= 515) {
            sd_writing = 0;
            uint8_t *card = sd_card_data();
            if (card && sd_writeBuf[0] == 0xfe) {
                memcpy(card + (size_t)sd_writeLba * 512, sd_writeBuf + 1, 512);
                sd_write_persist(sd_writeLba, card + (size_t)sd_writeLba * 512);
            }
            /* accepté, puis busy (comme sdcard.ts) */
            sd_out_push(0x05); sd_out_push(0x00);
            sd_out_push(0x00); sd_out_push(0xff);
        }
        return;
    }
    if (sd_cmdIdx > 0) {
        sd_cmdBuf[sd_cmdIdx++] = v;
        if (sd_cmdIdx >= 6) {
            sd_cmdIdx = 0;
            uint32_t arg = ((uint32_t)sd_cmdBuf[1] << 24) | ((uint32_t)sd_cmdBuf[2] << 16) |
                           ((uint32_t)sd_cmdBuf[3] << 8) | sd_cmdBuf[4];
            sd_command(sd_cmdBuf[0] & 0x3f, arg);
        }
        return;
    }
    if ((v & 0xc0) == 0x40) sd_cmdBuf[sd_cmdIdx++] = v; /* 0xff ignoré */
}

static void sd_byte(uint8_t v) {
    if (!sd_selected()) {
        sd_reset_state();
        return; /* silencieuse quand désélectionnée */
    }
    ser4_data = sd_pending;
    sd_pending = sd_outPos < sd_outLen ? sd_out[sd_outPos++] : 0xff;
    { static int sdDbg2 = -1;
      if (sdDbg2 < 0) sdDbg2 = getenv("SD_DEBUG") ? (getenv("SD_DEBUG")[0] == '2' ? 1 : 0) : 0;
      if (sdDbg2 && sd_outLen + sd_cmdIdx > 0)
          fprintf(stderr, "[sdx] t=%u w=%02x -> %02x q=%u/%u sel=%d\n",
                  tickCount, v, ser4_data, sd_outPos, sd_outLen, sd_selected()); }
    sd_process(v);
}


/* --------- carte SD construite depuis un répertoire local --------- */
#include <dirent.h>
#include <sys/stat.h>

#define FAT_SECTOR 512
#define FAT_SPC 8
#define FAT_TOTAL 131072u   /* 64 Mio -> FAT16 */
#define FAT_ROOT  512

typedef struct { char name83[12]; int isDir; uint32_t first, size; char path[1024]; } FatEnt;

static void to83(const char *name, char used[][12], int nUsed, char out[12]) {
    char up[1024];
    snprintf(up, sizeof(up), "%s", name);
    for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
    char base[9] = {0}, ext[4] = {0};
    const char *dot = strrchr(up, '.');
    if (dot && dot != up) { strncpy(ext, dot + 1, 3); strncpy(base, up, dot - up > 8 ? 8 : dot - up); }
    else strncpy(base, up, 8);
    /* nettoie les caractères interdits */
    for (char *q = base; *q; q++) if (!isalnum((unsigned char)*q) && !strchr("$%'()-@^_`{}~!#", *q)) *q = '_';
    for (char *q = ext; *q; q++) if (!isalnum((unsigned char)*q) && !strchr("$%'()-@^_`{}~!#", *q)) *q = '_';
    char comb[16];
    snprintf(comb, sizeof(comb), "%s%s%s", base, ext[0] ? "." : "", ext);
    char final[16];
    snprintf(final, sizeof(final), "%s", comb);
    for (int i = 1; i < 100; i++) {
        int dup = 0;
        for (int j = 0; j < nUsed; j++) if (!strcmp(used[j], final)) dup = 1;
        if (!dup) break;
        snprintf(final, sizeof(final), "%.6s~%d%s%s", base, i, ext[0] ? "." : "", ext);
    }
    snprintf(used[nUsed < 1000 ? nUsed : 999], 12, "%s", final);
    memset(out, ' ', 11);
    /* le champ 8.3 ne contient jamais de point : base et extension sont
     * copiées séparément (final les réunit, mais tronqué à 8 caractères il
     * laissait le point dans le nom — « ICON.BMP » devenait ICON.BMP.BMP,
     * entrée invalide qui faisait rejeter toute la carte par le firmware) */
    memcpy(out, base, strlen(base) > 8 ? 8 : strlen(base));
    if (ext[0]) memcpy(out + 8, ext, strlen(ext) > 3 ? 3 : strlen(ext));
}

static int fatTotalSectors;
static uint8_t *fatImage;
static size_t   fatImageSize;
static uint32_t fatClusterCount; /* clusters de données du volume */
static uint32_t fatSpc = 8;         /* secteurs / cluster (dynamique) */
static uint32_t fatFatsz = 65;      /* secteurs / FAT (dynamique) */
/* Table de partitions, par cible : les cartes réelles ont toujours un MBR
 * et la lib META officielle (SdFat, partition 1) ne monte pas une
 * superfloppy — elle lit le secteur 0, ne trouve pas d'entrée à 0x1BE et
 * boucle sur son écran de chargement.  Le loader gbrecomp (meta_fat.c)
 * gère les deux dispositions ; en revanche le lecteur FAT Pokitto attend
 * le secteur de boot en LBA 0 (superfloppy).  fatPartStart est donc posé
 * au moment de la construction de la carte (fat_bootstrap_for), la cible
 * étant déjà détectée à ce moment. */
static uint32_t fatPartStart; /* décalage du volume (secteurs) */
typedef struct { uint32_t lba; size_t bytes; char path[1024]; uint8_t *mem; } FatFile;
static FatFile  fatFiles[512];
static int      fatFileCount;

/* liste de fichiers virtuels (navigateur : drop/dossier -> données en
 * mémoire) ; le natif garde opendir via fat_build_from_dir */
typedef struct { char path[1024]; uint8_t *data; size_t size; } VFile;
static VFile vfiles[512];
static int   nvfiles;

static void vfiles_reset(void) {
    for (int i = 0; i < nvfiles; i++) free(vfiles[i].data);
    nvfiles = 0;
}

static void vfiles_add(const char *path, const uint8_t *data, size_t len) {
    if (nvfiles >= 512) return;
    VFile *v = &vfiles[nvfiles++];
    snprintf(v->path, sizeof(v->path), "%s", path);
    v->data = malloc(len ? len : 1);
    memcpy(v->data, data, len);
    v->size = len;
}

/* nombre de dossiers distincts hébergeant les vfiles (pour dimensionner
 * la carte : chaque dossier = 1 cluster d'entrées) */
static int vfile_dir_count(void) {
    static char dirs[600][512];
    int n = 0;
    for (int i = 0; i < nvfiles; i++) {
        const char *p = vfiles[i].path;
        const char *sl = strrchr(p, '/');
        if (!sl) continue; /* racine : zone fixe */
        int dl = (int)(sl - p);
        int seen = 0;
        for (int j = 0; j < n; j++)
            if ((int)strlen(dirs[j]) == dl && strncmp(dirs[j], p, dl) == 0) { seen = 1; break; }
        if (!seen && n < 600) { memcpy(dirs[n], p, dl); dirs[n][dl] = 0; n++; }
    }
    return n;
}

static int fat_used_find(char used[][12], int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(used[i], s)) return 1;
    return 0;
}

/* alloue et écrit récursivement ; renvoie le premier cluster */
static uint32_t fatNext = 2;
static uint8_t *fatTable;

static uint32_t fat_alloc(int clusters) {
    uint32_t first = fatNext;
    /* garde : ne jamais écrire au-delà de la table allouée — une carte
     * pleine termine la chaîne au lieu de corrompre le tas (le volume
     * était dimensionné au forfait, un dossier source plus grand
     * débordait) */
    uint32_t cap = fatFatsz * (FAT_SECTOR / 2);
    uint32_t written = 0;
    for (int i = 0; i < clusters; i++) {
        uint32_t c = first + (uint32_t)i;
        if (c + 1 >= cap) {
            fprintf(stderr, "carte SD : pleine, contenu tronqué\n");
            break;
        }
        uint16_t v = (i == clusters - 1) ? 0xffff : (uint16_t)(c + 1);
        fatTable[c * 2] = v & 0xff; fatTable[c * 2 + 1] = v >> 8;
        written = i + 1;
    }
    if (written) fatNext = first + written;
    return first;
}

static uint32_t fat_data_lba(uint32_t first) {
    return fatPartStart + 1 + 2 * fatFatsz + (FAT_ROOT * 32 + 511) / 512 + (first - 2) * fatSpc;
}

static void fat_place(const char *dir, uint32_t parentFirst, int isRoot,
                      FatEnt *entries, int nEntries, uint32_t selfFirst);

static uint32_t fat_alloc_dir_data(void) { return fat_alloc(1); }

static void fat_write_dir_data(uint32_t first, FatEnt *entries, int n, uint32_t selfFirst, uint32_t parentFirst, int isRoot) {
    uint8_t buf[64 * FAT_SECTOR];
    uint32_t csz = fatSpc * FAT_SECTOR;
    if (csz > sizeof(buf)) csz = sizeof(buf);
    memset(buf, 0, csz);
    uint32_t lba = fat_data_lba(first);
    if ((size_t)lba * FAT_SECTOR + csz > fatImageSize) return; /* hors carte */
    if (!isRoot) {
        memcpy(buf, ".          ", 11); buf[11] = 0x10;
        buf[26] = selfFirst & 0xff; buf[27] = selfFirst >> 8;
        memcpy(buf + 32, "..         ", 11); buf[32 + 11] = 0x10;
        buf[32 + 26] = parentFirst & 0xff; buf[32 + 27] = parentFirst >> 8;
    }
    int off = isRoot ? 0 : 64;
    for (int i = 0; i < n && off < (int)sizeof(buf) - 32; i++) {
        memcpy(buf + off, entries[i].name83, 11);
        buf[off + 11] = entries[i].isDir ? 0x10 : 0x20;
        buf[off + 26] = entries[i].first & 0xff;
        buf[off + 27] = entries[i].first >> 8;
        buf[off + 28] = entries[i].size & 0xff; buf[off + 29] = (entries[i].size >> 8) & 0xff;
        buf[off + 30] = (entries[i].size >> 16) & 0xff; buf[off + 31] = (entries[i].size >> 24) & 0xff;
        off += 32;
    }
    memcpy(fatImage + lba * FAT_SECTOR, buf, csz);
}

/* place le contenu du répertoire ; renvoie les entrées allouées */
static void fat_walk(const char *dir, uint32_t parentFirst, int isRoot,
                     FatEnt **outEntries, int *outN) {
    DIR *d = opendir(dir);
    if (!d) { *outEntries = NULL; *outN = 0; return; }
    struct dirent *e;
    char used[256][12]; int nUsed = 0;
    FatEnt *ents = calloc(256, sizeof(FatEnt));
    int n = 0;
    while ((e = readdir(d)) && n < 200) {
        if (e->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        to83(e->d_name, used, nUsed++, ents[n].name83);
        ents[n].isDir = S_ISDIR(st.st_mode);
        ents[n].size = (uint32_t)st.st_size;
        snprintf(ents[n].path, sizeof(ents[n].path), "%s", full);
        n++;
    }
    closedir(d);
    /* alloue dossiers puis fichiers ; écrit les données */
    for (int i = 0; i < n; i++) {
        if (ents[i].isDir) {
            ents[i].first = fat_alloc_dir_data();
        } else {
            int clusters = (int)((ents[i].size + fatSpc * FAT_SECTOR - 1) / (fatSpc * FAT_SECTOR));
            if (clusters == 0) clusters = 1;
            ents[i].first = fat_alloc(clusters);
            uint32_t flba = fat_data_lba(ents[i].first);
            size_t fwant = (size_t)clusters * fatSpc * FAT_SECTOR;
            if (flba * FAT_SECTOR + fwant > fatImageSize) {
                /* le fichier ne tient pas sur la carte : sauté (la garde
                 * de fat_alloc borne la table, celle-ci borne les données) */
                fprintf(stderr, "carte SD : %s hors capacité, ignoré\n", ents[i].path);
                continue;
            }
            FILE *g = fopen(ents[i].path, "rb");
            if (g) {
                uint8_t *data = malloc(fwant);
                memset(data, 0, fwant);
                size_t got = fread(data, 1, ents[i].size, g);
                (void)got;
                memcpy(fatImage + flba * FAT_SECTOR, data, fwant);
                free(data);
                fclose(g);
                if (fatFileCount < 512) {
                    fatFiles[fatFileCount].lba = fat_data_lba(ents[i].first);
                    fatFiles[fatFileCount].bytes = ents[i].size;
                    snprintf(fatFiles[fatFileCount].path, sizeof(fatFiles[fatFileCount].path), "%s", ents[i].path);
                    fatFileCount++;
                }
            }
        }
    }
    /* place les sous-dossiers (récursif) */
    for (int i = 0; i < n; i++) {
        if (ents[i].isDir) {
            FatEnt *sub = NULL; int nSub = 0;
            fat_walk(ents[i].path, ents[i].first, 0, &sub, &nSub);
            fat_write_dir_data(ents[i].first, sub, nSub, ents[i].first, isRoot ? 0 : parentFirst, isRoot);
            free(sub);
        }
    }
    *outEntries = ents; *outN = n;
}

/* amorce du volume : géométrie calculée pour contenir `bytes` de données
 * (FAT16, clusters 4-32 Ko, volume borné à ~511 Mo) */
static void fat_bootstrap_for(size_t bytes, int ndirs) {
    fatPartStart = (emuTarget == TGT_POKITTO) ? 0 : 2048; /* 1 Mio, alignement SD */
    /* FAT16 impose 4085..65000 clusters : choisit le spc en conséquence */
    uint32_t spc = 8; /* 4 Ko */
    double cl;
    for (;;) {
        cl = (double)bytes / (spc * FAT_SECTOR) + 8 + ndirs;
        if (cl > 65000.0 && spc < 64) { spc *= 2; continue; }
        if (cl < 4085.0 && spc > 1) { spc /= 2; continue; }
        break;
    }
    uint32_t clusters = (uint32_t)cl + 1;
    if (clusters < 4085u) clusters = 4085u; /* plancher FAT16 */
    if (clusters > 65000u) clusters = 65000u;
    fatClusterCount = clusters;
    uint32_t fatsz = (uint32_t)(((clusters + 2) * 2 + FAT_SECTOR - 1) / FAT_SECTOR);
    uint32_t vol = 1 + 2 * fatsz + 32 + clusters * spc; /* secteurs du volume */
    uint32_t total = fatPartStart + vol;                /* + table de partitions */
    fatSpc = spc;
    fatFatsz = fatsz;
    fatTotalSectors = (int)total;
    fatImageSize = (size_t)total * FAT_SECTOR;
    fatImage = calloc(1, fatImageSize);
    fatTable = calloc((size_t)fatsz * FAT_SECTOR, 1);
    if (!fatImage || !fatTable) { /* carte hors ressources : pas de carte */
        fprintf(stderr, "carte SD : image trop grande (%zu Mio) — abandon\n",
                fatImageSize / (1024 * 1024));
        free(fatImage); fatImage = NULL; fatImageSize = 0;
        free(fatTable); fatTable = NULL;
        return;
    }
    fatTable[0] = 0xf8; fatTable[1] = 0xff;
    fatTable[2] = 0xff; fatTable[3] = 0xff;
    /* --- secteur de boot de la partition (le driver du firmware ne lit
     * que le champ 16 bits du total, fat_finish recopie les FAT) --- */
    uint8_t *bs = fatImage + fatPartStart * FAT_SECTOR;
    bs[0] = 0xeb; bs[1] = 0x3c; bs[2] = 0x90;
    memcpy(bs + 3, "GBREMU", 6);
    bs[0x0b] = 0x00; bs[0x0c] = 0x02; /* 512 octets/secteur */
    bs[0x0d] = (uint8_t)spc;
    bs[0x0e] = 0x01; bs[0x0f] = 0x00;
    bs[0x10] = 0x02;
    bs[0x11] = FAT_ROOT & 0xff; bs[0x12] = FAT_ROOT >> 8;
    bs[0x15] = 0xf8;
    bs[0x16] = fatsz & 0xff; bs[0x17] = (fatsz >> 8) & 0xff;
    bs[0x18] = 32; bs[0x19] = 0;   /* secteurs/piste */
    bs[0x1a] = 8; bs[0x1b] = 0;    /* têtes */
    /* FAT16 : total du VOLUME en 16 bits (0x13) sous 65536 secteurs,
     * sinon en 32 (0x20) */
    if (vol < 65536u) {
        bs[0x13] = vol & 0xff; bs[0x14] = (vol >> 8) & 0xff;
    } else {
        bs[0x20] = vol & 0xff; bs[0x21] = (vol >> 8) & 0xff;
        bs[0x22] = (vol >> 16) & 0xff; bs[0x23] = (vol >> 24) & 0xff;
    }
    bs[0x1c] = fatPartStart & 0xff; bs[0x1d] = (fatPartStart >> 8) & 0xff; /* secteurs cachés */
    bs[0x1e] = (fatPartStart >> 16) & 0xff; bs[0x1f] = (fatPartStart >> 24) & 0xff;
    bs[510] = 0x55; bs[511] = 0xaa;
    /* --- MBR : une partition FAT16 occupant tout le reste --- */
    fatImage[0x1be] = 0x00; /* non amorçable */
    {   /* CHS calculés avec la géométrie du BPB (8 têtes, 32 secteurs/piste) ;
         * les pilotes utilisent le champ LBA — le Codé CHS ne sert qu'aux
         * outils de disque.  Au-delà de 1023 cylindres : forme saturée. */
        uint32_t end = total - 1;
        uint32_t cs = fatPartStart / 256, ce = end / 256;
        fatImage[0x1bf] = (uint8_t)((fatPartStart / 32) % 8);
        fatImage[0x1c0] = (uint8_t)((fatPartStart % 32 + 1) | ((cs >> 2) & 0xc0));
        fatImage[0x1c1] = (uint8_t)cs;
        fatImage[0x1c2] = 0x06; /* type FAT16 */
        fatImage[0x1c3] = ce > 1023 ? 0xfe : (uint8_t)((end / 32) % 8);
        fatImage[0x1c4] = ce > 1023 ? 0xff : (uint8_t)((end % 32 + 1) | ((ce >> 2) & 0xc0));
        fatImage[0x1c5] = ce > 1023 ? 0xff : (uint8_t)ce;
        fatImage[0x1c6] = fatPartStart & 0xff; fatImage[0x1c7] = (fatPartStart >> 8) & 0xff;
        fatImage[0x1c8] = (fatPartStart >> 16) & 0xff; fatImage[0x1c9] = (fatPartStart >> 24) & 0xff;
        fatImage[0x1ca] = vol & 0xff; fatImage[0x1cb] = (vol >> 8) & 0xff;
        fatImage[0x1cc] = (vol >> 16) & 0xff; fatImage[0x1cd] = (vol >> 24) & 0xff;
        fatImage[510] = 0x55; fatImage[511] = 0xaa;
    }
}

static FatEnt *rootEnts; /* entrées de la racine, consommées par fat_finish */
static int rootN;

/* écrit la racine puis recopie les 2 FAT (la table a été remplie par les
 * allocations) */
static void fat_finish(const char *label) {
    uint32_t fatsz = fatFatsz; /* dynamique (l'ancien 65 figé écrivait la
                                * racine au mauvais endroit et lisait
                                * hors-tampon pour les cartes réduites) */
    uint8_t *rootBuf = calloc(FAT_ROOT * 32, 1);
    int off = 0;
    for (int i = 0; i < rootN; i++) {
        memcpy(rootBuf + off, rootEnts[i].name83, 11);
        rootBuf[off + 11] = rootEnts[i].isDir ? 0x10 : 0x20;
        rootBuf[off + 26] = rootEnts[i].first & 0xff;
        rootBuf[off + 27] = rootEnts[i].first >> 8;
        rootBuf[off + 28] = rootEnts[i].size & 0xff; rootBuf[off + 29] = (rootEnts[i].size >> 8) & 0xff;
        rootBuf[off + 30] = (rootEnts[i].size >> 16) & 0xff; rootBuf[off + 31] = (rootEnts[i].size >> 24) & 0xff;
        off += 32;
    }
    uint32_t rootLba = fatPartStart + 1 + 2 * fatsz;
    memcpy(fatImage + rootLba * FAT_SECTOR, rootBuf, FAT_ROOT * 32);
    memcpy(fatImage + (fatPartStart + 1) * FAT_SECTOR, fatTable, fatsz * FAT_SECTOR);
    memcpy(fatImage + (fatPartStart + 1 + fatsz) * FAT_SECTOR, fatTable, fatsz * FAT_SECTOR);
    free(rootBuf); free(rootEnts); rootEnts = NULL; rootN = 0;
    if (getenv("FAT_DUMP")) {
        FILE *g = fopen(getenv("FAT_DUMP"), "wb");
        if (g) { fwrite(fatImage, 1, fatImageSize, g); fclose(g);
                 printf("image FAT test : %s\n", getenv("FAT_DUMP")); }
    }
    printf("carte SD : %s (%d fichiers)\n", label, fatFileCount);
}

/* taille réelle du contenu (fichiers + marge par fichier) */
static size_t fat_probe_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t total = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 8) total += fat_probe_dir(full, depth + 1);
        } else {
            total += (size_t)st.st_size + 4096;
        }
    }
    closedir(d);
    return total;
}

static void fat_build_from_dir(const char *dir) {
    /* géométrie historique (64 Mo) : la parité META est validée sur cette
     * base ; les dossiers plus grands sont bornés par la garde de
     * fat_alloc (la sonde fat_probe_dir reste disponible si besoin) */
    (void)fat_probe_dir;
    fat_bootstrap_for(64u * 1024 * 1024, 16);
    rootEnts = NULL; rootN = 0;
    fat_walk(dir, 0, 1, &rootEnts, &rootN);
    fat_finish(dir);
}

/* ---- construction depuis les fichiers virtuels (navigateur) ---- */

static void fat_walk_vfiles(const char *prefix, uint32_t parentFirst, int isRoot,
                            FatEnt **outEntries, int *outN) {
    int prefixLen = (int)strlen(prefix);
    char used[256][12]; int nUsed = 0;
    FatEnt *ents = calloc(256, sizeof(FatEnt));
    int n = 0;
    /* premier niveau : fichiers du préfixe sans '/', puis dossiers */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nvfiles && n < 200; i++) {
            const char *p = vfiles[i].path;
            if (strncmp(p, prefix, prefixLen) != 0) continue;
            const char *rest = p + prefixLen;
            if (!*rest) continue;
            const char *slash = strchr(rest, '/');
            int isDir = slash != NULL;
            if ((pass == 0) != (!isDir)) continue;
            int nameLen = isDir ? (int)(slash - rest) : (int)strlen(rest);
            char name[1024];
            memcpy(name, rest, nameLen); name[nameLen] = 0;
            /* doublon déjà vu (plusieurs fichiers dans le même dossier) */
            int seen = 0;
            for (int j = 0; j < n; j++)
                if ((int)strlen(ents[j].path) == prefixLen + nameLen &&
                    strncmp(ents[j].path, p, prefixLen + nameLen) == 0) { seen = 1; break; }
            if (seen) continue;
            to83(name, used, nUsed++, ents[n].name83);
            ents[n].isDir = isDir;
            ents[n].size = isDir ? 0 : (uint32_t)vfiles[i].size;
            snprintf(ents[n].path, sizeof(ents[n].path), "%.*s", prefixLen + nameLen, p);
            n++;
        }
    }
    /* alloue dossiers puis fichiers ; écrit les données */
    for (int i = 0; i < n; i++) {
        if (ents[i].isDir) {
            ents[i].first = fat_alloc_dir_data();
        } else {
            int clusters = (int)((ents[i].size + fatSpc * FAT_SECTOR - 1) / (fatSpc * FAT_SECTOR));
            if (clusters == 0) clusters = 1;
            ents[i].first = fat_alloc(clusters);
            /* retrouve les données du vfile */
            for (int j = 0; j < nvfiles; j++) {
                if (strcmp(vfiles[j].path, ents[i].path) == 0) {
                    uint8_t *dst = fatImage + fat_data_lba(ents[i].first) * FAT_SECTOR;
                    memset(dst, 0, (size_t)clusters * fatSpc * FAT_SECTOR);
                    memcpy(dst, vfiles[j].data, vfiles[j].size);
                    if (fatFileCount < 512) {
                        fatFiles[fatFileCount].lba = fat_data_lba(ents[i].first);
                        fatFiles[fatFileCount].bytes = ents[i].size;
                        fatFiles[fatFileCount].mem = vfiles[j].data;
                        fatFiles[fatFileCount].path[0] = 0;
                        fatFileCount++;
                    }
                    break;
                }
            }
        }
    }
    /* sous-dossiers (récursif) */
    for (int i = 0; i < n; i++) {
        if (ents[i].isDir) {
            FatEnt *sub = NULL; int nSub = 0;
            char subPrefix[1024];
            snprintf(subPrefix, sizeof(subPrefix), "%s/", ents[i].path);
            fat_walk_vfiles(subPrefix, ents[i].first, 0, &sub, &nSub);
            fat_write_dir_data(ents[i].first, sub, nSub, ents[i].first, isRoot ? 0 : parentFirst, isRoot);
            free(sub);
        }
    }
    *outEntries = ents; *outN = n;
}

static void fat_build_from_vfiles(void) {
    size_t total = 0;
    for (int i = 0; i < nvfiles; i++) total += vfiles[i].size + 128;
    /* marge : arrondi au cluster de chaque fichier + dossiers + slack */
    total += (size_t)(nvfiles + vfile_dir_count() + 16) * 32768;
    if (total < 4u * 1024 * 1024) total = 4u * 1024 * 1024; /* plancher 4 Mo */
    fat_bootstrap_for(total, vfile_dir_count());
    rootEnts = NULL; rootN = 0;
    fat_walk_vfiles("", 0, 1, &rootEnts, &rootN);
    fat_finish("fichiers déposés");
}

/* écriture CMD24 : répercute dans le fichier local si le secteur appartient
 * à un fichier du répertoire */
static void sd_write_persist(uint32_t lba, const uint8_t *data) {
    for (int i = 0; i < fatFileCount; i++) {
        if (lba >= fatFiles[i].lba && (lba - fatFiles[i].lba) * 512 < fatFiles[i].bytes) {
            if (fatFiles[i].mem) {
                /* fichier virtuel (navigateur) : écrit dans le tampon ;
                 * l'export inter-sessions est géré côté JS */
                memcpy(fatFiles[i].mem + (lba - fatFiles[i].lba) * 512, data, 512);
            } else {
                FILE *g = fopen(fatFiles[i].path, "r+b");
                if (g) {
                    fseek(g, (long)((lba - fatFiles[i].lba) * 512), SEEK_SET);
                    fwrite(data, 1, 512, g); fclose(g);
                }
            }
            return;
        }
    }
}


/* ------------------------------------------------ zip = carte SD ------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t *zip_inflate_raw(const uint8_t *src, size_t csize, size_t usize) {
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -15) != Z_OK) return NULL;
    uint8_t *out = malloc(usize ? usize : 1);
    s.next_in = (const uint8_t *)src; s.avail_in = (uInt)csize;
    s.next_out = out; s.avail_out = (uInt)usize;
    int r = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (r != Z_STREAM_END || s.avail_out != 0) { free(out); return NULL; }
    return out;
}


typedef struct { char name[1024]; const uint8_t *cdata; size_t csize; size_t usize; int method; } ZipEnt;

static void reset_machine(void);
static void load_firmware_data(const uint8_t *data, size_t len, const char *display);
static int fwLoaded;
static const char *fwName;

/* parse le central directory ; renvoie le nombre d'entrées (fichiers) */
static int zip_parse(const uint8_t *data, size_t len, ZipEnt *ents, int max) {
    if (len < 22) return 0;
    long eocd = -1;
    for (long i = (long)len - 22; i >= 0; i--) {
        if (data[i] == 'P' && data[i+1] == 'K' && data[i+2] == 5 && data[i+3] == 6) { eocd = i; break; }
    }
    if (eocd < 0) return 0;
    int nents = rd16(data + eocd + 10);
    uint32_t cd = rd32(data + eocd + 16);
    size_t p = cd;
    int n = 0;
    for (int k = 0; k < nents && n < max; k++) {
        if (p + 46 > len || rd32(data + p) != 0x02014b50u) break;
        int method = rd16(data + p + 10);
        size_t csize = rd32(data + p + 20);
        size_t usize = rd32(data + p + 24);
        int nlen = rd16(data + p + 28), elen = rd16(data + p + 30), clen = rd16(data + p + 32);
        uint32_t lho = rd32(data + p + 42);
        int isdir = (data[p + p ? 0 : 0] == 0); /* sans objet : test réel plus bas */
        (void)isdir;
        if (p + 46 + nlen + elen + clen > len) break;
        const uint8_t *nm = data + p + 46;
        /* entrée répertoire : nom fini par '/' */
        if (nlen > 0 && nm[nlen - 1] == '/') { p += 46 + nlen + elen + clen; continue; }
        if (method != 0 && method != 8) { p += 46 + nlen + elen + clen; continue; }
        /* local header : nom+extra après les 30 octets */
        if (lho + 30 > len || rd32(data + lho) != 0x04034b50u) { p += 46 + nlen + elen + clen; break; }
        int l_nlen = rd16(data + lho + 26), l_elen = rd16(data + lho + 28);
        size_t doff = lho + 30 + l_nlen + l_elen;
        if (doff + csize > len) { p += 46 + nlen + elen + clen; break; }
        ZipEnt *e = &ents[n++];
        size_t nl = nlen < 1023 ? nlen : 1023;
        memcpy(e->name, nm, nl); e->name[nl] = 0;
        e->cdata = data + doff; e->csize = csize; e->usize = usize; e->method = method;
        p += 46 + nlen + elen + clen;
    }
    return n;
}

/* décompresse une entrée dans un tampon neuf (libéré par l'appelant) */
static uint8_t *zip_entry_data(const ZipEnt *e) {
    if (e->method == 0) {
        uint8_t *out = malloc(e->usize ? e->usize : 1);
        memcpy(out, e->cdata, e->usize);
        return out;
    }
    return zip_inflate_raw(e->cdata, e->csize, e->usize);
}

/* le contenu du zip = carte SD complète ; s'il contient un (unique) .bin à
 * la racine ou dans un sous-dossier, il devient le firmware (le loader
 * listera tous les jeux de la carte, comme sur la vraie console) */
static int zip_load_card(const uint8_t *data, size_t len) {
    ZipEnt ents[256];
    int n = zip_parse(data, len, ents, 256);
    if (n == 0) return 0;
    /* décompresse TOUT d'abord : les pointeurs cdata pointent dans `data`,
     * que reset_machine va libérer (sd_unload) */
    uint8_t *datas[256];
    int fwb = -1;
    for (int i = 0; i < n; i++) {
        datas[i] = zip_entry_data(&ents[i]);
        if (!datas[i]) { /* entrée illisible : retirée de la liste */
            memmove(ents + i, ents + i + 1, sizeof(ZipEnt) * (size_t)(n - i - 1));
            n--; i--; continue;
        }
        size_t l = strlen(ents[i].name);
        if (fwb < 0 && l > 4 && strcasecmp(ents[i].name + l - 4, ".bin") == 0) fwb = i;
    }
    reset_machine();
    if (fwb >= 0) {
        char base[1024];
        const char *slash = strrchr(ents[fwb].name, '/');
        snprintf(base, sizeof(base), "%s", slash ? slash + 1 : ents[fwb].name);
        load_firmware_data(datas[fwb], ents[fwb].usize, base);
    }
    for (int i = 0; i < n; i++) {
        vfiles_add(ents[i].name, datas[i], ents[i].usize);
        free(datas[i]);
    }
    if (nvfiles > 0) fat_build_from_vfiles();
    printf("carte SD : zip (%d fichiers)", n);
    if (fwLoaded) printf(" + firmware %s", fwName);
    printf("\n");
    return fwLoaded ? 2 : 1;
}

/* ===================================================== POKITTO =========
 * LPC11U68 (Cortex-M0) — port C fidèle du PokittoEmu de felipemanga.
 * Même cœur d'exécution que la META ; l'environnement change :
 *  - flash chargée à 0 (vecteurs), SRAM 0x10000000 (32 Kio) + 2 x 2 Kio
 *    (0x20000000, 0x20004000), EEPROM 4 Ko persistée (<jeu>.eeprom) ;
 *  - périphériques LPC : SYSCON, IOCON, CT32B0/1, SysTick, SCT0/1, SSP0/1,
 *    ADC, RTC, USART0, GPIO (4 banques) ;
 *  - écran 220x176 piloté en bit-bang GPIO (front montant POUT1[12]) ;
 *  - audio R2R 8 bits via GPIO (POUT1[31:28] | POUT2[23:20]) + audio HLE
 *    (détection par somme de contrôle du handler CT32B0 du firmware stock) ;
 *  - carte SD SPI sur SSP0 (sélection POUT0[7]) ;
 *  - API ROM à 0x30000000 : division et IAP via BLX 0x1fff1ffx.
 * ====================================================================== */

/* boutons : bit0 bas, 1 gauche, 2 droite, 3 haut, 4 A, 5 B, 6 MENU, 7 HOME
 * (actifs bas : 0 = enfoncé).  Ordre du registre à décalage physique du
 * META (MSB en premier : home..down), confirmé par le source du registre
 * gdl de lapinou : left,right,up,a,b,menu,down,home — même masque pour
 * les deux cibles. */
#define BTN_LEFT   (1u << 0)
#define BTN_RIGHT  (1u << 1)
#define BTN_UP     (1u << 2)
#define BTN_A      (1u << 3)
#define BTN_B      (1u << 4)
#define BTN_MENU   (1u << 5)
#define BTN_DOWN   (1u << 6)
#define BTN_HOME   (1u << 7)
#define BTN_DIRMASK (BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_UP)

static SDL_GameController *pad; /* définitions complètes dans la section SDL */
static SDL_Joystick *joyFb;
static char fwPath[1024];
static char outImgPath[512];
static uint32_t emu_nextFrameTick;
static uint32_t pk_ignoreBadWrites; /* -w/-W : ignorer les écritures flash */

/* PRNG déterministe (SRAM « poubelle » à l'allumage, ADC DAT1) */
static uint32_t pk_prngState = 0x2545F491u;
static uint32_t pk_prng(void) {
    pk_prngState ^= pk_prngState << 13;
    pk_prngState ^= pk_prngState >> 17;
    pk_prngState ^= pk_prngState << 5;
    return pk_prngState;
}

static uint8_t pk_sram1[0x800], pk_usbsram[0x800], pk_eeprom[0x1000];
static int pk_eepromDirty;

/* --- SYSCON (0x40048000) : stockage générique + PINTSEL pour les boutons */
static uint32_t pk_syscon[256];
#define PK_SYSCON_SYSPLLCTRL   2u
#define PK_SYSCON_SYSAHBCLKCTRL 32u
#define PK_SYSCON_PINTSEL(n)   (94u + (n))
static uint32_t sys_VTOR, sys_AIRCR;
static uint32_t pk_iocon[88];

/* --- CT32B0/1 + SysTick */
struct pk_ct { uint32_t r[14]; }; /* IR TCR TC PR PC MCR MR0..3 CCR CR0..2 */
static struct pk_ct pk_ct[2];
#define PK_CT_IR 0
#define PK_CT_TCR 1
#define PK_CT_TC 2
#define PK_CT_PR 3
#define PK_CT_PC 4
static uint32_t pk_lastTick;
static uint32_t pk_systickCSR, pk_systickRVR, pk_systickCVR;

/* --- SCT0/1 : stockage pur (comme la référence) */
static uint32_t pk_sct[2][324];

/* --- SSP0/1 : file d'entrée LIFO + auditeurs (SSP0 = carte SD) */
struct pk_spi {
    uint32_t cr0, cr1, dr, sr, cpsr, imsc, ris, mis, icr, dmacr;
    int dataSize, frameSize, clearable;
    uint8_t inBuf[8];
    int inLen;
};
static struct pk_spi pk_spi0, pk_spi1;

static void pk_sd_write(uint32_t b32); /* carte SD, défini plus bas */

static void pk_spi_in(struct pk_spi *s, uint32_t v, int clear) {
    if (s->clearable && clear) s->inLen = 0;
    if (s->inLen < (int)sizeof(s->inBuf)) {
        memmove(s->inBuf + 1, s->inBuf, (size_t)s->inLen);
        s->inBuf[0] = (uint8_t)v;
        s->inLen++;
    }
    s->sr = 3 | (s->inLen < s->frameSize ? 0 : 4);
}

static void pk_spi_out_byte(struct pk_spi *s, uint8_t b) {
    if (s == &pk_spi0) pk_sd_write(b); /* unique auditeur : carte SD */
}

static uint32_t pk_spi_write_dr(struct pk_spi *s, uint32_t v) {
    s->clearable = 1;
    if (s->dataSize == 0xF) { /* trames 16 bits : octet bas puis haut */
        pk_spi_out_byte(s, (uint8_t)(v & 0xFF));
        v >>= 8;
        s->clearable = 0;
    }
    pk_spi_out_byte(s, (uint8_t)(v & 0xFF));
    s->sr = 3 | (s->inLen < s->frameSize ? 0 : 4);
    return v;
}

static uint32_t pk_spi_read_dr(struct pk_spi *s) {
    uint32_t v = s->dr;
    if (s->dataSize == 0xF && s->inLen > 1) {
        v = s->inBuf[s->inLen - 1]; s->inLen--;
        v <<= 8;
        v |= s->inBuf[s->inLen - 1]; s->inLen--;
    } else if (s->inLen) {
        v = s->inBuf[s->inLen - 1]; s->inLen--;
    }
    s->sr = 3 | (s->inLen < s->frameSize ? 0 : 4);
    return v;
}

/* --- ADC (0x4001C000) */
static uint32_t pk_adc[22];
static uint32_t pk_adc_dat8 = 0x8000, pk_adc_dat9 = 0x8000;

/* --- RTC (0x40024000) */
static uint32_t pk_rtc[4];
static int pk_rtcEnabled;

/* --- USART0 (0x40008000) */
static uint32_t pk_usart[19];

/* --- carte SD SPI (SSP0), machine d'états de la référence */
static int pk_sd_enabled = 1;
static int pk_sd_checkCRC = 1;
static uint32_t pk_sd_writeAddress = ~0u, pk_sd_writeCount, pk_sd_nextReadAddress;
static uint32_t pk_sd_command, pk_sd_altCommand, pk_sd_argument, pk_sd_state;
static int pk_sd_idle = 1;
static uint32_t pk_sd_resetCounter;
static uint8_t pk_sd_response[600];
static int pk_sd_respLen;
static int pk_sd_dirty; /* export via --out-img */
static uint16_t pk_sd_crc[256];

static uint8_t *pk_sd_card(void) { return sd_card_data(); }
static size_t pk_sd_card_size(void) { return sd_card_size(); }

static void pk_sd_resp(const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) {
        if (pk_sd_respLen < (int)sizeof(pk_sd_response)) pk_sd_response[pk_sd_respLen++] = b[i];
    }
}
static void pk_sd_resp_pop(void) {
    if (pk_sd_respLen) {
        pk_sd_respLen--;
        memmove(pk_sd_response, pk_sd_response + 1, (size_t)pk_sd_respLen);
    }
}

static void pk_sd_read_sector(uint32_t lba) {
    uint8_t r[2 + 512 + 2]; /* token + données + CRC16 */
    uint8_t *card = pk_sd_card();
    size_t base = (size_t)lba * 512;
    uint16_t crc = 0;
    r[0] = 0;
    r[1] = 0xFE; /* token de données */
    for (int i = 0; i < 512; i++) {
        uint8_t b = (card && base + i < pk_sd_card_size()) ? card[base + i] : 0xFF;
        r[2 + i] = b;
        crc = (uint16_t)((crc << 8) ^ pk_sd_crc[((crc >> 8) ^ b) & 0xFF]);
    }
    r[2 + 512] = (uint8_t)(crc >> 8);
    r[2 + 513] = (uint8_t)crc;
    pk_sd_resp(r, 2 + 512 + 2);
}

static void pk_sd_exec(uint32_t cmd, uint32_t arg) {
    pk_sd_respLen = 0;
    switch (cmd) {
        case 0: pk_sd_idle = 1; { uint8_t r = 1; pk_sd_resp(&r, 1); } break;
        case 8: { uint8_t r[5] = { (uint8_t)(pk_sd_idle ? 1 : 0), 0, 0, 0x01, 0xAA };
                  pk_sd_resp(r, 5); } break;
        case 12:
            if (pk_sd_state == 16) pk_sd_state = 0;
            { uint8_t r[2] = { 0, 0 }; pk_sd_resp(r, 2); }
            break;
        case 13: { uint8_t r[2] = { 0, 0 }; pk_sd_resp(r, 2); } break;
        case 16: { uint8_t r = (arg == 512) ? (uint8_t)(pk_sd_idle ? 1 : 0) : (1 << 6);
                   pk_sd_resp(&r, 1); } break;
        case 17: pk_sd_read_sector(arg); break;
        case 18:
            pk_sd_nextReadAddress = arg + 1;
            pk_sd_state = 16;
            pk_sd_read_sector(arg);
            break;
        case 24:
            { uint8_t r = 0; pk_sd_resp(&r, 1); }
            pk_sd_writeAddress = arg * 512;
            pk_sd_state = 1;
            break;
        case 55: pk_sd_altCommand = 1;
                 { uint8_t r = (uint8_t)(pk_sd_idle ? 1 : 0); pk_sd_resp(&r, 1); } break;
        case 58: { uint8_t r[5] = { (uint8_t)(pk_sd_idle ? 1 : 0), 0x40, 0x10, 0, 0 };
                   pk_sd_resp(r, 5); } break;
        case 59: pk_sd_checkCRC = arg != 0;
                 { uint8_t r = (uint8_t)(pk_sd_idle ? 1 : 0); pk_sd_resp(&r, 1); } break;
        case 61: case 169: /* ACMD41 (41 + marqueur alt 0x80) : quitte l'idle
                  * (la référence rendait {4}, ce qui
                  * empêchait toute init de carte réelle — ici la carte
                  * monte quand une image/un dossier est fourni) */
            pk_sd_idle = 0;
            { uint8_t r = 0; pk_sd_resp(&r, 1); } break;
        case 62: { uint8_t r = 0; pk_sd_resp(&r, 1); } break;
        default:
            fprintf(stderr, "SD : commande inconnue %u arg=%x\n", cmd, arg);
            { uint8_t r = 4; pk_sd_resp(&r, 1); }
            break;
    }
}

/* répercute une écriture CMD24 dans la carte montée (FAT d'un dossier ou
 * d'un zip : même persistance que la META) */
static void pk_sd_sector_written(uint32_t lba, const uint8_t *data) {
    sd_write_persist(lba, data);
}

static void pk_sd_write(uint32_t b32) {
    uint8_t b = (uint8_t)b32;
    if (!pk_sd_enabled) {
        if (b == 0xFF) {
            pk_sd_resetCounter += 8;
            if (pk_sd_resetCounter >= 74) {
                pk_sd_state = 1;
                pk_sd_checkCRC = 1;
                pk_sd_writeAddress = ~0u;
            }
        }
        pk_sd_respLen = 0;
        pk_spi_in(&pk_spi0, 0xFF, 1);
        return;
    }
    pk_sd_resetCounter = 0;

    if (pk_sd_state == 16) { /* lecture multi-bloc */
        if ((b & 0x3F) == 12) {
            uint8_t z[9] = { 0 };
            pk_sd_respLen = 0;
            pk_sd_resp(z, 9);
            pk_sd_state = 1;
        } else if (pk_sd_respLen == 0) {
            pk_sd_exec(18, pk_sd_nextReadAddress);
        }
    }

    if (pk_sd_respLen) {
        pk_spi_in(&pk_spi0, pk_sd_response[0], 1);
        pk_sd_resp_pop();
        return;
    }

    switch (pk_sd_state) {
        case 1:
            if (b == 0xFF) break;
            if (pk_sd_writeAddress != ~0u) {
                if (b == 0xFE) { /* token d'écriture */
                    pk_sd_writeCount = 512;
                    pk_sd_state = 12;
                    pk_spi_in(&pk_spi0, 0, 1);
                    return;
                }
                if (b == 0xFD) { /* fin multi-bloc */
                    pk_sd_state = 1;
                    pk_spi_in(&pk_spi0, 0, 1);
                    { uint8_t r = 0; pk_sd_respLen = 0; pk_sd_resp(&r, 1); }
                    return;
                }
            }
            if (b > 0x7F) break;
            pk_sd_command = b & 0x3F;
            pk_sd_argument = 0;
            pk_sd_state++;
            break;
        case 2: case 3: case 4: case 5:
            pk_sd_argument = (pk_sd_argument << 8) | b;
            pk_sd_state++;
            break;
        case 6: /* CRC */
            pk_sd_state++;
            break;
        case 7: {
            pk_sd_state = 1;
            if (pk_sd_altCommand) pk_sd_command += 0x80;
            pk_sd_altCommand = 0;
            pk_sd_exec(pk_sd_command, pk_sd_argument);
            pk_spi_in(&pk_spi0, pk_sd_response[0], 1);
            pk_sd_resp_pop();
            return;
        }
        case 10: case 11: case 13: case 14:
            pk_sd_state++;
            pk_spi_in(&pk_spi0, 0, 1);
            break;
        case 12: { /* données du secteur */
            uint8_t *card = pk_sd_card();
            if (card && pk_sd_writeAddress < pk_sd_card_size())
                card[pk_sd_writeAddress] = b;
            pk_sd_writeAddress++;
            pk_sd_writeCount--;
            if (!pk_sd_writeCount) {
                pk_sd_state++;
                pk_sd_dirty = 1;
                if (card)
                    pk_sd_sector_written((pk_sd_writeAddress - 512) / 512,
                                         card + (pk_sd_writeAddress - 512));
            }
            pk_spi_in(&pk_spi0, 0, 1);
            return;
        }
        case 15:
            pk_spi_in(&pk_spi0, 0x5, 1);
            pk_sd_state = 1;
            return;
    }
    pk_spi_in(&pk_spi0, 0xFF, 1);
}

static void pk_sd_machine_reset(void) {
    pk_sd_respLen = 0;
    pk_sd_state = 0;
    pk_sd_idle = 1;
    pk_sd_altCommand = 0;
    pk_sd_resetCounter = 0;
    pk_sd_writeAddress = ~0u;
    pk_sd_checkCRC = 1;
}

/* --- écran 220x176 bit-bang GPIO (ST7775 de la référence) */
static uint16_t pk_lcd[MAX_SCREEN_W * MAX_SCREEN_H];
static int pk_lcdDirty = 1;
static uint32_t pk_colStart, pk_colEnd = 175, pk_pageStart, pk_pageEnd = 219;
static uint32_t pk_col, pk_page;
static int pk_lcdV;

static void pk_lcd_reset(void) {
    pk_colStart = 0;
    pk_colEnd = 175;
    pk_pageStart = 0;
    pk_pageEnd = 219;
    pk_col = pk_page = 0;
    pk_lcdDirty = 1;
    pk_lcdV = 0;
}

static void pk_lcd_cmd03(uint16_t d) { pk_lcdV = d & 0x08; } /* Entry Mode */
static void pk_lcd_cmd20(uint16_t d) { pk_col = d - pk_colStart; }
static void pk_lcd_cmd21(uint16_t d) { pk_page = d - pk_pageStart; }
static void pk_lcd_cmd36(uint16_t d) { pk_colEnd = d; }
static void pk_lcd_cmd37(uint16_t d) { pk_colStart = d; }
static void pk_lcd_cmd38(uint16_t d) { pk_pageEnd = d; }
static void pk_lcd_cmd39(uint16_t d) { pk_pageStart = d; }
static void pk_lcd_stub(uint16_t d) { (void)d; }

static void pk_lcd_cmd22(uint16_t d) {
    int cs = (int)pk_colStart, ce = (int)pk_colEnd;
    int ps = (int)pk_pageStart, pe = (int)pk_pageEnd;
    int cd = ce - cs, pd = pe - ps;
    int x = cs + (int)pk_col, y = ps + (int)pk_page;
    if (!(x < 0 || x >= 176 || y < 0 || y >= 220)) {
        uint16_t *p = &pk_lcd[x * 220 + y];
        if (*p != d) { *p = d; pk_lcdDirty = 1; }
    }
    if (!pk_lcdV) {
        pk_col++;
        if (pk_col > (uint32_t)cd) {
            pk_col = 0;
            pk_page++;
            if (pk_page > (uint32_t)pd) pk_page = 0;
        }
    } else {
        pk_page++;
        if (pk_page > (uint32_t)pd) {
            pk_page = 0;
            pk_col++;
            if (pk_col > (uint32_t)cd) pk_col = 0;
        }
    }
}

static void (*pk_lcdCmd)(uint16_t) = pk_lcd_stub;

static void pk_lcd_write(uint32_t cd, uint16_t v) {
    if (cd) {
        pk_lcdCmd(v);
        return;
    }
    switch (v) {
        case 0x03: pk_lcdCmd = pk_lcd_cmd03; break;
        case 0x20: pk_lcdCmd = pk_lcd_cmd20; break;
        case 0x21: pk_lcdCmd = pk_lcd_cmd21; break;
        case 0x22: pk_lcdCmd = pk_lcd_cmd22; break;
        case 0x36: pk_lcdCmd = pk_lcd_cmd36; break;
        case 0x37: pk_lcdCmd = pk_lcd_cmd37; break;
        case 0x38: pk_lcdCmd = pk_lcd_cmd38; break;
        case 0x39: pk_lcdCmd = pk_lcd_cmd39; break;
        default:   pk_lcdCmd = pk_lcd_stub; break;
    }
}

/* --- audio R2R GPIO + HLE */
#define PK_AQ_SIZE (1u << 15)
#define PK_AQ_MASK (PK_AQ_SIZE - 1)
#define PK_IFREQ (1.0f / 22050.0f)
static uint8_t pk_aqData[PK_AQ_SIZE];
static float pk_aqDelta[PK_AQ_SIZE];
static uint32_t pk_aqStart, pk_aqEnd, pk_aqSize;
static float pk_audioHoldF;
static uint8_t pk_prevData = 0xFF;
static uint32_t pk_prevTicks;

enum { PK_HLE_DETECT, PK_HLE_DISABLED, PK_HLE_ENABLED };
static int pk_hleState = PK_HLE_DETECT;
static uint32_t pk_hleIrqAddress;
static uint8_t *pk_hleBuffer;
static uint32_t *pk_hlePlayhead;

static uint8_t *pk_audio_address(uint32_t v) {
    if (v < FLASH_SIZE) return flash + v;
    if (v >= 0x10000000u && v < 0x10000000u + SRAM_SIZE) return sram + v - 0x10000000u;
    if (v >= 0x20000000u && v < 0x20000000u + sizeof(pk_sram1)) return pk_sram1 + v - 0x20000000u;
    if (v >= 0x20004000u && v < 0x20004000u + sizeof(pk_usbsram)) return pk_usbsram + v - 0x20004000u;
    return NULL;
}

static void pk_audio_reopen(int freq); /* défini dans la section SDL */

static void pk_audio_check_hle(uint32_t rate) {
    uint32_t timerIRQ = pk_read_word(sys_VTOR + (34 << 2));
    if (pk_hleState != PK_HLE_DETECT) {
        if (pk_hleIrqAddress == timerIRQ) return;
    }
    pk_hleState = PK_HLE_DISABLED;
    pk_hleIrqAddress = timerIRQ;
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) sum += pk_read_word(timerIRQ + ((uint32_t)i << 2));
    if (sum != 0x32a90803u) return;
    uint32_t vbuffer = pk_read_word(timerIRQ + 0x70 - 1);
    uint32_t vplay = pk_read_word(timerIRQ + 0x6c - 1);
    pk_hleBuffer = pk_audio_address(vbuffer);
    pk_hlePlayhead = (uint32_t *)pk_audio_address(vplay);
    if (!pk_hleBuffer || !pk_hlePlayhead) return;
    pk_hleState = PK_HLE_ENABLED;
    pk_audio_reopen((pk_syscon[PK_SYSCON_SYSPLLCTRL] == 0x25 ? 72000000 : 45000000) / (int)rate);
    fprintf(stderr, "audio HLE actif (%d Hz)\n",
            (pk_syscon[PK_SYSCON_SYSPLLCTRL] == 0x25 ? 72000000 : 45000000) / (int)rate);
}

/* valeur écrite sur le R2R : POUT1[31:28] | POUT2[23:20] */
static void pk_audio_gpio_write(void);

static void pk_audio_write(uint8_t data) {
    if (pk_hleState == PK_HLE_ENABLED) return;
    pk_prevData = data;
    float clock = pk_syscon[PK_SYSCON_SYSPLLCTRL] == 0x25 ? 72000000.f : 45000000.f;
    float delta = (float)(uint32_t)(tickCount - pk_prevTicks) / clock;
    pk_prevTicks = tickCount;
    pk_aqDelta[pk_aqEnd] = delta;
    pk_aqData[pk_aqEnd] = data;
    pk_aqEnd = (pk_aqEnd + 1) & PK_AQ_MASK;
    if (pk_aqStart == pk_aqEnd) pk_aqStart = (pk_aqStart + 1) & PK_AQ_MASK;
    else pk_aqSize++;
}

static int pk_audio_ready(void) { return pk_aqSize >= 600; }

/* --- GPIO (0xA0000000) */
static uint32_t pk_pin[3], pk_pout[3], pk_mask[3], pk_dir[3];
static uint32_t pk_isel, pk_ienr, pk_ienf, pk_rise, pk_fall, pk_ist;

static void pk_pout_write(uint32_t p, uint32_t v) {
    if (p == 0) {
        pk_sd_enabled = !(v & (1u << 7));
        pk_pout[0] = v;
        pk_pin[0] = (pk_pin[0] & ~pk_dir[0]) | (v & pk_dir[0]);
        return;
    }
    if (p == 1) {
        if (!(pk_pout[1] & (1u << 12)) && (v & (1u << 12)))
            pk_lcd_write((pk_pout[0] >> 2) & 1, (uint16_t)(pk_pout[2] >> 3));
        if (!(pk_pout[1] & 1) && (v & 1))
            pk_lcd_reset();
        pk_pin[1] = (pk_pin[1] & ~pk_dir[1]) | (v & pk_dir[1]);
        pk_pout[1] = v;
        return;
    }
    pk_pout[2] = v;
    pk_pin[2] = (pk_pin[2] & ~pk_dir[2]) | (v & pk_dir[2]);
}

static void pk_audio_gpio_write(void) {
    pk_audio_write((uint8_t)((pk_pout[1] >> 28) | ((pk_pout[2] >> 16) & 0xF0)));
}

static void pk_gpio_input(uint32_t pinId, uint32_t bit, uint32_t val) {
    val = !!val;
    uint32_t old = (pk_pin[pinId] >> bit) & 1;
    if (old == val) return;
    pk_pin[pinId] = (pk_pin[pinId] & ~(1u << bit)) | (val << bit);
    uint32_t id = bit + (pinId == 1 ? 24 : pinId == 2 ? 56 : 0);
    for (uint32_t f = 0; f < 8; f++) {
        if (pk_syscon[PK_SYSCON_PINTSEL(f)] != id) continue;
        if (val ? (pk_ienr & (1u << f)) : (pk_ienf & (1u << f))) {
            pk_ist |= 1u << f;
            if (val) pk_rise |= 1u << f;
            else pk_fall |= 1u << f;
        }
    }
}

static void pk_gpio_update(void) {
    if (pk_ist && armIrqEnable) {
        for (uint32_t f = 0; f < 8; f++) {
            if (pk_ist & (1u << f)) { pk_interrupt(16 + f); return; }
        }
    }
}

/* --- API ROM (0x30000000) : pointeurs IAP + division */
static uint32_t pk_rom[16] = {
    0x30000000u, 0, 0, 0x30000000u,      /* usbd, rsv, rsv, pPWRD */
    0x30000030u, 0x30000000u, 0x30000000u, 0x30000000u, /* div, i2c, dma, rsv */
    0x30000000u, 0x30000000u, 0, 0x30000000u,           /* rsv, uartN, rsv, uart0 */
    0x1fff1ff3u, 0x1fff1ff5u, 0x1fff1ff7u, 0x1fff1ff9u  /* sidiv uidiv sidivmod uidivmod */
};

static void pk_iap_stub(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    (void)r0; (void)r1; (void)r2; (void)r3;
}

static void pk_iap_write_eeprom(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    (void)r2; (void)r3;
    uint32_t ee = pk_read_word(r0 + 4), buf = pk_read_word(r0 + 8), n = pk_read_word(r0 + 12);
    for (uint32_t i = 0; i < n && ee + i < sizeof(pk_eeprom); i++)
        pk_eeprom[ee + i] = pk_read_byte(buf + i);
    pk_eepromDirty = 1;
    pk_write_word(r1, 0);
}

static void pk_iap_read_eeprom(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    (void)r2; (void)r3;
    uint32_t ee = pk_read_word(r0 + 4), buf = pk_read_word(r0 + 8), n = pk_read_word(r0 + 12);
    for (uint32_t i = 0; i < n && ee + i < sizeof(pk_eeprom); i++)
        pk_write_byte(buf + i, pk_eeprom[ee + i]);
    pk_write_word(r1, 0);
}

static void pk_iap_prewrite(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    (void)r0; (void)r2; (void)r3;
    pk_write_word(r1, 0);
}

static void pk_iap_write_sector(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    (void)r2; (void)r3;
    uint32_t dst = pk_read_word(r0 + 4), src = pk_read_word(r0 + 8), len = pk_read_word(r0 + 12);
    if (dst + len > FLASH_SIZE) len = FLASH_SIZE - dst;
    if (dst & 0xFF) dst &= ~0xFFu;
    for (uint32_t i = 0; i < len; i++)
        flash[dst++] = pk_read_byte(src++);
    pk_write_word(r1, 0);
}

static void pk_iap_erase(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    (void)r0; (void)r2; (void)r3;
    pk_write_word(r1, 0);
}

static void pk_iap_read_uid(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    (void)r0; (void)r2; (void)r3;
    static const uint32_t uid[5] = { 0, 0x10101010u, 0x20202020u, 0x30303030u, 0x40404040u };
    for (int i = 0; i < 5; i++, r1 += 4) pk_write_word(r1, uid[i]);
}

static void (*pk_iap_cmd[63])(uint32_t, uint32_t, uint32_t, uint32_t) = {
    [50] = pk_iap_prewrite,
    [51] = pk_iap_write_sector,
    [52] = pk_iap_erase,
    [54] = pk_iap_stub,
    [57] = pk_iap_read_uid,
    [58] = pk_iap_erase,
    [61] = pk_iap_write_eeprom,
    [62] = pk_iap_read_eeprom,
};

/* BLX vers l'API ROM (0x1fff1ffx) : IAP et division (42 ticks comme la
 * référence) */
static void pk_blx(uint32_t opcode) {
    int base = (opcode >> 3) & 15;
    uint32_t target = regs[base];
    if ((target & 0xFFFFFFF0u) == 0x1fff1ff0u) {
        uint32_t t = (target & 0xF) >> 1;
        switch (t) {
            case 0: {
                uint32_t cmdId = pk_read_word(regs[0]);
                if (cmdId > 62) fprintf(stderr, "IAP invalide : %u\n", cmdId);
                else {
                    if (!pk_iap_cmd[cmdId]) pk_iap_stub(regs[0], regs[1], regs[2], regs[3]);
                    else pk_iap_cmd[cmdId](regs[0], regs[1], regs[2], regs[3]);
                }
                break;
            }
            case 1: regs[0] = (uint32_t)((int32_t)regs[0] / (int32_t)regs[1]); break;
            case 2: regs[0] = regs[0] / regs[1]; break;
            case 3: {
                int32_t n = (int32_t)regs[1], d = (int32_t)regs[2];
                pk_write_word(regs[0], (uint32_t)(n / d));
                pk_write_word(regs[0] + 4, (uint32_t)(n % d));
                break;
            }
            case 4: {
                uint32_t n = regs[1], d = regs[2];
                pk_write_word(regs[0], n / d);
                pk_write_word(regs[0] + 4, n % d);
                break;
            }
        }
        /* 42 ticks comme la référence (notre pas en a déjà compté 1) ;
         * regs[15] est déjà sur l'adresse de reprise (BLX+4 = next+2) :
         * on avance les compteurs SANS toucher le PC */
        for (int i = 0; i < 41; i++) { sysTickTrigger++; tickCount++; }
        return;
    }
    setReg(14, regs[15] - 2);       /* LR = BLX + 2 (regs[15] est à +4) */
    setReg(15, target & ~1u);
    incrementPc();
}

/* --- interruption (xPSR aux positions ARM réelles) */
static long pk_irqCount[64];
static void pk_interrupt(uint32_t id) {
    if (id < 64) pk_irqCount[id]++;
    uint32_t psr = (uint32_t)(fN ? 1u << 31 : 0) | (fZ ? 1u << 30 : 0) |
                   (fC ? 1u << 29 : 0) | (fV ? 1u << 28 : 0) | (1u << 24);
    pushStack(psr);
    pushStack(regs[15]);
    pushStack(regs[14]);
    pushStack(regs[12]);
    pushStack(regs[3]);
    pushStack(regs[2]);
    pushStack(regs[1]);
    pushStack(regs[0]);
    regs[14] = 0xfffffff9u;
    regs[15] = pk_read_word(sys_VTOR + (id << 2));
    armIrqEnable = 0;
    incrementPc();
}

/* --- timers : SysTick + CT32B0/1 (delta en ticks CPU) */
static uint32_t pk_systick_tick(uint32_t delta) {
    if (!(pk_systickCSR & 1)) return ~0u;
    pk_systickCVR -= delta;
    if ((int32_t)pk_systickCVR < 0) {
        pk_systickCVR += pk_systickRVR & 0xFFFFFF;
        pk_systickCSR |= 1u << 16;
    }
    if (armIrqEnable && (pk_systickCSR & (1u << 16))) {
        pk_systickCSR &= ~(1u << 16);
        pk_interrupt(15);
    }
    return pk_systickCVR;
}

static uint32_t pk_ct_tick(struct pk_ct *ct, uint32_t num, uint32_t delta) {
    uint32_t tti = 128;
    if (!(ct->r[PK_CT_TCR] & 1)) return ~0u;
    int32_t pc = (int32_t)ct->r[PK_CT_PC];
    if (pc < 0) pc = 0;
    pc += (int32_t)delta;
    uint32_t oldTC = ct->r[PK_CT_TC];
    uint32_t pr = ct->r[PK_CT_PR] + 1;
    uint32_t cc = (uint32_t)pc / pr;
    pc -= (int32_t)(cc * pr);
    ct->r[PK_CT_PC] = (uint32_t)pc;
    ct->r[PK_CT_TC] += cc;

    for (int m = 0; m < 4; m++) {
        uint32_t mri = 1u << (m * 3), mrr = 1u << (m * 3 + 1), mrs = 1u << (m * 3 + 2);
        uint32_t mr = ct->r[6 + (uint32_t)m];
        if (oldTC < mr && ct->r[PK_CT_TC] >= mr) {
            if (ct->r[5] & mri) {
                ct->r[PK_CT_IR] |= 1u << m;
                uint32_t t = (mr - ct->r[PK_CT_TC]) * pr;
                if (m == 0 || t < tti) tti = t;
            }
            if (ct->r[5] & mrs) ct->r[PK_CT_TCR] &= ~1u;
            if (ct->r[5] & mrr) ct->r[PK_CT_TC] -= mr;
        } else if (ct->r[PK_CT_TC] < mr) {
            uint32_t t = (mr - ct->r[PK_CT_TC]) * pr;
            if (m == 0 || t < tti) tti = t;
        }
    }

    if (ct->r[PK_CT_IR] && armIrqEnable) {
        if (num == 0) pk_audio_check_hle(ct->r[7]); /* MR1 */
        pk_interrupt(34 + num);
    }
    return tti;
}

static void pk_timers_update(void) {
    uint32_t delta = tickCount - pk_lastTick;
    if (!delta) return;
    pk_lastTick = tickCount;
    pk_systick_tick(delta);
    pk_ct_tick(&pk_ct[0], 0, delta);
    pk_ct_tick(&pk_ct[1], 1, delta);
}

/* chaîne d'interruptions + reset AIRCR, appelée au début de chaque pas */
static void pk_machine_step(void) {
    if (sys_AIRCR & 4) { /* SYSRESETREQ */
        sys_AIRCR = 0x05FA0000u;
        pk_reset_core();
    }
    pk_timers_update();
    pk_gpio_update();
}

/* --- mémoire : bancs LPC (mêmes sémantiques que la référence :
 *     lecture hors bornes -> HardFault + ~0, écriture -> silencieuse) */

static uint32_t pk_rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t pk_ct_peek(const struct pk_ct *ct, uint32_t a) {
    uint32_t off = a - (a < 0x40018000u ? 0x40014000u : 0x40018000u);
    uint32_t idx = off >> 2;
    return idx < 14 ? ct->r[idx] : 0;
}

static uint32_t pk_reg_peek(uint32_t a) {
    if (a < 0x10000000u)
        return a + 4 <= FLASH_SIZE ? pk_rd32le(flash + a) : 0;
    if (a < 0x20000000u) {
        uint32_t o = a - 0x10000000u;
        return o + 4 <= SRAM_SIZE ? pk_rd32le(sram + o) : 0;
    }
    if (a < 0x30000000u) {
        if ((a >> 14) & 1) {
            uint32_t o = a - 0x20004000u;
            return o + 4 <= sizeof(pk_usbsram) ? pk_rd32le(pk_usbsram + o) : 0;
        }
        uint32_t o = a - 0x20000000u;
        return o + 4 <= sizeof(pk_sram1) ? pk_rd32le(pk_sram1 + o) : 0;
    }
    if (a < 0x40000000u) {
        uint32_t idx = (a - 0x30000000u) >> 2;
        return idx < 16 ? pk_rom[idx] : 0;
    }
    if (a < 0x50000000u) { /* APB */
        switch ((a >> 14) & 0x1F) {
            case 2: { uint32_t i = (a - 0x40008000u) >> 2;
                      return i < 19 ? pk_usart[i] : 0; }
            case 5: return pk_ct_peek(&pk_ct[0], a);
            case 6: return pk_ct_peek(&pk_ct[1], a);
            case 7: {
                if ((a & ~3u) == 0x4001C040u) return pk_adc_dat8;
                if ((a & ~3u) == 0x4001C044u) return pk_adc_dat9;
                uint32_t i = (a - 0x4001C000u) >> 2;
                return i < 22 ? pk_adc[i] : 0;
            }
            case 9: { uint32_t i = (a - 0x40024000u) >> 2;
                      return i < 4 ? pk_rtc[i] : 0; }
            case 16: { uint32_t off = a - 0x40040000u;
                       return off <= 0x24 ? (&pk_spi0.cr0)[off >> 2] : 0; }
            case 17: { uint32_t i = (a - 0x40044000u) >> 2;
                       return i < 88 ? pk_iocon[i] : 0; }
            case 18: { uint32_t i = (a - 0x40048000u) >> 2;
                       return i < 256 ? pk_syscon[i] : 0; }
            case 22: { uint32_t off = a - 0x40058000u;
                       return off <= 0x24 ? (&pk_spi1.cr0)[off >> 2] : 0; }
            default: return 0;
        }
    }
    if (a < 0x60000000u) { /* CDS : SCT0/1 */
        uint32_t sel = (a >> 13) & 0xF;
        if (sel == 6) { uint32_t i = (a - 0x5000C000u) >> 2;
                        return i < 324 ? pk_sct[0][i] : 0; }
        if (sel == 7) { uint32_t i = (a - 0x5000E000u) >> 2;
                        return i < 324 ? pk_sct[1][i] : 0; }
        return 0;
    }
    if (a >= 0xA0000000u && a < 0xB0000000u) { /* GPIO */
        uint32_t off = a - 0xA0000000u;
        if (off < 0x1000) { /* banque octet */
            uint32_t p, b;
            if (off < 0x18) { p = 0; b = off; }
            else if (off < 0x40) { p = 1; b = off - 0x20; }
            else if (off < 0x58) { p = 2; b = off - 0x40; }
            else return 0;
            uint32_t bit = b; /* l'adresse octet encode déjà la broche */
            return ((pk_pin[p] >> bit) & 1);
        }
        if (off < 0x2000) { /* banque mot */
            uint32_t p, bit;
            if (off < 0x60) { p = 0; bit = off >> 2; }
            else if (off >= 0x80 && off < 0x100) { p = 1; bit = (off - 0x80) >> 2; }
            else if (off >= 0x100 && off < 0x180) { p = 2; bit = (off - 0x100) >> 2; }
            else return 0;
            return (pk_pin[p] >> bit) & 1;
        }
        if (off >= 0x2000 && off < 0x3000) { /* banque principale */
            uint32_t idx = (off - 0x2000) >> 2;
            if (idx >= 64 && idx < 67) return pk_pin[idx - 64];
            if (idx >= 96 && idx < 99) return pk_pin[idx - 96] & ~pk_mask[idx - 96];
            if (idx < 3) return pk_dir[idx];
            if (idx >= 32 && idx < 35) return pk_mask[idx - 32];
            if (idx >= 128 && idx < 195) return pk_pin[(idx - 128) % 4];
            return 0;
        }
        if (off >= 0x4000 && off < 0x5000) { /* drapeaux d'interruption */
            uint32_t idx = (off - 0x4000) >> 2;
            switch (idx) {
                case 0: return pk_isel;
                case 1: case 2: case 3: return pk_ienr;
                case 4: case 5: case 6: return pk_ienf;
                case 7: return pk_rise;
                case 8: return pk_fall;
                case 9: return pk_ist;
                default: return 0;
            }
        }
        return 0;
    }
    if (a >= 0xE000E000u && a < 0xE0010000u) { /* PPB */
        uint32_t off = a - 0xE000E000u;
        switch (off) {
            case 0x010: return pk_systickCSR;
            case 0x014: return pk_systickRVR;
            case 0x018: return pk_systickCVR;
            case 0x01C: return 4; /* CALIB */
            case 0x100: case 0x180: case 0x200: case 0x280:
                return pk_syscon[192 + off / 0x80]; /* ISER/ICER/ISPR/ICPR */
            case 0xD00: return 0x410CC200u; /* CPUID Cortex-M0 */
            case 0xD04: return 0;
            case 0xD08: return sys_VTOR;
            case 0xD0C: return 0x05FA0000u;
            default: return 0;
        }
    }
    return 0;
}

static uint32_t pk_reg_read(uint32_t a) {
    uint32_t v = pk_reg_peek(a);
    if (a < 0x50000000u) {
        uint32_t al = a & ~3u;
        if (al == 0x4001C024u) return pk_prng() & 0xFFF; /* ADC DAT1 */
        if ((al == 0x40014008u || al == 0x40018008u) ||
            (al == 0x40024008u && pk_rtcEnabled))
            pk_timers_update();
        if (al == 0x40014008u) return pk_ct[0].r[PK_CT_TC];
        if (al == 0x40018008u) return pk_ct[1].r[PK_CT_TC];
        if (al == 0x40024008u && pk_rtcEnabled)
            return getenv("EMU_FIXED_RTC")
                       ? (uint32_t)strtoul(getenv("EMU_FIXED_RTC"), NULL, 10)
                       : (uint32_t)time(NULL);
        if (al == 0x40040008u) return pk_spi_read_dr(&pk_spi0);
        if (al == 0x40058008u) return pk_spi_read_dr(&pk_spi1);
    }
    return v;
}

static void pk_reg_write(uint32_t a, uint32_t v) {
    if (a < 0x10000000u) { /* flash en lecture seule */
        if (pk_ignoreBadWrites) { pk_ignoreBadWrites--; return; }
        pk_interrupt(3);
        return;
    }
    if (a < 0x20000000u) {
        uint32_t o = a - 0x10000000u;
        if (o + 4 <= SRAM_SIZE) {
            sram[o] = v & 0xFF; sram[o+1] = (v >> 8) & 0xFF;
            sram[o+2] = (v >> 16) & 0xFF; sram[o+3] = (v >> 24) & 0xFF;
        }
        return;
    }
    if (a < 0x30000000u) {
        uint8_t *buf = ((a >> 14) & 1) ? pk_usbsram : pk_sram1;
        uint32_t base = ((a >> 14) & 1) ? 0x20004000u : 0x20000000u;
        uint32_t sz = 0x800;
        uint32_t o = a - base;
        if (o + 4 <= sz) {
            buf[o] = v & 0xFF; buf[o+1] = (v >> 8) & 0xFF;
            buf[o+2] = (v >> 16) & 0xFF; buf[o+3] = (v >> 24) & 0xFF;
        }
        return;
    }
    if (a < 0x40000000u) return; /* ROM : lecture seule (silencieux) */
    if (a < 0x50000000u) { /* APB */
        switch ((a >> 14) & 0x1F) {
            case 2: {
                uint32_t i = (a - 0x40008000u) >> 2;
                if (i < 19) pk_usart[i] = v;
                if ((a & ~3u) == 0x40008000u && pk_usart[3] == 3) { /* LCR == 8N1 */
                    fputc((int)(v & 0xFF), stdout);
                    if ((v & 0xFF) == 10) fflush(stdout);
                }
                return;
            }
            case 5: case 6: {
                struct pk_ct *ct = ((a >> 14) & 0x1F) == 5 ? &pk_ct[0] : &pk_ct[1];
                uint32_t off = a - (((a >> 14) & 0x1F) == 5 ? 0x40014000u : 0x40018000u);
                uint32_t idx = off >> 2;
                if (idx == 0) { ct->r[0] &= ~v; return; }        /* IR : acquitte */
                if (idx == 1) {                                   /* TCR */
                    ct->r[1] = v;
                    if (v & 2) { ct->r[PK_CT_TC] = 0; ct->r[PK_CT_PC] = 0; }
                    return;
                }
                if (idx < 14) ct->r[idx] = v;
                return;
            }
            case 7: { /* ADC */
                uint32_t i = (a - 0x4001C000u) >> 2;
                if (i < 22) pk_adc[i] = (i == 0) ? 0 : v; /* CTRL écrit -> 0 */
                return;
            }
            case 9: { /* RTC */
                uint32_t i = (a - 0x40024000u) >> 2;
                if (i < 4) pk_rtc[i] = v;
                if ((a & ~3u) == 0x40024000u)
                    pk_rtcEnabled = !(v & 1) && (v & (1u << 7)) &&
                                    (pk_syscon[PK_SYSCON_SYSAHBCLKCTRL] & (1u << 30));
                return;
            }
            case 16: case 22: {
                struct pk_spi *s = ((a >> 14) & 0x1F) == 16 ? &pk_spi0 : &pk_spi1;
                uint32_t off = a - (((a >> 14) & 0x1F) == 16 ? 0x40040000u : 0x40058000u);
                switch (off) {
                    case 0x00:
                        s->cr0 = v;
                        s->dataSize = (int)(v & 0xF);
                        s->frameSize = s->dataSize == 0xF ? 2 : 1;
                        return;
                    case 0x04: s->cr1 = v; return;
                    case 0x08: s->dr = pk_spi_write_dr(s, v); return;
                    case 0x0C: return; /* SR : lecture seule */
                    default:
                        if (off <= 0x24) (&s->cr0)[off >> 2] = v;
                        return;
                }
            }
            case 17: { uint32_t i = (a - 0x40044000u) >> 2;
                       if (i < 88) pk_iocon[i] = v;
                       return; }
            case 18: { uint32_t i = (a - 0x40048000u) >> 2;
                       if (i < 256) pk_syscon[i] = v;
                       return; }
            default: return;
        }
    }
    if (a < 0x60000000u) { /* CDS : SCT0/1 */
        uint32_t sel = (a >> 13) & 0xF;
        if (sel == 6) { uint32_t i = (a - 0x5000C000u) >> 2;
                        if (i < 324) pk_sct[0][i] = v; }
        else if (sel == 7) { uint32_t i = (a - 0x5000E000u) >> 2;
                             if (i < 324) pk_sct[1][i] = v; }
        return;
    }
    if (a >= 0xA0000000u && a < 0xB0000000u) { /* GPIO */
        uint32_t off = a - 0xA0000000u;
        if (off < 0x1000) { /* banque octet */
            uint32_t p, b;
            if (off < 0x18) { p = 0; b = off; }
            else if (off < 0x40) { p = 1; b = off - 0x20; }
            else if (off < 0x58) { p = 2; b = off - 0x40; }
            else return;
            uint32_t bit = b; /* l'adresse octet encode déjà la broche */
            if (v & 0xFF) pk_pout[p] |= 1u << bit;
            else pk_pout[p] &= ~(1u << bit);
            if (a == 0xA0000057u) pk_audio_gpio_write(); /* bit 23 latching */
            return;
        }
        if (off < 0x2000) { /* banque mot */
            uint32_t p, bit;
            if (off < 0x60) { p = 0; bit = off >> 2; }
            else if (off >= 0x80 && off < 0x100) { p = 1; bit = (off - 0x80) >> 2; }
            else if (off >= 0x100 && off < 0x180) { p = 2; bit = (off - 0x100) >> 2; }
            else return;
            pk_pout_write(p, (pk_pout[p] & ~(1u << bit)) | ((!!v) << bit));
            return;
        }
        if (off >= 0x2000 && off < 0x3000) { /* banque principale */
            uint32_t idx = (off - 0x2000) >> 2;
            if (idx < 3) { pk_dir[idx] = v; return; }
            if (idx >= 32 && idx < 35) { pk_mask[idx - 32] = v; return; }
            if (idx >= 64 && idx < 67) { pk_pout_write(idx - 64, v); return; }
            if (idx >= 96 && idx < 99) { /* MPIN */
                uint32_t w = idx - 96;
                pk_pout_write(w, (pk_pout[w] & pk_mask[w]) | (v & ~pk_mask[w]));
                if (w == 2 && pk_mask[2] == ~0x00F00000u) pk_audio_gpio_write();
                return;
            }
            if (idx >= 128 && idx < 131) { /* SET */
                uint32_t w = idx - 128;
                pk_pout_write(w, pk_pout[w] | v);
                if (w == 2 && (v & (1u << 23))) pk_audio_gpio_write();
                return;
            }
            if (idx >= 160 && idx < 163) { /* CLR */
                uint32_t w = idx - 160;
                pk_pout_write(w, pk_pout[w] & ~v);
                if (w == 2 && (v & (1u << 23))) pk_audio_gpio_write();
                return;
            }
            if (idx >= 192 && idx < 195) { /* TOGGLE */
                pk_pout_write(idx - 192, pk_pout[idx - 192] ^ v);
                return;
            }
            return;
        }
        if (off >= 0x4000 && off < 0x5000) { /* drapeaux d'interruption */
            uint32_t idx = (off - 0x4000) >> 2;
            switch (idx) {
                case 0: pk_isel = v; return;
                case 1: pk_ienr = v; return;
                case 2: pk_ienr |= v; return;
                case 3: pk_ienr &= ~v; return;
                case 4: pk_ienf = v; return;
                case 5: pk_ienf |= v; return;
                case 6: pk_ienf &= ~v; return;
                case 7: pk_rise = v; return;
                case 8: pk_fall = v; return;
                case 9: pk_ist &= ~v; pk_rise &= ~v; pk_fall &= ~v; return;
            }
            return;
        }
        return;
    }
    if (a >= 0xE000E000u && a < 0xE0010000u) { /* PPB */
        uint32_t off = a - 0xE000E000u;
        switch (off) {
            case 0x010: pk_systickCSR = v; return;
            case 0x014: pk_systickRVR = v; return;
            case 0x018: pk_systickCSR &= ~(1u << 16); pk_systickCVR = pk_systickRVR; return;
            case 0x100: case 0x180: case 0x200: case 0x280:
                pk_syscon[192 + off / 0x80] = v; return;
            case 0xD08: sys_VTOR = v; return;
            case 0xD0C: sys_AIRCR = 0x05FA0000u | (v & 4); return;
            default: return;
        }
    }
}

static uint32_t pk_read_word(uint32_t a) {
    uint32_t v = pk_reg_read(a & ~3u);
    return v;
}
static uint16_t pk_read_half(uint32_t a) {
    uint32_t v = pk_reg_read(a & ~3u);
    return (uint16_t)(v >> ((a & 2) << 3));
}
static uint8_t pk_read_byte(uint32_t a) {
    uint32_t v = pk_reg_read(a & ~3u); /* mot aligné, puis lane d'octet */
    return (uint8_t)(v >> ((a & 3) << 3));
}
static void pk_write_word(uint32_t a, uint32_t v) { pk_reg_write(a & ~3u, v); }
static void pk_write_half(uint32_t a, uint16_t v) {
    uint32_t al = a & ~3u;
    uint32_t old = pk_reg_peek(al);
    uint32_t lane = (uint32_t)(a & 2) << 3;
    pk_reg_write(al, (old & ~(0xFFFFu << lane)) | ((uint32_t)v << lane));
}
static void pk_write_byte(uint32_t a, uint8_t v) {
    uint32_t al = a & ~3u;
    uint32_t old = pk_reg_peek(al);
    uint32_t sh = (uint32_t)(a & 3) << 3;
    pk_reg_write(al, (old & ~(0xFFu << sh)) | ((uint32_t)v << sh));
}

/* --- boutons -> GPIO (A=1_9, B=1_4, C=1_10, haut=1_13, bas=1_3,
 *     gauche=1_25, droite=1_7, D/éclairage=0_1) */
static void pk_btn_gpio(uint8_t mask, int pressed) {
    static const struct { uint8_t m; uint32_t port, bit; } map[] = {
        { BTN_DOWN, 1, 3 }, { BTN_LEFT, 1, 25 }, { BTN_RIGHT, 1, 7 }, { BTN_UP, 1, 13 },
        { BTN_A, 1, 9 }, { BTN_B, 1, 4 }, { BTN_MENU, 1, 10 }, { BTN_HOME, 0, 1 },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (mask & map[i].m) pk_gpio_input(map[i].port, map[i].bit, (uint32_t)pressed);
}

/* ADC DAT8/9 depuis le stick (axe droit de la manette, axes 4/3 d'un
 * joystick brut, comme la référence) */
static void pk_adc_frame(void) {
    if (joyFb) {
        pk_adc_dat8 = (uint32_t)(((int32_t)SDL_JoystickGetAxis(joyFb, 4) >> 1) + 0x8000);
        pk_adc_dat9 = (uint32_t)((-(int32_t)SDL_JoystickGetAxis(joyFb, 3) >> 1) + 0x8000);
    } else if (pad) {
        int16_t ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
        int16_t ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
        pk_adc_dat8 = (uint32_t)(((int32_t)ax >> 1) + 0x8000);
        pk_adc_dat9 = (uint32_t)((-(int32_t)ay >> 1) + 0x8000);
    } else {
        pk_adc_dat8 = pk_adc_dat9 = 0x8000;
    }
}

/* --- réinitialisation de la machine Pokitto */
static void pk_reset_core(void) {
    /* table CRC16 (CDMA/x25 du protocole SD), calculée une fois */
    static int crcInit;
    if (!crcInit) {
        crcInit = 1;
        for (int i = 0; i < 256; i++) {
            uint16_t r = (uint16_t)(i << 8);
            for (int j = 0; j < 8; j++)
                r = (r & 0x8000) ? (uint16_t)((r << 1) ^ 0x1021) : (uint16_t)(r << 1);
            pk_sd_crc[i] = r;
        }
    }

    /* SRAM « poubelle » déterministe (la référence tire un rand() par octet) */
    pk_prngState = 0x2545F491u;
    for (unsigned i = 0; i < SRAM_SIZE; i++) sram[i] = (uint8_t)pk_prng();
    for (unsigned i = 0; i < sizeof pk_sram1; i++) pk_sram1[i] = (uint8_t)pk_prng();
    for (unsigned i = 0; i < sizeof pk_usbsram; i++) pk_usbsram[i] = (uint8_t)pk_prng();

    memset(regs, 0, sizeof regs);
    memset(regD, 0, sizeof regD);
    fN = fZ = fC = fV = 0;
    armIrqEnable = 1;
    tickCount = 0;
    pk_lastTick = 0;

    /* SYSCON / périphériques (valeurs de la référence) */
    memset(pk_syscon, 0, sizeof pk_syscon);
    pk_syscon[PK_SYSCON_SYSPLLCTRL] = 0x23;
    pk_syscon[17] = 1;  /* SYSPLLCLKUEN */
    pk_syscon[3] = 1;   /* SYSPLLSTAT */
    pk_syscon[29] = 1;  /* MAINCLKUEN */
    pk_syscon[PK_SYSCON_SYSAHBCLKCTRL] = 0x8004857u;
    pk_syscon[30] = 1;  /* SYSAHBCLKDIV */
    pk_syscon[19] = 1;  /* USBPLLCLKUEN */
    pk_syscon[5] = 1;   /* USBPLLSTAT */
    sys_VTOR = 0;
    sys_AIRCR = 0x05FA0000u;
    for (unsigned i = 0; i < 88; i++) pk_iocon[i] = 0x90;
    memset(pk_adc, 0, sizeof pk_adc);
    pk_adc[4] = 0x80000000u; /* SEQA_GDAT */
    pk_adc[16] = pk_adc[17] = 0x8000;
    pk_adc_dat8 = pk_adc_dat9 = 0x8000;
    pk_rtc[0] = 7; pk_rtc[1] = 0xFFFF;
    pk_rtc[2] = getenv("EMU_FIXED_RTC")
                    ? (uint32_t)strtoul(getenv("EMU_FIXED_RTC"), NULL, 10)
                    : (uint32_t)time(NULL);
    pk_rtcEnabled = 0;
    memset(pk_usart, 0, sizeof pk_usart);
    pk_usart[0] = 1; pk_usart[5] = 0x60; pk_usart[10] = 0x10;
    pk_usart[11] = 0xF0; pk_usart[12] = 0x80;
    memset(pk_ct, 0, sizeof pk_ct);
    memset(pk_sct, 0, sizeof pk_sct);
    pk_sct[0][0] = 0x7E00; pk_sct[0][1] = 0x00040004u;
    pk_sct[1][0] = 0x7E00; pk_sct[1][1] = 0x00040004u;
    memset(&pk_spi0, 0, sizeof pk_spi0);
    memset(&pk_spi1, 0, sizeof pk_spi1);
    pk_spi0.sr = pk_spi1.sr = 3;
    pk_spi0.imsc = pk_spi1.imsc = 0x8;
    pk_spi0.clearable = pk_spi1.clearable = 1;
    pk_sd_machine_reset();
    pk_sd_enabled = 1;

    pk_lcd_reset();
    memset(pk_lcd, 0, sizeof pk_lcd);
    pk_lcdDirty = 1;

    /* audio */
    pk_hleState = PK_HLE_DETECT;
    pk_hleIrqAddress = 0;
    pk_hleBuffer = NULL;
    pk_hlePlayhead = NULL;
    pk_aqStart = pk_aqEnd = pk_aqSize = 0;
    pk_prevData = 0xFF;
    pk_prevTicks = 0;
    aq_head = aq_tail = 0;
    audioHold = 0;

    /* l'EEPROM survit au reset (mémoire d'état, sauvegardes) */
    pk_ist = pk_rise = pk_fall = 0;
    pk_ienr = pk_ienf = 0;
    pk_pin[0] = pk_pin[1] = pk_pin[2] = 0;
    pk_pout[0] = pk_pout[1] = pk_pout[2] = 0;
    pk_mask[0] = pk_mask[1] = pk_mask[2] = 0;
    pk_dir[0] = pk_dir[1] = pk_dir[2] = 0;

    /* vecteurs (la flash est déjà chargée) */
    regs[13] = pk_read_word(0);
    regs[14] = 0xffffffffu;
    regs[15] = pk_read_word(4) & ~1u;
    incrementPc();
    emu_nextFrameTick = tickCount + frame_ticks();
}

/* EEPROM : chargée/sauvegardée à côté du .bin (natif uniquement) */
static void pk_eeprom_path(char *out, size_t n) {
    snprintf(out, n, "%s", fwPath);
    char *dot = strrchr(out, '.');
    if (dot && strchr(out, '/')) {
        /* ne tronque que si l'extension est courte */
        if (out + strlen(out) - dot <= 5) *dot = 0;
    }
    snprintf(out + strlen(out), n - strlen(out), ".eeprom");
}

static void pk_eeprom_load(void) {
#ifndef __EMSCRIPTEN__
    char path[1100];
    pk_eeprom_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(pk_eeprom, 1, sizeof pk_eeprom, f);
    (void)n;
    fclose(f);
    fprintf(stderr, "eeprom : %s\n", path);
#endif
}

static void pk_eeprom_save(void) {
#ifndef __EMSCRIPTEN__
    if (!pk_eepromDirty) return;
    char path[1100];
    pk_eeprom_path(path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(pk_eeprom, 1, sizeof pk_eeprom, f);
    fclose(f);
    pk_eepromDirty = 0;
    printf("eeprom : %s\n", path);
#endif
}

/* DEBUG temporaire : état IRQ/timers */
static void pk_debug_dump(void) {
    if (!getenv("EMU_PK_DEBUG")) return;
    for (int i = 0; i < 64; i++)
        if (pk_irqCount[i]) fprintf(stderr, "IRQ %d : %ld tirs\n", i, pk_irqCount[i]);
    for (int n = 0; n < 2; n++)
        fprintf(stderr, "CT32B%d : TCR=%x TC=%u PR=%u MCR=%x MR=[%u %u %u %u] IR=%x armIrq=%d\n",
                n, pk_ct[n].r[1], pk_ct[n].r[2], pk_ct[n].r[3], pk_ct[n].r[5],
                pk_ct[n].r[6], pk_ct[n].r[7], pk_ct[n].r[8], pk_ct[n].r[9],
                pk_ct[n].r[0], armIrqEnable);
    fprintf(stderr, "SysTick CSR=%x RVR=%u CVR=%u\n", pk_systickCSR, pk_systickRVR, pk_systickCVR);
}

/* export de la carte SD modifiée (--out-img) */
static void pk_card_export(void) {
#ifndef __EMSCRIPTEN__
    if (!outImgPath[0] || !pk_sd_dirty) return;
    uint8_t *card = pk_sd_card();
    size_t sz = pk_sd_card_size();
    if (!card || !sz) return;
    FILE *f = fopen(outImgPath, "wb");
    if (!f) return;
    fwrite(card, 1, sz, f);
    fclose(f);
    printf("carte exportée : %s (%zu Kio)\n", outImgPath, sz / 1024);
#endif
}



static long stWrites = 0, ramwrTotal = 0;
/* le panneau META est câblé BGR ; une init qui déclare MADCTL.BGR=1
 * (jeux maison à init custom, ex. lapinou : 0xC8) envoie des couleurs
 * d'ordre natif — l'émulateur doit inverser R/B à l'affichage.  La lib
 * standard ne déclare jamais BGR et pré-swappe elle-même. */
static int lcdBgrSwapped;
static uint8_t st7735_byte(uint8_t v) {
    if (portB_out & (1u << 22)) return 0xff; /* CS écran haut */
    stWrites++;
    if (!(portB_out & (1u << 23))) { /* commande */
        lcd_lastCommand = v;
        lcd_argIndex = 0;            /* comme st7735.ts : reset à chaque commande */
        if (v == 0x2c) ramwrTotal++;
        if (v == 0x36 && getenv("EMU_LCD_DEBUG")) fprintf(stderr, "[lcd] MADCTL cmd\n");
        return 0xff;
    }
    if (lcd_lastCommand == 0x36) {
        if (v & 0x08) lcdBgrSwapped = 1;
        if (getenv("EMU_LCD_DEBUG"))
            fprintf(stderr, "[lcd] MADCTL <- %02x (BGR=%d, verrou=%d)\n", v, (v >> 3) & 1, lcdBgrSwapped);
    }
    { /* données */
        switch (lcd_lastCommand) {
            case 0x2c: /* RAMWR */
                if (lcd_argIndex % 2 == 0) lcd_tmp = v;
                else {
                    uint16_t p = (uint16_t)((lcd_tmp << 8) | v);
                    if (lcdBgrSwapped) p = (uint16_t)((p >> 11) | (p & 0x07e0u) | ((p & 0x1fu) << 11));
                    if (lcd_x < 160 && lcd_y < 128) pix[lcd_y * 160 + lcd_x] = p;
                    if (++lcd_x > lcd_xEnd) { lcd_x = lcd_xStart; if (++lcd_y > lcd_yEnd) lcd_y = lcd_yStart; }
                }
                break;
            case 0x2a: /* CASET */
                if (lcd_argIndex == 1) lcd_xStart = lcd_x = v;
                else if (lcd_argIndex == 3) lcd_xEnd = v;
                break;
            case 0x2b: /* RASET */
                if (lcd_argIndex == 1) lcd_yStart = lcd_y = v;
                else if (lcd_argIndex == 3) lcd_yEnd = v;
                break;
        }
        lcd_argIndex++;
    }
    return 0xff;
}

/* ------------------------------------------------------------ DMAC */

static uint32_t dmac_baseAddr, dmac_wrbAddr, dmac_desc, dmac_chid;

/* Canaux déclenchés par TC4 (audio META_AUDIO_DMA du firmware gbrecomp) :
 * suivis à part du hack « copie immédiate » ci-dessus, sinon leur chaîne
 * détourne dmac_desc et l'écran (canal 0) ne reçoit plus rien.  Un beat
 * par débordement TC4 ; le descripteur suivant est lu à la fin du courant
 * (invalide ou nul -> canal arrêté, comme une erreur de fetch). */
#define DMAC_CHANNELS 12
#define DMAC_TRIG_TC4_OVF 0x1bu /* TC4_DMAC_ID_OVF (SAMD21) */
#define DMAC_TRIG_SERCOM4_TX 0x0au /* SERCOM4_DMAC_ID_TX (écran) */
#define DMAC_TRIG_SERCOM4_RX 0x09u /* SERCOM4_DMAC_ID_RX (carte SD, lib récente) */
/* DMA SPI cadencé : les beats sortent au rythme du baud SERCOM4 (le CPU
 * continue de tourner pendant le transfert — sur hardware le DMA écran
 * prend ~6,8 ms par demi-frame à 24 MHz, d'où les 40-55 fps réels) */
static int      spiDmaCh = -1;   /* canal SERCOM4-TX en cours, -1 = aucun */
static uint32_t spiBeatAcc;      /* accumulateur de ticks */
static uint32_t spiBeatTicks = 7; /* ticks par beat (baud) */
static uint32_t spiBaud;         /* SERCOM4 BAUD (f = 48 MHz / (2×(b+1))) */
static uint8_t  dmaTrig[DMAC_CHANNELS], dmaOn[DMAC_CHANNELS];
static uint8_t  dmacIntFlag[DMAC_CHANNELS]; /* INTFLAG par canal : TCMPL=0x02, SUSP=0x04 */
static uint8_t  dmacIntEn[DMAC_CHANNELS];   /* CHINTENSET par canal (lecture) */
static uint16_t dmaCtrl[DMAC_CHANNELS], dmaCnt[DMAC_CHANNELS], dmaIdx[DMAC_CHANNELS];
static uint32_t dmaSrc[DMAC_CHANNELS], dmaDst[DMAC_CHANNELS], dmaNext[DMAC_CHANNELS];

static uint16_t fetchHalf(uint32_t a);
static uint32_t fetchWord(uint32_t a);
static uint8_t fetchByte(uint32_t a);
static void writeWord(uint32_t a, uint32_t v);
static void writeHalf(uint32_t a, uint16_t v);
static void writeByte(uint32_t a, uint8_t v);

static void dma_sercom4_rx_beat(void);
static int dmaBeatSkipSd; /* un beat DMA d'affichage ne doit pas horloger la carte */
static int dma_is_tc4(uint32_t ch) {
    return ch < DMAC_CHANNELS && dmaTrig[ch] == DMAC_TRIG_TC4_OVF;
}

static void dma_load(uint32_t ch, uint32_t desc) {
    uint16_t ctrl = desc ? fetchHalf(desc) : 0;
    if (!(ctrl & 1u)) { /* VALID absent : fin de chaîne -> canal suspendu */
        dmaOn[ch] = 0;
        dmacIntFlag[ch] |= 0x04; /* SUSP */
        dmacInterrupt = 1;
        return;
    }
    dmaCtrl[ch] = ctrl;
    dmaCnt[ch] = fetchHalf(desc + 0x02);
    dmaSrc[ch] = fetchWord(desc + 0x04);
    dmaDst[ch] = fetchWord(desc + 0x08);
    dmaNext[ch] = fetchWord(desc + 0x0c);
    dmaIdx[ch] = 0;
    dmaOn[ch] = 1;
}

/* le matériel écrit le descripteur courant dans la banque WRB à chaque
 * fin de bloc (VALID effacé, BTCNT=0) — les libs pollent cette copie
 * pour savoir si un transfert est fini */
static void dma_wrb_write(uint32_t ch) {
    if (!dmac_wrbAddr || ch >= DMAC_CHANNELS) return;
    uint32_t a = dmac_wrbAddr + ch * 0x10;
    writeHalf(a, (uint16_t)(dmaCtrl[ch] & ~1u));
    writeHalf(a + 2, 0);
    writeWord(a + 4, dmaSrc[ch]);
    writeWord(a + 8, dmaDst[ch]);
    writeWord(a + 0xc, dmaNext[ch]);
}

static void dma_beat(uint32_t ch) {
    if (!dmaOn[ch]) return;
    if (dmaCnt[ch] == 0) { dma_wrb_write(ch); dma_load(ch, dmaNext[ch]); return; }
    uint32_t size = 1u << ((dmaCtrl[ch] >> 8) & 3u);           /* BEATSIZE */
    uint32_t src = dmaSrc[ch], dst = dmaDst[ch];
    if (dmaCtrl[ch] & (1u << 10)) src += (dmaIdx[ch] - dmaCnt[ch]) * size; /* SRCINC */
    if (dmaCtrl[ch] & (1u << 11)) dst += (dmaIdx[ch] - dmaCnt[ch]) * size; /* DSTINC */
    static int dmaDbg = -1;
    if (dmaDbg < 0) dmaDbg = getenv("EMU_DMA_DEBUG") ? 1 : 0;
    if (dmaDbg && (dst == 0x42001828u || dst < 0x20000000u)) /* SPI ou canal égaré */
        fprintf(stderr, "[dma] tick=%u ch=%u beat src=%x dst=%x ctrl=%04x idx=%u cnt=%u\n",
                tickCount, ch, src, dst, dmaCtrl[ch], dmaIdx[ch], dmaCnt[ch]);
    if (dst == 0x42001828u && size == 1) {
        /* beat SPI : n'horloge la carte SD que pour un dummy 0xFF pendant
         * une transaction active — sinon c'est un pixel du display, et le
         * collisionneur viderait la file SD au milieu d'un échange CPU */
        uint8_t b = fetchByte(src);
        int sdClock = b == 0xffu && sd_selected() && (sd_outLen != 0 || sd_cmdIdx != 0);
        dmaBeatSkipSd = !sdClock;
        writeByte(dst, b);
        dmaBeatSkipSd = 0;
        if (sdClock) dma_sercom4_rx_beat(); /* plein-duplex SD */
    } else {
        if (size == 1) writeByte(dst, fetchByte(src));
        else if (size == 2) writeHalf(dst, fetchHalf(src));
        else writeWord(dst, fetchWord(src));
    }
    if (++dmaIdx[ch] >= dmaCnt[ch]) {
        /* TCMPL PAR DESCRIPTEUR : le matériel lève l'interruption à chaque
         * fin de bloc (chaîné ou pas) — les libs comptent les descripteurs
         * libres via les callbacks de l'ISR (dma_desc_free_count) */
        dmacIntFlag[ch] |= 0x02;
        dma_wrb_write(ch);
        dmacInterrupt = 1;
        dma_load(ch, dmaNext[ch]); /* suit la chaîne, ou suspend si invalide */
    }
}

/* La lib récente lit la carte SD via un canal DMA déclenché par
 * SERCOM4_RX (SdSpiGamebuino.cpp : receive() arme rx = SPI.DATA -> buffer,
 * tx = n x 0xFF, puis attend les deux TCMPL).  Chaque beat TX horloge un
 * octet : le beat RX est son miroir plein-duplex (la réponse vient d'être
 * produite dans ser4_data). */
static void dma_sercom4_rx_beat(void) {
    for (uint32_t ch = 0; ch < DMAC_CHANNELS; ch++) {
        /* canaux déclenchés SERCOM4_RX ou armés depuis un descripteur qui
         * lit SPI.DATA (la lib récente prépare le RX en mémoire) */
        if (!dmaOn[ch]) continue;
        if (dmaTrig[ch] != DMAC_TRIG_SERCOM4_RX && dmaSrc[ch] != 0x42001828u) continue;
        if (dmaCnt[ch] == 0) { dma_wrb_write(ch); dma_load(ch, dmaNext[ch]); continue; }
        uint32_t size = 1u << ((dmaCtrl[ch] >> 8) & 3u);
        uint32_t dst = dmaDst[ch];
        if (dmaCtrl[ch] & (1u << 11)) dst += (dmaIdx[ch] - dmaCnt[ch]) * size;
        if (size == 1) writeByte(dst, ser4_data);
        else if (size == 2) writeHalf(dst, ser4_data);
        else writeWord(dst, ser4_data);
        if (++dmaIdx[ch] >= dmaCnt[ch]) {
            dmacIntFlag[ch] |= 0x02;
            dma_wrb_write(ch);
            dmacInterrupt = 1;
            dma_load(ch, dmaNext[ch]);
        }
    }
}

static int dma_tc4_active(void) {
    for (uint32_t ch = 0; ch < DMAC_CHANNELS; ch++)
        if (dmaOn[ch] && dmaTrig[ch] == DMAC_TRIG_TC4_OVF) return 1;
    return 0;
}

static void irq_inject(uint32_t vector);

/* ------------------------------------------------- interruptions */

static void irq_inject(uint32_t vector) {
    /* même motif que le TypeScript : xPSR, PC, LR, r12, r3..r0 */
    uint32_t psr = (fC ? 1 : 0) | (fN ? 2 : 0) | (fV ? 4 : 0) | (fZ ? 8 : 0);
    pushStack(psr);
    pushStack(regs[15]);
    pushStack(regs[14]);
    pushStack(regs[12]);
    pushStack(regs[3]);
    pushStack(regs[2]);
    pushStack(regs[1]);
    pushStack(regs[0]);
    regs[15] = vector;
    regs[14] = 0xfffffff9u;
    incrementPc(); /* pipeline + ticks, comme l'injection du TS */
}

/* ----------------------------------------------------------- mémoire */

#define RD8(a)  ((a) < FLASH_SIZE ? flash[a] : 0)
#define RD8S(a) ((a) < SRAM_SIZE ? sram[a] : 0)

/* PRNG pour ADC RESULT (le TS utilise Math.random ; ADC_FIXED permet de
 * comparer les trajectoires avec le TS piloté au même ADC constant) */
static uint32_t adcPrng = 0x12345678u;
static int adcFixed = -1;
static uint32_t adc_random(void) {
    if (adcFixed < 0) adcFixed = getenv("ADC_FIXED") ? 1 : 0;
    if (adcFixed) return 0x7fff;
    adcPrng ^= adcPrng << 13; adcPrng ^= adcPrng >> 17; adcPrng ^= adcPrng << 5;
    return adcPrng & 0xffffu;
}

/* handlers de lecture périphériques enregistrés (comme le TS : consultés
 * pour les accès mot ET octet, jamais demi-mot).  0 = pas de handler. */
static int dbg(void);
extern int dbgDac;
static uint32_t periph_read(uint32_t a, int *handled) {
    *handled = 0;
    if ((a & ~0x1fu) == 0x41004400u) { *handled = 1; return port_read(0, a & 0x1f); }
    if ((a & ~0x1fu) == 0x41004480u) { *handled = 1; return port_read(1, a & 0x1f); }
    if (a == 0x42001818u || a == 0x42001c18u) { *handled = 1; return 0x07; } /* SERCOM4/5 INTFLAG */
    if (a == 0x42001828u) { *handled = 1;
        if (dbg() && dbgDac < 6000 && ((portA_out >> 27) & 1) == 0) {
            /* cartes sélectionnée seulement : le polling CS-haut noie tout */
            fprintf(stderr, "[dbg] SPI DATA lu <- %02x pc=%x PA27=%d tick=%u\n", ser4_data, regs[15] - 2,
                    (portA_out >> 27) & 1, tickCount);
            dbgDac++;
        }
        return ser4_data; }                /* SERCOM4 DATA */
    if (a == 0x42001c28u) { *handled = 1; return 0x80; }                     /* SERCOM5 DATA */
    if (a == 0x4100484eu) { *handled = 1; return dmacIntFlag[dmac_chid & 0xfu]; } /* DMAC CHINTFLAG (fenêtre CHID) */
    if (a >= 0x4100485eu && a < 0x41004900u && ((a - 0x4100484eu) & 0xfu) == 0) {
        *handled = 1; return dmacIntFlag[(a - 0x4100484eu) >> 4]; } /* CHINTFLAG indexé (canaux 1+) */
    if (a >= 0x4100485du && a < 0x41004900u && ((a - 0x4100484du) & 0xfu) == 0) {
        *handled = 1; return dmacIntEn[(a - 0x4100484du) >> 4]; } /* CHINTENSET indexé */
    if (a == 0x41004820u) { /* INTPEND : premier canal avec un drapeau levé */
        *handled = 1;
        for (uint32_t ch = 0; ch < DMAC_CHANNELS; ch++) {
            if (dmacIntFlag[ch]) {
                return (ch & 0xfu) | ((dmacIntFlag[ch] & 1u) << 4)
                     | (((dmacIntFlag[ch] >> 2) & 1u) << 5)
                     | (((dmacIntFlag[ch] >> 1) & 1u) << 6);
            }
        }
        return 0;
    }
    if (a == 0x4200300du) { *handled = 1; return tc4IntEnMask; }             /* TC4 INTENSET */
    if (a == 0x4200300eu) { *handled = 1; return tc4IntFlagMask; }           /* TC4 INTFLAG */
    if (a == 0x41004840u && dma_is_tc4(dmac_chid)) {                         /* CHCTRLA ENABLE */
        *handled = 1; return dmaOn[dmac_chid] ? 0x02 : 0; }
    return 0;
}

static uint32_t periph_read(uint32_t a, int *handled);
/* alias physique de la flash (0x00400000) : certaines libs y accèdent
 * directement pour leurs données (images embarquées streamees en DMA) */
#define FLASH_PHYS_BASE 0x00400000u

static uint32_t fetchWord(uint32_t a) {
    if (emuTarget == TGT_POKITTO) return pk_read_word(a);
    if (a >= FLASH_PHYS_BASE && a < FLASH_PHYS_BASE + FLASH_SIZE) a -= FLASH_PHYS_BASE;
    if (a < 0x20000000u) { if (a + 4 > FLASH_SIZE) return 0;
        return (uint32_t)flash[a] | ((uint32_t)flash[a+1] << 8) |
               ((uint32_t)flash[a+2] << 16) | ((uint32_t)flash[a+3] << 24); }
    if (a < 0x40000000u) { a -= 0x20000000u; if (a + 4 > SRAM_SIZE) return 0;
        return (uint32_t)sram[a] | ((uint32_t)sram[a+1] << 8) |
               ((uint32_t)sram[a+2] << 16) | ((uint32_t)sram[a+3] << 24); }
    if (a < 0x60000000u) {
        if (a == 0x4000080cu) return 0b11010010;  /* SYSCTRL PCLKSR: tout prêt */
        if (a == 0x4200401au) return adc_random();/* ADC RESULT */
        int handled; return periph_read(a, &handled);
    }
    return 0;
}

static uint16_t fetchHalf(uint32_t a) {
    if (emuTarget == TGT_POKITTO) return pk_read_half(a);
    if (a >= FLASH_PHYS_BASE && a < FLASH_PHYS_BASE + FLASH_SIZE) a -= FLASH_PHYS_BASE;
    if (a < 0x20000000u) { if (a + 2 > FLASH_SIZE) return 0;
        return (uint16_t)(flash[a] | (flash[a+1] << 8)); }
    if (a < 0x40000000u) { a -= 0x20000000u; if (a + 2 > SRAM_SIZE) return 0;
        return (uint16_t)(sram[a] | (sram[a+1] << 8)); }
    /* demi-mot : le TS ne consultait que l'ADC, mais la lib officielle lit
     * DMAC INTPEND en ldrh (0x41004820) pour retrouver le canal à service —
     * les périphériques passent donc par periph_read aussi */
    if (a >= 0x40000000u && a < 0x60000000u) {
        if (a == 0x4200401au) return (uint16_t)adc_random();
        int handled; uint32_t v = periph_read(a & ~1u, &handled);
        return (uint16_t)v;
    }
    return 0;
}

static uint8_t fetchByte(uint32_t a) {
    if (emuTarget == TGT_POKITTO) return pk_read_byte(a);
    if (a >= FLASH_PHYS_BASE && a < FLASH_PHYS_BASE + FLASH_SIZE) a -= FLASH_PHYS_BASE;
    if (a < 0x20000000u) return RD8(a);
    if (a < 0x40000000u) return RD8S(a - 0x20000000u);
    if (a == 0x40000c00u) return 0;           /* GCLK CTRL : pas de reset en cours */
    if (a == 0x42004018u) return 1;           /* ADC INTFLAG RESRDY */
    int handled;
    uint32_t v = periph_read(a, &handled);
    if (handled) return (uint8_t)v;
    return 0;
}

static void writeWord(uint32_t a, uint32_t v);
static void writeHalf(uint32_t a, uint16_t v);
static void writeByte(uint32_t a, uint8_t v);

int dbgDac = 0;
static int dbgTc4Cfg = 0;
static int dbgEnabled = -1; /* EMU_DEBUG=1 : traces de config périphériques */
static int dbg(void) {
    if (dbgEnabled < 0) dbgEnabled = getenv("EMU_DEBUG") ? 1 : 0;
    return dbgEnabled;
}
static uint32_t millisWatchAddr = 0x20001fbcu; /* surchargé par MILLIS_WATCH */
static long millisWrites;
/* montre générique d'écriture (WATCH_ADDR), pour le débogage */
static long usbWatch;
static uint32_t watchAddr = 0;
static uint32_t prevInstPc;  /* PC à l'entrée du pas courant (la boucle le tient à jour) */
/* programmation flash (auto-patch des loaders) : effet net du NVMCTRL —
 * la valeur écrite est stockée en flash et le code fraîchement écrit est
 * exécuté aux pas suivants.  Les commandes du contrôleur (0x41004000+)
 * restent des no-ops : sur circuit le jeu prépare ses pages puis les
 * programme ; ici seule l'écriture effective compte.  L'alias physique
 * 0x00400000 est accepté en écriture aussi.
 *
 * Build wasm : écritures simplement IGNORÉES (comportement d'avant) — la
 * distribution web privilégie le démarrage partout ; les écrans de boot
 * sont identiques, seuls les auto-patchs des loaders restent inertes. */
#ifdef __EMSCRIPTEN__
static void flash_store(uint32_t a, uint32_t v, int bytes) {
    (void)a; (void)v; (void)bytes;
}
#else
static void flash_store(uint32_t a, uint32_t v, int bytes) {
    if (a >= FLASH_PHYS_BASE) a -= FLASH_PHYS_BASE;
    if (a + (uint32_t)bytes > FLASH_SIZE) return;
    static int nvmDbg = -1;
    if (nvmDbg < 0) nvmDbg = getenv("NVM_DEBUG") ? 1 : 0;
    if (nvmDbg) {
        static int n; if (n < 300) fprintf(stderr, "[nvm] tick=%u pc=%x flash[%x] <- %0*x (%d o)\n",
                                          tickCount, prevInstPc, a, bytes * 2,
                                          v & ((1u << (bytes * 8)) - 1), bytes);
        n++;
    }
    uint8_t *p = flash + a;
    if (bytes == 4) {
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    } else if (bytes == 2) {
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    } else p[0] = (uint8_t)v;
}
#endif /* __EMSCRIPTEN__ */

static void writeWord(uint32_t a, uint32_t v) {
    if (emuTarget == TGT_POKITTO) { pk_write_word(a, v); return; }
    if (a == millisWatchAddr) millisWrites++;
    if (watchAddr && a == watchAddr && usbWatch < 4000)
        fprintf(stderr, "[watch %x] tick=%u pc=%x val=%08x\n", a, tickCount, regs[15] - 2, v);
    if (a < 0x20000000u) { flash_store(a, v, 4); return; }
    if (a < 0x40000000u) { a -= 0x20000000u; if (a + 4 > SRAM_SIZE) return;
        sram[a] = v & 0xff; sram[a+1] = (v >> 8) & 0xff;
        sram[a+2] = (v >> 16) & 0xff; sram[a+3] = (v >> 24) & 0xff; return; }
    if ((a & ~0x1fu) == 0x41004400u) { port_write(0, a & 0x1f, v); return; }
    if ((a & ~0x1fu) == 0x41004480u) { port_write(1, a & 0x1f, v); return; }
    if (a == 0x42001828u) { sercom4_write((uint8_t)v); return; } /* SERCOM4 DATA */
    if (a == 0x42004808u) { dac_write((uint16_t)v); return; }    /* DAC DATA */
    if (a == 0x42003000u) { if (dbg() && dbgTc4Cfg < 8) { fprintf(stderr, "[dbg] CTRLA word <- %x\n", v); dbgTc4Cfg++; } tc4CtrlA = v; tc4Enabled = (v & 0x02) != 0; if (!tc4Enabled) tc4Counter = 0; return; }
    if (a == 0x42003018u) { tc4Top = v; return; }                /* TC4 CC0 */
    if (a == 0x4200300cu) { tc4IntEnMask &= (uint8_t)~v; tc4Armed = (tc4IntEnMask & 0x33) != 0; return; } /* TC4 INTENCLR */
    if (a == 0x4200300du) { tc4IntEnMask |= (uint8_t)v; tc4Armed = (tc4IntEnMask & 0x33) != 0; return; } /* TC4 INTENSET : OVF=0x01 (jeux maison), MC0=0x10 (lib standard) */
    if (a == 0x4200300eu) { tc4IntFlagMask &= (uint8_t)~v; return; } /* TC4 INTFLAG (acquittement) */
    /* GCLK GENDIV/GENCTRL : sans effet (la cadence TC4 est dérivée de
     * CTRLA/CC0, et la fréquence du générateur vaut 48 MHz pour les
     * configurations audio rencontrées — lib standard comme jeux maison) */
    if (a == 0x40000c04u || a == 0x40000c08u) return;
    if (a == 0x41004834u) { dmac_baseAddr = v; return; }
    if (a == 0x41004838u) { dmac_wrbAddr = v; return; }
    if (a == 0x4100483fu) { dmac_chid = v; return; }
    /* la lib officielle adresse les canaux en INDEXÉ — 16 octets par canal :
     * Channel[n] = 0x41004840 + n*16, CHCTRLA@+0, CHCTRLB@+4, CHINTENCLR@+C,
     * CHINTENSET@+D, CHINTFLAG@+E.  Les canaux 1+ (0x50+) arrivent ici ; les
     * adresses 0x40-0x4F sont la fenêtre CHID (= canal 0 pour la lib, qui
     * n'écrit jamais CHID) et passent par les handlers CHID ci-dessous. */
    if (a >= 0x41004850u && a < 0x41004900u) {
        uint32_t ch = (a - 0x41004840u) >> 4;
        uint32_t off = (a - 0x41004840u) & 0xfu;
        if (ch < DMAC_CHANNELS && (off == 0 || off == 4 || (off >= 0xc && off <= 0xe))) {
            if (off == 0) { /* CHCTRLA */
                if ((v & 0x03u) == 0x02u) {
                    uint32_t desc = dmac_baseAddr ? dmac_baseAddr + ch * 0x10 : 0;
                    if (desc) dma_load(ch, desc);
                } else dmaOn[ch] = 0; /* SWRST / disable */
                return;
            }
            if (off == 4) { dmaTrig[ch] = (uint8_t)((v >> 8) & 0x3fu); return; } /* CHCTRLB.TRIGSRC */
            if (off == 0xc) { dmacIntEn[ch] = (uint8_t)~v; return; }  /* CHINTENCLR */
            if (off == 0xd) { dmacIntEn[ch] = (uint8_t)v; return; }   /* CHINTENSET */
            if (off == 0xe) {
                dmacIntFlag[ch] = 0; /* CHINTFLAG (acquitter) */
                for (uint32_t k = 0; k < DMAC_CHANNELS; k++)
                    if (k != ch && dmacIntFlag[k]) { dmacInterrupt = 1; break; }
                return;
            }
        }
        return;
    }
    if (a == 0x41004844u && dmac_chid < DMAC_CHANNELS) { /* CHCTRLB */
        /* comme le TS : seul TRIGSRC est retenu ; CMD (RESUME/retigger) est
         * ignoré — le transfert part de l'écriture CHCTRLA=ENABLE, le
         * curseur de descripteurs tournant (anneau de la lib) fait le reste */
        dmaTrig[dmac_chid] = (uint8_t)((v >> 8) & 0x3fu);
        return;
    }
    if (a == 0x4100484eu && dmac_chid < DMAC_CHANNELS) { /* CHINTFLAG : acquittement */
        dmacIntFlag[dmac_chid] &= (uint8_t)~v;
        /* l'interruption DMAC est level-triggered sur le hardware : tant
         * qu'un autre canal attend, le NVIC ré-entre dans le handler — la
         * lib ne lit INTPEND qu'une fois par entrée */
        for (uint32_t k = 0; k < DMAC_CHANNELS; k++)
            if (k != dmac_chid && dmacIntFlag[k]) { dmacInterrupt = 1; break; }
        return;
    }
    if (a == 0x41004840u && dma_is_tc4(dmac_chid)) { /* canal audio (TC4) */
        if ((v & 0x03u) == 0x02u) { if (!dmaOn[dmac_chid]) dma_load(dmac_chid, dmac_baseAddr + dmac_chid * 0x10); }
        else dmaOn[dmac_chid] = 0; /* SWRST ou désactivation */
        return;
    }
    if (a == 0x41004840u && (v & 0x03u) != 0x02u && spiDmaCh == (int)dmac_chid) {
        spiDmaCh = -1; dmaOn[dmac_chid] = 0; /* SWRST/désactivation du canal SPI */
        return;
    }
    if (a == 0x41004840u && (v & 0x03u) != 0x02u && dmaTrig[dmac_chid & 0xfu] == DMAC_TRIG_SERCOM4_RX) {
        dmaOn[dmac_chid & 0xfu] = 0; /* désactivation du canal RX SD */
        return;
    }
    if (a == 0x41004840u) { /* CHCTRLA == 2 : transfert via descripteur */
        if (v == 0x02) {
            if (!dmac_desc) dmac_desc = dmac_baseAddr + dmac_chid * 0x10;
            if (dmaTrig[dmac_chid] == DMAC_TRIG_SERCOM4_RX) {
                /* réception SD : armé seulement — les beats arrivent au
                 * rythme des octets envoyés (miroir plein-duplex), une
                 * copie instantanée lirait ici 512x le même octet */
                dma_load(dmac_chid, dmac_desc);
                dmac_desc = 0;
                return;
            }
            uint16_t pctrl = fetchHalf(dmac_desc);
            uint32_t pdst = fetchWord(dmac_desc + 0x08);
            if (dmaTrig[dmac_chid] == DMAC_TRIG_SERCOM4_TX || pdst == 0x42001828u) {
                /* écran : beats cadencés par le baud SPI, le CPU vit pendant */
                dma_load(dmac_chid, dmac_desc);
                if (dmaOn[dmac_chid]) {
                    uint32_t b = (spiBaud & 0xFFu) + 1;
                    spiBeatTicks = (b * 20u + 1u) / 3u; /* 8 bits @ f/2(1+b), 20000 ticks/ms */
                    if (spiBeatTicks < 1) spiBeatTicks = 1;
                    spiBeatAcc = 0;
                    spiDmaCh = (int)dmac_chid;
                    { static int n; if (n++ < 6)
                        fprintf(stderr, "[dma spi] ch=%u ctrl=%04x src=%x dst=%x n=%u beats=%u t\n",
                                (unsigned)dmac_chid, dmaCtrl[dmac_chid], dmaSrc[dmac_chid],
                                dmaDst[dmac_chid], dmaCnt[dmac_chid], spiBeatTicks); }
                } else {
                    static int n; if (n++ < 4)
                        fprintf(stderr, "[dma spi] ch=%u : descripteur INVALIDE\n", (unsigned)dmac_chid);
                }
                dmac_desc = 0;
                return;
            }
            uint16_t ctrl = fetchHalf(dmac_desc);
            uint16_t btcnt = fetchHalf(dmac_desc + 0x02);
            uint32_t src = fetchWord(dmac_desc + 0x04);
            uint32_t dst = fetchWord(dmac_desc + 0x08);
            uint32_t nxt = fetchWord(dmac_desc + 0x0c);
            /* SAMD21 : SRCADDR/DSTADDR sont des adresses de FIN ; les
             * drapeaux SRCINC/DSTINC disent si l'on remonte.  BEATSIZE
             * (octet/demi/mot) respecté — memset32 du jeu (beats mot,
             * source fixe) sinon n'écrit que 4 octets. */
            uint32_t size = 1u << ((ctrl >> 8) & 3u);
            int srcinc = ctrl & (1u << 10), dstinc = ctrl & (1u << 11);
            if (getenv("EMU_DMA_DEBUG") && btcnt > 100) {
                unsigned h = 0x811c9dc5;
                for (uint32_t k = 0; k < btcnt; k++) {
                    uint32_t s = srcinc ? src + (k - btcnt) * size : src;
                    if (size == 1) h = (h ^ fetchByte(s)) * 0x01000193u;
                    else if (size == 2) h = (h ^ fetchHalf(s)) * 0x01000193u;
                    else h = (h ^ fetchWord(s)) * 0x01000193u;
                }
                fprintf(stderr, "[dma] ch=%u src=%x dst=%x n=%u sz=%u lcd_y=%u tick=%u hash=%08x\n",
                        (unsigned)dmac_chid, src, dst, btcnt, size, lcd_y, tickCount, h);
            }
            for (uint16_t i = 0; i < btcnt; i++) {
                uint32_t s = srcinc ? src + (i - btcnt) * size : src;
                uint32_t d = dstinc ? dst + (i - btcnt) * size : dst;
                if (size == 1) writeByte(d, fetchByte(s));
                else if (size == 2) writeHalf(d, fetchHalf(s));
                else writeWord(d, fetchWord(s));
            }
            dma_wrb_write(dmac_chid);
            dmac_desc = nxt;
            dmacInterrupt = 1; /* verrouillé, traité au prochain step (TS) */
        }
        return;
    }
}

static void writeHalf(uint32_t a, uint16_t v) {
    if (emuTarget == TGT_POKITTO) { pk_write_half(a, v); return; }
    if (a < 0x20000000u) { flash_store(a, v, 2); return; }
    if (watchAddr && a == watchAddr && usbWatch < 4000)
        fprintf(stderr, "[watch-h %x] tick=%u pc=%x val=%04x\n", a, tickCount, prevInstPc, v);
    if (a < 0x40000000u) { a -= 0x20000000u; if (a + 2 > SRAM_SIZE) return;
        sram[a] = v & 0xff; sram[a+1] = (v >> 8) & 0xff; return; }
    if (a == 0x42004808u) { if (dbg() && dbgDac < 3) { fprintf(stderr, "[dbg] DAC half <- %x\n", v); dbgDac++; } dac_write(v); return; }
    if (a == 0x42003000u) { if (dbg() && dbgTc4Cfg < 8) { fprintf(stderr, "[dbg] CTRLA half <- %x\n", v); dbgTc4Cfg++; } tc4CtrlA = v; tc4Enabled = (v & 0x02) != 0; if (!tc4Enabled) tc4Counter = 0; return; }
    if (a == 0x42003018u) { tc4Top = v; return; }
    if (a == 0x4200300du) { tc4IntEnMask |= (uint8_t)v; tc4Armed = (tc4IntEnMask & 0x33) != 0; return; }
    if (a == 0x4200300eu) { tc4IntFlagMask &= (uint8_t)~v; return; }
    if (a == 0x40000c02u) { if (dbg() && dbgTc4Cfg < 16) { fprintf(stderr, "[gclk] CLKCTRL <- %x\n", v); dbgTc4Cfg++; } return; }
    if (a == 0x40000c04u) { fprintf(stderr, "[gclk] GENDIV <- %x\n", v); return; }
    if (a == 0x40000c08u) { fprintf(stderr, "[gclk] GENCTRL <- %x\n", v); return; }
    if ((a & ~0x1fu) == 0x41004400u) { port_write(0, a & 0x1f, v); return; }
    if ((a & ~0x1fu) == 0x41004480u) { port_write(1, a & 0x1f, v); return; }
    writeWord(a, v);
}

static void writeByte(uint32_t a, uint8_t v) {
    if (emuTarget == TGT_POKITTO) { pk_write_byte(a, v); return; }
    if (a < 0x20000000u) { flash_store(a, v, 1); return; }
    if (a < 0x40000000u) { uint32_t sa = a - 0x20000000u; if (sa < SRAM_SIZE) sram[sa] = v; return; }
    if (a == 0x4200300du) { tc4IntEnMask |= (uint8_t)v; tc4Armed = (tc4IntEnMask & 0x33) != 0; return; } /* TC4 INTENSET */
    if (a == 0x4200300eu) { tc4IntFlagMask &= (uint8_t)~v; return; } /* TC4 INTFLAG */
    if (a == 0x42001828u) { sercom4_write(v); return; }
    if (a == 0x4200180au) { spiBaud = v; return; } /* SERCOM4 BAUD (SPI) */
    if (a == 0x4100484eu) { /* DMAC CHINTFLAG acquittement (fenêtre, octet) */
        dmacIntFlag[dmac_chid & 0xfu] &= (uint8_t)~v;
        for (uint32_t k = 0; k < DMAC_CHANNELS; k++)
            if (k != (dmac_chid & 0xfu) && dmacIntFlag[k]) { dmacInterrupt = 1; break; }
        return;
    }
    if (a == 0x4100483fu) { dmac_chid = v; return; }
    if ((a & ~0x1fu) == 0x41004400u) { port_write(0, a & 0x1f, v); return; }
    if ((a & ~0x1fu) == 0x41004480u) { port_write(1, a & 0x1f, v); return; }
    writeWord(a, v);
}

/* --------------------------------------------------- DMAC (écran) */


/* ---------------------------------------------------- SERCOM4 data */

static void buttons_apply(void);

static uint8_t serLast[8]; static long serIdx; static long serNz;
static void sercom4_write(uint8_t v) {
    if (v) serNz++;
    if (getenv("EMU_DMA_DEBUG")) { serLast[serIdx++ & 7] = v; }
    /* ordre du TypeScript (écran, boutons, carte SD) ; l'octet de réponse
     * repart à 0x80 à chaque échange, les périphériques sélectionnés le
     * remplacent (sercom-register.ts : this.data = 0x80 puis listeners).
     * Boutons : PB03 (lib standard) ; PA25 accepté aussi — même registre à
     * décalage sur le bus, certains jeux maison le pilotent en direct. */
    ser4_data = 0x80;
    st7735_byte(v);
    if ((portB_out & (1u << 3)) == 0 || (portA_out & (1u << 25)) == 0)
        ser4_data = buttonData; /* boutons : PB03 (lib standard) ou PA25
                                 * (certains jeux maison pilotent ce CS en
                                 * direct — même registre à décalage) */
    if (!dmaBeatSkipSd) sd_byte(v);
}

/* ---------------------------------------------------------------- CPU */

static void incrementPc(void) {
    sysTickTrigger++;
    tickCount++;
    regs[15] += 2;

    if (emuTarget == TGT_POKITTO) return; /* timers LPC dans pk_machine_step */
    if (tc4Enabled && tc4Top > 0 && !tc4Armed && dma_tc4_active()) {
        /* TC4 sans interruption, lu par DMA : cadence fixe, 1 ms = 20000
         * ticks (SysTick émulé), donc 907 ticks par échantillon à 22 kHz */
        if (++tc4Counter >= 907u) {
            tc4Counter = 0;
            tc4Fires++;
            for (uint32_t ch = 0; ch < DMAC_CHANNELS; ch++)
                if (dmaTrig[ch] == DMAC_TRIG_TC4_OVF) dma_beat(ch);
        }
    } else if (tc4Enabled && tc4Armed && tc4Top > 0) {
        /* cadence dérivée de la vraie config (MFRQ) : F = GCLK_TC4 /
         * (prescale × (CC0+1)) ; GCLK audio = 48 MHz (lib standard et jeux
         * maison), domaine de ticks de l'ému : 20000/ms → période =
         * prescale × (CC0+1) × 20000/48 = ×5/12.  L'ancien gouverneur
         * heuristique du TS comprimait la période quand le jeu sert chaque
         * interruption immédiatement (son qui accélère à l'infini). */
        static const uint16_t prescTab[8] = {1, 2, 4, 8, 16, 64, 256, 1024};
        uint32_t cfg = (uint32_t)prescTab[(tc4CtrlA >> 8) & 7u] * (tc4Top + 1) * 5u / 12u;
        tc4Period = cfg < 2 ? 2 : cfg;
        if (++tc4Counter >= tc4Period) {
            tc4Counter = 0;
            tc4Fires++;
            tc4IntFlagMask |= tc4IntEnMask & 0x11u; /* OVF/MC0 posés, lus par le handler */
            tc4Interrupt = 1;
        }
    }
    if (spiDmaCh >= 0) { /* beats SPI : un octet tous les spiBeatTicks */
        if (!dmaOn[spiDmaCh]) { spiDmaCh = -1; }
        else if (++spiBeatAcc >= spiBeatTicks) {
            spiBeatAcc = 0;
            dma_beat(spiDmaCh);
            if (!dmaOn[spiDmaCh]) { /* bloc terminé : TCMPL */
                spiDmaCh = -1;
                dmacInterrupt = 1;
            }
        }
    }
}

/* (traceAllStep est local à step) */
static uint32_t traceFrom = 0;
static void pushStack(uint32_t v) { regs[13] -= 4; writeWord(regs[13], v); }
static uint32_t popStack(void) { uint32_t v = fetchWord(regs[13]); regs[13] += 4; return v; }

/* hachage d'état pour comparaison avec le TS (FNV-1a) */
static uint32_t state_hash(void) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 16; i++) { h = (h ^ regs[i]) * 0x01000193u; }
    /* 32 Ko seulement : comparable au harnais TS (la SRAM étendue au-delà
     * de 0x8000 ne sert qu'aux buffers SD des libs récentes) */
    for (uint32_t i = 0; i < 0x8000u; i++) { h = (h ^ sram[i]) * 0x01000193u; }
    if (emuTarget == TGT_POKITTO) {
        for (size_t i = 0; i < sizeof pk_sram1; i++) { h = (h ^ pk_sram1[i]) * 0x01000193u; }
        for (size_t i = 0; i < sizeof pk_usbsram; i++) { h = (h ^ pk_usbsram[i]) * 0x01000193u; }
    }
    return h;
}
static int hashMode = -1;
static uint32_t hashInterval = 250000;
static uint32_t ihash; /* hachage par instruction (EMU_IHASH / WASM_DEBUG) */
static int ihashOn;
static uint32_t sramDumpAt = 0;

static void setReg(int i, uint32_t v) { regs[i] = v; regD[i] = (double)v; }
static void setNZ(uint32_t r) { fN = (r & 0x80000000u) != 0; fZ = r == 0; }

static uint32_t addSetCond(uint32_t a, uint32_t b, int carry) {
    /* retenue sur la somme 64 bits : `r < a` après troncature donnait
     * C=0 pour toute soustraction sans emprunt (cmp x, x) — le TS
     * compare result > 0xffffffff AVANT troncature */
    uint64_t r64 = (uint64_t)a + b + (unsigned)carry;
    uint32_t r = (uint32_t)r64;
    fC = (r64 >> 32) != 0;
    fV = (~(a ^ b) & (a ^ r) & 0x80000000u) != 0;
    setNZ(r);
    return r;
}

static void irq_inject(uint32_t vector);

static uint32_t lastExecPc;  /* PC de l'instruction exécutée au pas précédent */

/* TRACE_TAIL=<n> : tampon circulaire des n dernières lignes de trace ;
 * déversé quand un PC fou est détecté (le tick du crash varie selon les
 * runs) — TRACE_TAIL_OUT=<fichier>, sinon stderr */
#define TRACE_TAIL_LINES 8192
static char *traceTail[TRACE_TAIL_LINES];
static unsigned traceTailCount, traceTailNext, traceTailLen;
static int traceTailOn = -1;

static void trace_tail_init(void) {
    const char *e = getenv("TRACE_TAIL");
    traceTailOn = e ? atoi(e) : 0;
    if (traceTailOn > TRACE_TAIL_LINES) traceTailOn = TRACE_TAIL_LINES;
}

static void trace_tail_push(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    free(traceTail[traceTailNext]);
    traceTail[traceTailNext] = strdup(buf);
    traceTailNext = (traceTailNext + 1) % (unsigned)traceTailOn;
    if (traceTailLen < (unsigned)traceTailOn) traceTailLen++;
    traceTailCount++;
}

static void trace_tail_dump(void) {
    FILE *f = stderr;
    const char *out = getenv("TRACE_TAIL_OUT");
    if (out) { f = fopen(out, "w"); if (!f) f = stderr; }
    fprintf(f, "--- trace_tail (%u lignes) ---\n", traceTailLen);
    for (unsigned i = 0; i < traceTailLen; i++) {
        unsigned idx = (traceTailNext + (unsigned)traceTailOn - traceTailLen + i) % (unsigned)traceTailOn;
        if (traceTail[idx]) fputs(traceTail[idx], f), fputc('\n', f);
    }
    if (f != stderr) fclose(f);
}

static void step(void) {
    lastExecPc = prevInstPc;
    prevInstPc = regs[15] - 2;
    /* trace instruction par instruction (comparaison TS) : avant toute
     * injection, état = ce qui va s'exécuter */
    static long stepNo = 0;
    static int traceAllOn = -1;
    if (traceAllOn < 0) traceAllOn = getenv("TRACE_ALL") ? 1 : 0;
    int traceAllStep = traceAllOn && tickCount >= traceFrom;
    if (traceAllStep)
        fprintf(stderr, "%ld %u %x %04x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n", stepNo, tickCount,
                regs[15] - 2, fetchHalf(regs[15] - 2), regs[13],
                regs[0], regs[1], regs[2], regs[3],
                regs[4], regs[5], regs[6], regs[7],
                regs[8], regs[9], regs[10], regs[11], regs[12], regs[14]);
    if (traceTailOn > 0) /* initialisation paresseuse au premier appel */
        trace_tail_push("%ld %u %x %04x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x", stepNo, tickCount,
                regs[15] - 2, fetchHalf(regs[15] - 2), regs[13],
                regs[0], regs[1], regs[2], regs[3],
                regs[4], regs[5], regs[6], regs[7],
                regs[8], regs[9], regs[10], regs[11], regs[12], regs[14]);
    else if (traceTailOn < 0)
        trace_tail_init();
    stepNo++;
    if (emuTarget == TGT_POKITTO) {
        /* SysTick, CT32B0/1, broches et SYSRESETREQ (ordre de la référence) */
        pk_machine_step();
    } else {
        if (tc4Interrupt) {
            tc4Interrupt = 0;
            irq_inject(tc4Vector);
        }
        if (dmacInterrupt) {
            dmacInterrupt = 0;
            irq_inject(dmacVector);
        }
        /* SysTick indépendant du DMAC : la chaîne `else if` du TS affamait
         * Millis dès qu'une lib streame l'écran en DMA continu (dmacInterrupt
         * posé à chaque pas) — gb.update() ne s'ouvrait plus jamais */
        if (sysTickTrigger >= 20000) { /* 1 ms émulée (hack du TS) */
            sysTickTrigger = 0;
            sysTickEntries++;
            irq_inject(sysTickVector);
        }
    }

    uint32_t instAddr = regs[15] - 2;
    /* fin d'interruption : BX LR avec EXC_RETURN (0xfffffff9) fait atterrir
     * le PC sur 0xfffffff8 -> dépiler r0-r3, r12, lr, pc puis xPSR */
    while (instAddr == 0xfffffff8u) {
        setReg(0, popStack());
        setReg(1, popStack());
        setReg(2, popStack());
        setReg(3, popStack());
        setReg(12, popStack());
        setReg(14, popStack());
        setReg(15, popStack());
        uint32_t psrWord = fetchWord(regs[13]);
        if (emuTarget == TGT_POKITTO) { /* xPSR aux positions ARM réelles */
            fN = (psrWord >> 31) & 1;
            fZ = (psrWord >> 30) & 1;
            fC = (psrWord >> 29) & 1;
            fV = (psrWord >> 28) & 1;
            armIrqEnable = 1;
        } else { /* encodage TS (C,N,V,Z dans les bits 0..3) */
            fC = (psrWord & 1) != 0; fN = (psrWord & 2) != 0;
            fV = (psrWord & 4) != 0; fZ = (psrWord & 8) != 0;
        }
        regs[13] += 4;
        instAddr = regs[15] - 2;
    }
    if (emuTarget == TGT_POKITTO && instAddr >= 0x00040000u &&
        instAddr < 0x10000000u) { /* exécution hors flash : vecteur de reset */
        static int pkWildLogged = 0;
        if (pkWildLogged < 10)
            fprintf(stderr, "[pc fou pk] tick=%u pc=%x lr=%x sp=%x\n",
                    tickCount, instAddr, regs[14], regs[13]);
        pkWildLogged++;
        regs[15] = pk_read_word(4) & ~1u;
        return;
    }
    if (emuTarget == TGT_META && instAddr >= 0x42000000u) { /* PC fou : le TS planterait ici — log fort */
        static int wildLogged = 0;
        if (wildLogged < 3)
            fprintf(stderr, "[pc fou] tick=%u pc=%x lr=%x sp=%x\n",
                    tickCount, instAddr, regs[14], regs[13]);
        wildLogged++;
        if (wildLogged == 1) trace_tail_dump();
        tickCount += 2; /* pas d'instruction exécutée : avance artificielle,
                         * sinon les interruptions se figent avec le tick */
        regs[15] = vectorBase + fetchWord(vectorBase + 4);
        return;
    }
    /* PC hors flash (et hors SRAM exécutable) : log de diagnostic avec le PC
     * de l'instruction précédente — WILD_RESET=1 reprend sur le vecteur de
     * reset au lieu d'exécuter les mauvaises herbes (comportement TS = non) */
    if (emuTarget == TGT_META && instAddr >= 0x00040000u &&
        !(instAddr >= 0x20000000u && instAddr < 0x20000000u + SRAM_SIZE)) {
        static int wildFlashLogged = 0;
        static int wildReset = -1;
        if (wildReset < 0) wildReset = getenv("WILD_RESET") ? 1 : 0;
        if (wildFlashLogged < 3)
            fprintf(stderr, "[pc fou flash] tick=%u pc=%x prev=%x lr=%x sp=%x r0=%x r1=%x r2=%x r3=%x\n",
                    tickCount, instAddr, lastExecPc, regs[14], regs[13],
                    regs[0], regs[1], regs[2], regs[3]);
        wildFlashLogged++;
        if (wildFlashLogged == 1) trace_tail_dump();
        tickCount += 2;
        if (wildReset) { regs[15] = vectorBase + fetchWord(vectorBase + 4); return; }
    }
    uint16_t inst = fetchHalf(instAddr);
    static int trace = -1;
    if (trace < 0) trace = getenv("EMU_TRACE") ? 1 : 0;
    if (trace && (tickCount & 0x3ffff) == 0)
        fprintf(stderr, "[tick %u] pc=0x%x inst=%04x r0=%08x sp=%08x\n",
                tickCount, instAddr, inst, regs[0], regs[13]);
    incrementPc();

    uint32_t op = inst;

    /* déplacements décalés + add/sub registre/imm3 */
    if ((op & 0xe000) == 0x0000) {
        int rs = (op >> 3) & 7, rd = op & 7;
        if ((op & 0x1800) != 0x1800) {
            int opc = (op >> 11) & 3, off = (op >> 6) & 0x1f;
            if (opc == 0) { /* LSL imm */
                uint32_t v = regs[rs];
                if (off) {
                    uint32_t partial = v << (off - 1);
                    setReg(rd, partial << 1);
                    fC = (partial & 0x80000000u) != 0;
                } else { setReg(rd, v); }
                setNZ(regs[rd]);
            } else if (opc == 1) { /* LSR imm */
                uint32_t n = off ? off : 32, v = regs[rs];
                uint32_t partial = v >> (n - 1);
                setReg(rd, partial >> 1);
                fC = (partial & 1) != 0;
                setNZ(regs[rd]);
            } else if (opc == 2) { /* ASR imm */
                uint32_t n = off ? off : 32;
                int32_t sv = (int32_t)regs[rs];
                int32_t partial = sv >> (int)(n - 1);
                setReg(rd, (uint32_t)(partial >> 1));
                fC = (partial & 1) != 0;
                setNZ(regs[rd]);
            }
        } else { /* add/subtract */
            int opc = (op >> 9) & 3, rn = (op >> 6) & 7;
            if (opc == 0) setReg(rd, addSetCond(regs[rs], regs[rn], 0));
            else if (opc == 1) setReg(rd, addSetCond(regs[rs], ~regs[rn], 1));
            else if (opc == 2) setReg(rd, addSetCond(regs[rs], rn, 0));
            else setReg(rd, addSetCond(regs[rs], ~((uint32_t)rn), 1));
        }
    }
    /* mov/cmp/add/sub immédiat */
    else if ((op & 0xe000) == 0x2000) {
        int rd = (op >> 8) & 7, opc = (op >> 11) & 3;
        uint32_t imm = op & 0xff;
        if (opc == 0) { setReg(rd, imm); setNZ(imm); }
        else if (opc == 1) addSetCond(regs[rd], ~imm, 1);
        else if (opc == 2) setReg(rd, addSetCond(regs[rd], imm, 0));
        else setReg(rd, addSetCond(regs[rd], ~imm, 1));
    }
    /* opérations ALU */
    else if ((op & 0xfc00) == 0x4000) {
        int opc = (op >> 6) & 0xf, rs = (op >> 3) & 7, rd = op & 7;
        uint32_t a = regs[rd], b = regs[rs];
        switch (opc) {
            case 0x0: setReg(rd, a & b); setNZ(regs[rd]); break;
            case 0x1: setReg(rd, a ^ b); setNZ(regs[rd]); break;
            /* décalages par registre : la META garde le quirk TS (retenue
             * calculée sur le COMPTEUR, comme le code JS) ; la Pokitto suit
             * la sémantique ARM réelle (référence C++). */
            case 0x2: if (emuTarget == TGT_POKITTO) {
                        uint32_t sh = b & 0xff;
                        if (sh) {
                            if (sh == 32) { fC = (a & 1) != 0; setReg(rd, 0); }
                            else if (sh < 32) { fC = ((a >> (32 - sh)) & 1) != 0; setReg(rd, a << sh); }
                            else { fC = 0; setReg(rd, 0); }
                        }
                        setNZ(regs[rd]);
                      } else {
                        uint32_t r = a << (b & 31); setReg(rd, r);
                        fC = (b & (1u << (b & 31))) != 0; setNZ(r);
                      }
                      break;
            case 0x3: if (emuTarget == TGT_POKITTO) {
                        uint32_t sh = b & 0xff;
                        if (sh) {
                            if (sh == 32) { fC = (a >> 31) & 1; setReg(rd, 0); }
                            else if (sh < 32) { fC = ((a >> (sh - 1)) & 1) != 0; setReg(rd, a >> sh); }
                            else { fC = 0; setReg(rd, 0); }
                        }
                        setNZ(regs[rd]);
                      } else {
                        uint32_t r = a >> (b & 31); setReg(rd, r);
                        fC = (b & (1u << ((32 - b) & 31))) != 0; setNZ(r);
                      }
                      break;
            case 0x4: if (emuTarget == TGT_POKITTO) {
                        uint32_t sh = b & 0xff;
                        if (sh < 32) {
                            fC = ((((int32_t)a) >> (int)(sh - 1)) & 1) != 0;
                            setReg(rd, (uint32_t)(((int32_t)a) >> (int)sh));
                        } else {
                            if (a & 0x80000000u) { setReg(rd, 0xFFFFFFFFu); fC = 1; }
                            else { setReg(rd, 0); fC = 0; }
                        }
                        setNZ(regs[rd]);
                      } else {
                        uint32_t r = (uint32_t)((int32_t)a >> (b & 31));
                        setReg(rd, r);
                        fC = (b & (1u << ((32 - b) & 31))) != 0; setNZ(r);
                      }
                      break;
            case 0x7: /* ROR : no-op côté META (non décodé par le TS) ;
                       * sémantique ARM pour la Pokitto */
                      if (emuTarget == TGT_POKITTO) {
                        uint32_t sh = b & 0x1f;
                        if (sh) {
                            fC = ((a >> (sh - 1)) & 1) != 0;
                            setReg(rd, (a >> sh) | (a << (32 - sh)));
                        } else if (b & 0xff) {
                            fC = (a >> 31) & 1;
                        }
                        setNZ(regs[rd]);
                      }
                      break;
            case 0x5: setReg(rd, addSetCond(a, b, fC)); break;                 /* ADC */
            case 0x6: setReg(rd, addSetCond(a, ~b, fC)); break;                /* SBC */
            case 0x8: setNZ(a & b); break;                                     /* TST */
            case 0x9: setReg(rd, addSetCond(0, ~b, 1)); break;                 /* NEG */
            case 0xa: addSetCond(a, ~b, 1); break;                             /* CMP reg */
            case 0xb: addSetCond(a, b, 0); break;                              /* CMN */
            case 0xc: setReg(rd, a | b); setNZ(regs[rd]); break;               /* ORR */
            case 0xd: if (emuTarget == TGT_POKITTO) { /* MUL 32 bits (référence) */
                        uint32_t r = regs[rs] * a;
                        setReg(rd, r);
                        fZ = r == 0;
                        fN = (r & 0x80000000u) != 0;
                      } else { /* MUL : le TS laisse le produit NON masqué (double
                                * JS) — on réplique via regD + fmod exact ; les
                                * Inf/NaN du TS donnent 0 (ToUint32).  Z teste la
                                * valeur non masquée, comme le == 0 du TS. */
                        double p = regD[rd] * regD[rs];
                        regD[rd] = p;
                        double m = fmod(p, 4294967296.0);
                        regs[rd] = (m >= -4294967295.0 && m <= 4294967295.0)
                                   ? (uint32_t)(int64_t)m : 0;
                        fZ = (p == 0.0);
                        fN = (regs[rd] & 0x80000000u) != 0;
                      }
                      break;
            case 0xe: setReg(rd, a & ~b); setNZ(regs[rd]); break;              /* BIC */
            case 0xf: setReg(rd, ~b); setNZ(regs[rd]); break;                  /* MVN */
        }
    }
    /* opérations sur registres hauts / BX / BLX */
    else if ((op & 0xfc00) == 0x4400) {
        int opH = (op >> 6) & 0xf;
        int rs = (op >> 3) & 7, rd = op & 7;
        switch (opH) {
            case 0x1: setReg(rd, addSetCond(regs[rd], regs[rs + 8], 0)); break;
            case 0x2: setReg(rd + 8, addSetCond(regs[rd + 8], regs[rs], 0)); break;
            case 0x3: setReg(rd + 8, addSetCond(regs[rd + 8], regs[rs + 8], 0)); break;
            case 0x5: addSetCond(regs[rd], ~regs[rs + 8], 1); break;
            case 0x6: addSetCond(regs[rd + 8], ~regs[rs], 1); break;
            case 0x7: addSetCond(regs[rd + 8], ~regs[rs + 8], 1); break;
            case 0x8: setReg(rd, regs[rs]); break;
            case 0x9: setReg(rd, regs[rs + 8]); break;
            case 0xa: if (rd + 8 == 15) { setReg(15, regs[rs] & ~1u); incrementPc(); }
                      else setReg(rd + 8, regs[rs]); break;
            case 0xb: if (rd + 8 == 15) { setReg(15, regs[rs + 8] & ~1u); incrementPc(); }
                      else setReg(rd + 8, regs[rs + 8]); break;
            case 0xc: setReg(15, regs[rs] & ~1u); incrementPc(); break;        /* BX r */
            case 0xd: setReg(15, regs[rs + 8] & ~1u); incrementPc(); break;    /* BX h */
            case 0xe: case 0xf: /* BLX r<rm> — Pokitto : API ROM 0x1fff1ffx
                                 * (IAP/division) interceptée avant le saut */
                if (emuTarget == TGT_POKITTO) { pk_blx(op); break; }
                setReg(14, (regs[15] - 2) | 1);
                setReg(15, regs[(op >> 3) & 7] & ~1u);
                incrementPc();
                break;
        }
    }
    /* LDR littéral (PC) */
    else if ((op & 0xf800) == 0x4800) {
        int rd = (op >> 8) & 7;
        uint32_t imm = (uint32_t)(op & 0xff) << 2;
        setReg(rd, fetchWord((regs[15] & ~3u) + imm));
    }
    /* load/store offset par registre */
    else if ((op & 0xf200) == 0x5000) {
        int lb = (op >> 10) & 3, ro = (op >> 6) & 7, rb = (op >> 3) & 7, rd = op & 7;
        uint32_t a = regs[rb] + regs[ro];
        if (lb == 0) writeWord(a, regs[rd]);
        else if (lb == 1) writeByte(a, (uint8_t)regs[rd]);
        else if (lb == 2) setReg(rd, fetchWord(a));
        else setReg(rd, fetchByte(a));
    }
    /* load/store signé / demi-mot par registre */
    else if ((op & 0xf200) == 0x5200) {
        int hs = (op >> 10) & 3, ro = (op >> 6) & 7, rb = (op >> 3) & 7, rd = op & 7;
        uint32_t a = regs[rb] + regs[ro];
        if (hs == 0) writeHalf(a, (uint16_t)regs[rd]);
        else if (hs == 1) { uint32_t v = fetchByte(a); setReg(rd, v & 0x80 ? v | 0xffffff00u : v); }
        else if (hs == 2) setReg(rd, fetchHalf(a));
        else { uint32_t v = fetchHalf(a); setReg(rd, v & 0x8000 ? v | 0xffff0000u : v); }
    }
    /* load/store immédiat mot/octet */
    else if ((op & 0xe000) == 0x6000) {
        int bl = (op >> 11) & 3, off = (op >> 6) & 0x1f, rb = (op >> 3) & 7, rd = op & 7;
        if (bl == 0) writeWord(regs[rb] + ((uint32_t)off << 2), regs[rd]);
        else if (bl == 1) setReg(rd, fetchWord(regs[rb] + ((uint32_t)off << 2)));
        else if (bl == 2) writeByte(regs[rb] + off, (uint8_t)regs[rd]);
        else setReg(rd, fetchByte(regs[rb] + off));
    }
    /* load/store demi-mot immédiat */
    else if ((op & 0xf000) == 0x8000) {
        int l = (op >> 11) & 1, off = (op >> 6) & 0x1f, rb = (op >> 3) & 7, rd = op & 7;
        if (l) setReg(rd, fetchHalf(regs[rb] + ((uint32_t)off << 1)));
        else writeHalf(regs[rb] + ((uint32_t)off << 1), (uint16_t)regs[rd]);
    }
    /* SP-relatif */
    else if ((op & 0xf000) == 0x9000) {
        int l = (op >> 11) & 1, rd = (op >> 8) & 7;
        uint32_t off = (uint32_t)(op & 0xff) << 2;
        if (l) setReg(rd, fetchWord(regs[13] + off));
        else writeWord(regs[13] + off, regs[rd]);
    }
    /* ADR / ADD rd, SP */
    else if ((op & 0xf000) == 0xa000) {
        int sp = (op >> 11) & 1, rd = (op >> 8) & 7;
        uint32_t c = (uint32_t)(op & 0xff) << 2;
        setReg(rd, sp ? regs[13] + c : (regs[15] & ~3u) + c);
    }
    /* add SP+offset */
    else if ((op & 0xff00) == 0xb000) {
        int neg = (op >> 7) & 1;
        uint32_t v = (uint32_t)(op & 0x7f) << 2;
        regs[13] += neg ? -v : v;
    }
    /* extensions signées / rev */
    else if ((op & 0xff00) == 0xb200) {
        int rm = (op >> 3) & 7, rd = op & 7, opc = (op >> 6) & 3;
        uint32_t v = regs[rm];
        if (opc == 0) { v &= 0xffff; if (v & 0x8000) v |= 0xffff0000u; }
        else if (opc == 1) { v &= 0xff; if (v & 0x80) v |= 0xffffff00u; }
        else if (opc == 2) v &= 0xffff;
        else v &= 0xff;
        setReg(rd, v);
    }
    else if ((op & 0xff00) == 0xba00) {
        int rm = (op >> 3) & 7, rd = op & 7, opc = (op >> 6) & 3;
        uint32_t v = regs[rm];
        if (opc == 0) v = ((v & 0xff000000u) >> 24) | ((v & 0x00ff0000u) >> 8) |
                          ((v & 0x0000ff00u) << 8) | ((v & 0x000000ffu) << 24);
        else if (opc == 1) v = ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8);
        setReg(rd, v);
    }
    else if (emuTarget == TGT_POKITTO && (op & 0xffc0) == 0xb640) {
        /* CPSIE/CPSID : PRIMASK, gate des interruptions (référence C++) */
        armIrqEnable = 1 ^ (int)((op >> 4) & 1);
    }
    else if ((op & 0xffe8) == 0xb666) { /* CPS : no-op comme le TS */ }
    /* push/pop — masque 0xf600 comme le TS : 0xfe00 laissait tous les POP
     * (0xbcxx-0xbdxx) hors du décodeur (traités en no-op → chute de pile) */
    else if ((op & 0xf600) == 0xb400) {
        int l = (op >> 11) & 1, r = (op >> 8) & 1, rlist = op & 0xff;
        if (!l) {
            if (r) pushStack(regs[14]);
            for (int i = 7; i >= 0; i--) if (rlist & (1u << i)) pushStack(regs[i]);
        } else {
            for (int i = 0; i < 8; i++) if (rlist & (1u << i)) setReg(i, popStack());
            if (r) { setReg(15, popStack() & ~1u); incrementPc(); }
        }
    }
    /* LDMIA/STMIA */
    else if ((op & 0xf000) == 0xc000) {
        int l = (op >> 11) & 1, rb = (op >> 8) & 7, rlist = op & 0xff;
        uint32_t addr = regs[rb];
        for (int i = 0; i < 8; i++) {
            if (rlist & (1u << i)) {
                if (l) setReg(i, fetchWord(addr)); else writeWord(addr, regs[i]);
                addr += 4;
            }
        }
        regs[rb] = addr;
    }
    /* branchement conditionnel */
    else if ((op & 0xf000) == 0xd000) {
        int cond = (op >> 8) & 0xf;
        int32_t off = op & 0xff;
        if (off & 0x80) off |= ~0xff;
        off <<= 1;
        int take = 0;
        switch (cond) {
            case 0x0: take = fZ; break;
            case 0x1: take = !fZ; break;
            case 0x2: take = fC; break;
            case 0x3: take = !fC; break;
            case 0x4: take = fN; break;
            case 0x5: take = !fN; break;
            case 0x6: take = fV; break;
            case 0x7: take = !fV; break;
            case 0x8: take = fC && !fZ; break;
            case 0x9: take = !fC || fZ; break;
            case 0xa: take = fN == fV; break;
            case 0xb: take = fN != fV; break;
            case 0xc: take = !fZ && (fN == fV); break;
            case 0xd: take = fZ || (fN != fV); break;
        }
        if (take) { regs[15] = (uint32_t)((int32_t)regs[15] + off); incrementPc(); }
    }
    /* SWI : no-op (comme le TS qui ne le décode pas) */
    else if ((op & 0xff00) == 0xdf00) { }
    /* branchement inconditionnel */
    else if ((op & 0xf800) == 0xe000) {
        int32_t off = op & 0x7ff;
        if (off & 0x400) off |= ~0x7ff;
        off <<= 1;
        regs[15] = (uint32_t)((int32_t)regs[15] + off);
        incrementPc();
    }
    /* BL long : deux demi-mots comme le TS.  LR doit pointer APRÈS la paire
     * (bit Thumb posé) : le TS fixe LR au second demi-mot — l'ancien port C
     * laissait LR = PC + off1<<12, ce qui corrompait tout retour bx lr.
     * La paire coûte 3 ticks et finit avec PC = cible+2, comme le TS. */
    else if (emuTarget == TGT_POKITTO && (op & 0xff80) == 0xf380) {
        /* 0xF38x « MRS » de la référence : rd = SP (le second demi-mot est
         * réellement exécuté au pas suivant, comme la table C++) */
        uint16_t nextInst = fetchHalf(instAddr + 2);
        setReg((nextInst >> 8) & 0xF, regs[13]);
    }
    else if ((op & 0xf800) == 0xf000) { /* espace 32 bits 0xF000-0xF7FF */
        uint16_t nextInst = fetchHalf(instAddr + 2);
        if ((nextInst & 0xf800) == 0xf800) {
            /* BL : LR doit pointer APRÈS la paire (bit Thumb posé) — le TS
             * fixe LR au second demi-mot, l'ancien port laissait
             * LR = PC + off1<<12 en corrompant tout retour bx lr.
             * La paire coûte 3 ticks et finit avec PC = cible+2 (état TS). */
            int32_t off1 = op & 0x7ff;
            if (off1 & 0x400) off1 |= ~0x7ff;
            int32_t off2 = nextInst & 0x7ff;
            uint32_t retour = regs[15];                      /* instAddr + 4 */
            setReg(14, regs[15] + ((uint32_t)off1 << 12));   /* intermédiaire */
            setReg(15, regs[14] + ((uint32_t)(off2 << 1)));  /* cible */
            if (traceAllStep) { /* ligne du second demi-mot, comme le pas TS
                                 * (LR = valeur intermédiaire à ce stade) */
                fprintf(stderr, "%ld %u %x %04x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n", stepNo, tickCount,
                        retour - 2, nextInst, regs[13],
                        regs[0], regs[1], regs[2], regs[3],
                        regs[4], regs[5], regs[6], regs[7],
                        regs[8], regs[9], regs[10], regs[11], regs[12], regs[14]);
                stepNo++;
            }
            setReg(14, retour | 1u);                         /* adresse de retour */
            incrementPc(); /* second demi-mot : PC = cible+2 */
            incrementPc(); /* troisième tick de la paire (pas du second demi-mot) */
            regs[15] -= 2; /* état final du TS */
        } else {
            /* MRS (0xF3EF 0x8xxx) : Rd <- 0 — mode thread (IPSR = 0) ; les
             * libs testent « suis-je dans une ISR ? » avec.  MSR, DSB/DMB
             * et autres 0xF0xx-0xF7FF : paire consommée sans effet.  SANS
             * ÇA le second demi-mot s'exécutait comme instruction 16 bits
             * fantôme (STRH sauvage en flash, registres écrasés, tests
             * IPSR déraillant — écran noir des jeux lib récente). */
            if ((op & 0xfff0) == 0xf3e0 && (nextInst & 0xf000) == 0x8000)
                setReg((nextInst >> 8) & 0xF, 0);
            incrementPc(); /* consomme le second demi-mot */
        }
    }
    else if ((op & 0xf000) == 0xe800 || (op & 0xf800) == 0xf800) {
        /* reste de l'espace 32 bits (0xE800-0xEFFF, 0xF800-0xFFFF : ldr.w,
         * str.w, udiv… ) : paire consommée comme no-op — le second demi-mot
         * ne doit jamais s'exécuter seul */
        incrementPc();
    }
    else {
        /* instruction non décodée : comme le TS, on continue */
    }
#if defined(EMU_IHASH) || defined(WASM_DEBUG)
    if (ihashOn)
    {
        ihash = (ihash ^ (instAddr * 2654435761u)) * 16777619u;
        ihash = (ihash ^ inst) * 16777619u;
        for (int ri = 0; ri < 8; ri++) ihash = (ihash ^ regs[ri]) * 16777619u;
        ihash = (ihash ^ regs[13]) * 16777619u;
    }
#endif
    if (hashMode && (tickCount % hashInterval) == 0)
        fprintf(stderr, "H %u %08x sp=%08x I %08x R %x %x %x %x %x %x %x %x %x %x %x %x %x %x TC4 %d %d %u %u %u %u %u\n",
                tickCount, state_hash(), regs[13], ihash,
                regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
                regs[6], regs[7], regs[8], regs[9], regs[10], regs[11],
                regs[12], regs[14], regs[15],
                tc4Enabled, tc4Armed, tc4Period, tc4Top, tc4Counter,
                tc4Window, tc4Fires - tc4Writes);
    if (sramDumpAt && tickCount >= sramDumpAt) {
        const char *p = getenv("SRAM_DUMP");
        FILE *df = fopen(p ? p : "/tmp/sram_c.bin", "wb");
        if (df) { fwrite(sram, 1, SRAM_SIZE, df); fclose(df); }
        fprintf(stderr, "[sram dump @%u]\n", tickCount);
        sramDumpAt = 0; /* une seule fois */
    }
}

/* --------------------------------------------------------- SDL + main */

static SDL_Texture *tex;
static uint32_t px32[MAX_SCREEN_W * MAX_SCREEN_H];

static void blit(SDL_Renderer *ren) {
    const uint16_t *src = emuTarget == TGT_POKITTO ? pk_lcd : pix;
    for (unsigned i = 0; i < SCR_W * SCR_H; i++) {
        uint16_t p = src[i];
        uint8_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
        /* même expansion que st7735.ts : décalages, pas de mise à l'échelle */
        px32[i] = 0xff000000u | ((b << 3) << 16) | ((g << 2) << 8) | (r << 3);
    }
    SDL_UpdateTexture(tex, NULL, px32, SCR_W * sizeof(uint32_t));
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

/* callback HLE (audio du firmware stock Pokitto) : lit le double tampon
 * du jeu via son playhead, comme la référence */
static void pk_hle_audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    if (!pk_hleBuffer || !pk_hlePlayhead) { memset(stream, 128, (size_t)len); return; }
    const uint8_t *srcb = pk_hleBuffer + 512 * ((pk_hlePlayhead[0] >> 9) & 1);
    int n = len < 512 ? len : 512;
    memcpy(stream, srcb, (size_t)n);
    memset(stream + n, 128, (size_t)(len - n));
    for (int i = 0; i < n; i++) wav_put((int16_t)((srcb[i] ^ 0x80) << 8));
}

/* audio Pokitto non HLE : ring (delta, octet) -> u8 22050 Hz, rééchantillonné
 * exactement comme la référence ; chaque octet sorti alimente aussi le WAV */
static void pk_audio_cb(Uint8 *stream, int len) {
    float err = 0;
    for (int i = 0; i < len; i++) {
        if (pk_aqSize < (uint32_t)len) {
            for (; i < len; i++) stream[i] = (uint8_t)pk_audioHoldF;
            return;
        }
        while (pk_aqSize) {
            float d = pk_aqDelta[pk_aqStart] + err - PK_IFREQ;
            if (d > 0) { pk_aqDelta[pk_aqStart] = d; err = 0; break; }
            pk_audioHoldF = pk_aqData[pk_aqStart];
            err = -d;
            pk_aqStart = (pk_aqStart + 1) & PK_AQ_MASK;
            pk_aqSize--;
        }
        stream[i] = (uint8_t)pk_audioHoldF;
        if (wavFile) wav_put((int16_t)(((uint8_t)pk_audioHoldF ^ 0x80) << 8));
    }
}

/* audio SDL : l'ISR écrit dans aq ; le callbackSDL consomme */
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    if (emuTarget == TGT_POKITTO) { pk_audio_cb(stream, len); return; }
    int16_t *out = (int16_t *)stream;
    for (int i = 0; i < len / 2; i++) {
        if (audioPending) { out[i] = 0; continue; } /* pré-buffer : silence */
        if (aq_head != aq_tail) {
            audioHold = aq[aq_head];
            aq_head = (aq_head + 1) % AQ_SIZE;
        } else {
            audioHold /= 2; /* sous-débit : relâche vers le silence */
        }
        out[i] = audioHold;
    }
}

/* boutons : même masque pour les deux cibles (voir le bloc Pokitto) ;
 * META : buttonData sur PB03, Pokitto : broches GPIO via pk_btn_gpio */

static uint32_t homeHeld;
static unsigned machineEpoch; /* incrémenté quand un drop réinitialise la machine */

static uint8_t key_bit(SDL_Keycode sym) {
    if (emuTarget == TGT_POKITTO) { /* A/B/C/D de la référence C++ */
        switch (sym) {
            case SDLK_UP: case SDLK_i: return BTN_UP;
            case SDLK_DOWN: case SDLK_k: return BTN_DOWN;
            case SDLK_LEFT: case SDLK_j: return BTN_LEFT;
            case SDLK_RIGHT: case SDLK_l: return BTN_RIGHT;
            case SDLK_a: return BTN_A;
            case SDLK_s: case SDLK_b: return BTN_B;
            case SDLK_d: case SDLK_c: return BTN_MENU;         /* C */
            case SDLK_f: return BTN_HOME;                      /* D (éclairage) */
            default: return 0;
        }
    }
    switch (sym) {
        case SDLK_DOWN: case SDLK_s: return BTN_DOWN;
        case SDLK_LEFT: case SDLK_q: case SDLK_a: return BTN_LEFT;
        case SDLK_RIGHT: case SDLK_d: return BTN_RIGHT;
        case SDLK_UP: case SDLK_z: case SDLK_w: return BTN_UP;
        case SDLK_j: case SDLK_SPACE: return BTN_A;            /* espace = A */
        case SDLK_k: case SDLK_LCTRL: case SDLK_RCTRL: return BTN_B; /* ctrl = B */
        case SDLK_u: case SDLK_RETURN: case SDLK_KP_ENTER: return BTN_MENU; /* entrée = start/menu */
        case SDLK_i: case SDLK_ASTERISK: case SDLK_KP_MULTIPLY: return BTN_HOME; /* * = select/home */
        default: return 0;
    }
}

static void btn_press(uint8_t mask) {
    if (emuTarget == TGT_POKITTO) { pk_btn_gpio(mask, 1); return; }
    if (getenv("EMU_DEBUG")) fprintf(stderr, "[btn] press mask=%02x -> buttonData=%02x\n", mask, (uint8_t)(buttonData & ~mask));
    buttonData &= (uint8_t)~mask;
    if (mask & BTN_HOME) homeHeld = SDL_GetTicks();
}

static void btn_release(uint8_t mask) {
    if (emuTarget == TGT_POKITTO) { pk_btn_gpio(mask, 0); return; }
    buttonData |= mask;
    if (mask & BTN_HOME) homeHeld = 0;
}

/* manette (SDL_GameController) */
static uint8_t pad_button_mask(uint8_t b) {
    if (emuTarget == TGT_POKITTO) {
        switch (b) {
            case SDL_CONTROLLER_BUTTON_A: return BTN_A;
            case SDL_CONTROLLER_BUTTON_B: return BTN_B;
            case SDL_CONTROLLER_BUTTON_X: return BTN_MENU;      /* C */
            case SDL_CONTROLLER_BUTTON_Y: return BTN_HOME;      /* D */
            case SDL_CONTROLLER_BUTTON_START: return BTN_MENU;
            case SDL_CONTROLLER_BUTTON_BACK: return BTN_HOME;
            default: return 0;
        }
    }
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return BTN_A;
        case SDL_CONTROLLER_BUTTON_B: return BTN_B;
        case SDL_CONTROLLER_BUTTON_START: return BTN_MENU;
        case SDL_CONTROLLER_BUTTON_BACK: case SDL_CONTROLLER_BUTTON_GUIDE: return BTN_HOME;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return BTN_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return BTN_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return BTN_RIGHT;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return BTN_UP;
        default: return 0;
    }
}

/* stick gauche -> croix directionnelle (zone morte ~18 %) */
static uint8_t pad_dir_bits(SDL_GameController *pad) {
    int16_t ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    const int16_t dz = 6000;
    uint8_t m = 0;
    if (ax < -dz) m |= BTN_LEFT;
    if (ax > dz) m |= BTN_RIGHT;
    if (ay < -dz) m |= BTN_UP;
    if (ay > dz) m |= BTN_DOWN;
    return m;
}

/* joystick sans mapping (boutons 0..3 = A, B, MENU, HOME ; chapeau = croix) */
static uint8_t joy_button_mask(uint8_t b) {
    switch (b) {
        case 0: return BTN_A;
        case 1: return BTN_B;
        case 2: return emuTarget == TGT_POKITTO ? BTN_MENU : BTN_MENU;  /* C / MENU */
        case 3: return BTN_HOME;                                        /* D / HOME */
        default: return 0;
    }
}

static uint8_t joy_hat_bits(SDL_Joystick *joy) {
    uint8_t h = SDL_JoystickGetHat(joy, 0);
    uint8_t m = 0;
    if (h & SDL_HAT_DOWN) m |= BTN_DOWN;
    if (h & SDL_HAT_LEFT) m |= BTN_LEFT;
    if (h & SDL_HAT_RIGHT) m |= BTN_RIGHT;
    if (h & SDL_HAT_UP) m |= BTN_UP;
    return m;
}

static char fwPath[1024];
static const char *fwName = NULL;
static int fwLoaded;
static int sd_explicit; /* carte passée explicitement en ligne de commande */
static int fwNeedsBoot; /* un zip a chargé un firmware : vecteurs à remettre */

static SDL_Window *emuWin;
static SDL_Renderer *emuRen;
static SDL_GameController *pad;
static SDL_Joystick *joyFb;
static uint8_t padDirBits; /* directions tenues par stick/chapeau */

static uint32_t emu_nextFrameTick = 334860u; /* pas initial (frame_ticks suit la cible) */
static Uint32 titleMs;
static uint32_t titleTick;
static char titleBuf[1200]; /* dernier titre construit (HUD wasm) */
static int titlePct;      /* dernier % (HUD wasm) */

/* décharge la carte SD courante (image ou image FAT d'un répertoire) */
static void sd_unload(void) {
    free(sd_image); sd_image = NULL; sd_size = 0;
    free(fatImage); fatImage = NULL; fatImageSize = 0;
    free(fatTable); fatTable = NULL;
    fatFileCount = 0; fatNext = 2;
    vfiles_reset();
    sd_reset_state();
    sd_initialized = 0;
    pk_sd_machine_reset();
    pk_sd_dirty = 0;
}

/* réinitialise la machine en conservant la carte SD montée
 * (changement de jeu depuis le sélecteur) */
static void reset_core(void) {
    if (emuTarget == TGT_POKITTO) { pk_reset_core(); return; }
    memset(sram, 0xff, SRAM_SIZE);
    memset(regs, 0, sizeof regs);
    memset(regD, 0, sizeof regD);
    fN = fZ = fC = fV = 0;
    tickCount = 0; sysTickTrigger = 0; sysTickEntries = 0;
    sysTickVector = dmacVector = tc4Vector = 0;
    dmacInterrupt = tc4Interrupt = 0;
    dmac_baseAddr = dmac_wrbAddr = dmac_desc = dmac_chid = 0;
    memset(dmaTrig, 0, sizeof dmaTrig); memset(dmaOn, 0, sizeof dmaOn);
    /* PA27 (CS carte SD) et PA25 (CS boutons) hauts : lignes désélectionnées
     * (pull-ups réelles), les périphériques n'écoutent le bus SPI que si le
     * firmware les sélectionne.  Les jeux sans lib standard (pas de pinMode
     * early) restent sinon « sélectionnés » en permanence et le trafic SPI
     * écran part dans la machine SD. */
    portA_out = (1u << 27) | (1u << 25); /* PA27 (SD) et PA25 (boutons) hauts */
    portB_out = portA_dir = portB_dir = 0;
    ser4_data = 0x80;
    tc4Enabled = tc4Armed = 0;
    tc4IntEnMask = tc4IntFlagMask = 0;
    tc4CtrlA = 0;
    spiDmaCh = -1; spiBeatAcc = 0; spiBeatTicks = 7; spiBaud = 0;
    memset(dmacIntFlag, 0, sizeof dmacIntFlag);
    tc4Top = tc4Counter = 0; tc4Period = 907;
    tc4Window = tc4Fires = tc4Writes = 0;
    lcd_xStart = lcd_xEnd = lcd_yStart = lcd_yEnd = lcd_x = lcd_y = 0;
    lcd_argIndex = lcd_lastCommand = lcd_tmp = 0;
    lcdBgrSwapped = 0;
    memset(pix, 0, sizeof pix);
    millisWrites = 0;
    aq_head = aq_tail = 0; audioHold = 0;
    emu_nextFrameTick = tickCount + frame_ticks();
    sd_reset_state(); /* pas de transaction SD résiduelle pour le jeu suivant */
    sd_initialized = 0; /* le firmware suivant rejoue toute l'init (CMD0 doit
                         * répondre « idle ») */
}

/* réinitialise toute la machine (drop d'un nouveau firmware) */
static void reset_machine(void) {
    reset_core();
    sd_unload();
}

static uint8_t *fwData;      /* copie du firmware (F5 = redémarrer) */
static size_t fwLen;
static void boot_vectors(void);
static void audio_start(void);
static void refresh_title(void);

/* conteneur .pop du loader Pokitto : enregistrements {clé, taille, données} ;
 * la clé du dernier enregistrement (>= 0x10000000, le SP initial) fait
 * double emploi avec les 8 premiers octets de la flash.  Sur un .bin brut,
 * le premier couple {SP, reset} déclenche le même chemin : identité. */
static void fw_strip_container(const uint8_t **data, size_t *len) {
    size_t off = 0;
    while (off + 8 <= *len) {
        uint32_t key = (uint32_t)(*data)[off] | ((uint32_t)(*data)[off+1] << 8) |
                       ((uint32_t)(*data)[off+2] << 16) | ((uint32_t)(*data)[off+3] << 24);
        uint32_t sz = (uint32_t)(*data)[off+4] | ((uint32_t)(*data)[off+5] << 8) |
                      ((uint32_t)(*data)[off+6] << 16) | ((uint32_t)(*data)[off+7] << 24);
        if (sz > *len || off + 8 + sz > *len) return; /* conteneur incohérent */
        if (key > 0x10000000u) {
            /* trouvé : la flash commence AU COUPLE (clé = SP, taille) */
            *data += off;
            *len -= off;
            return;
        }
        off += 8 + sz;
    }
}

static void load_firmware_data(const uint8_t *data, size_t len, const char *display) {
    const uint8_t *payload = data;
    size_t plen = len;
    fw_strip_container(&payload, &plen);
    uint32_t w0 = plen < 4 ? 0
                : (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
                  ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    if (!targetForced) {
        /* mot 0 = SP initial : SRAM LPC (0x1000xxxx) -> Pokitto,
         * SRAM SAM D21 (0x2000xxxx) -> META */
        if ((w0 & 0xFFFF0000u) == 0x10000000u) emuTarget = TGT_POKITTO;
        else if ((w0 & 0xFFFF0000u) == 0x20000000u) emuTarget = TGT_META;
        pk_screen_reconfig();
    }
    free(fwData);
    fwData = malloc(len ? len : 1);
    memcpy(fwData, data, len);
    fwLen = len;
    if (emuTarget == TGT_POKITTO) {
        memset(flash, 0x00, FLASH_SIZE); /* la référence ne remplit pas */
        size_t n = plen < FLASH_SIZE ? plen : FLASH_SIZE;
        if (n == 0) { fprintf(stderr, "firmware vide\n"); return; }
        memcpy(flash, payload, n);
        snprintf(fwPath, sizeof(fwPath), "%s", display);
        const char *b = strrchr(fwPath, '/');
        fwName = b ? b + 1 : fwPath;
        fwLoaded = 1;
        pk_reset_core(); /* vecteurs lisibles ici */
        pk_eeprom_load();
        printf("firmware Pokitto : %s (%zu Ko)\n", display, len / 1024);
        return;
    }
    memset(flash, 0xff, FLASH_SIZE); /* comme le TS : flash remplie de 0xff */
    size_t n = plen < (FLASH_SIZE - 0x4000) ? plen : (FLASH_SIZE - 0x4000);
    if (n == 0) { fprintf(stderr, "firmware vide\n"); return; }
    memcpy(flash + 0x4000, payload, n);
    snprintf(fwPath, sizeof(fwPath), "%s", display);
    const char *b = strrchr(fwPath, '/');
    fwName = b ? b + 1 : fwPath;
    fwLoaded = 1;
    printf("firmware : %s (%zu Ko)\n", display, len / 1024);
}

/* F5 : redémarre le firmware courant (la Pokitto garde sa carte et son
 * EEPROM, comme la référence) */
static void fw_restart(void) {
    if (!fwData) return;
    load_firmware_data(fwData, fwLen, fwPath);
    if (emuTarget == TGT_META) boot_vectors();
    audio_start();
    machineEpoch++;
    refresh_title();
}

static void load_firmware(const char *p, int rebindCard) {
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "firmware introuvable : %s\n", p); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)sz);
    if (fread(data, 1, (size_t)sz, f) == 0) {
        fprintf(stderr, "firmware vide\n"); free(data); fclose(f); return;
    }
    fclose(f);
    load_firmware_data(data, (size_t)sz, p);
    free(data);
    /* la carte SD est le répertoire contenant le firmware ; une carte
     * passée en ligne de commande ne rebind pas au premier lancement.
     * Pokitto : la référence ne rebind jamais automatiquement — carte =
     * image/zip/dossier explicite uniquement (un .bin déposé dans un
     * dossier géant ne construit pas une carte de plusieurs Go) */
    if (emuTarget == TGT_META && fwLoaded && (rebindCard || !sd_explicit)) {
        sd_unload();
        char dirbuf[1024];
        snprintf(dirbuf, sizeof(dirbuf), "%s", p);
        char *slash = strrchr(dirbuf, '/');
        if (slash) {
            *slash = 0;
            if (slash != dirbuf) fat_build_from_dir(dirbuf);
        }
    }
}

static int zip_load_card(const uint8_t *data, size_t len);

static void load_sd_from_path(const char *p) {
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) { sd_unload(); fat_build_from_dir(p); return; }
    FILE *g = fopen(p, "rb");
    if (!g) { fprintf(stderr, "carte introuvable : %s\n", p); return; }
    fseek(g, 0, SEEK_END); long sz = ftell(g); fseek(g, 0, SEEK_SET);
    sd_unload();
    sd_image = malloc((size_t)sz);
    fread(sd_image, 1, (size_t)sz, g);
    fclose(g);
    /* un .zip (ou tout fichier signé PK) = carte SD complète ; s'il
     * contient un .bin, il devient le firmware */
    if (sz > 4 && sd_image[0] == 'P' && sd_image[1] == 'K') {
        int r = zip_load_card(sd_image, (size_t)sz);
        free(sd_image); sd_image = NULL; sd_size = 0;
        if (r == 2) fwNeedsBoot = 1;
        return;
    }
    sd_size = (size_t)sz;
    printf("carte SD : image %s (%ld Kio)\n", p, sz / 1024);
}

static void boot_vectors(void) {
    if (emuTarget == TGT_POKITTO) {
        /* vecteurs déjà chargés par pk_reset_core */
        fprintf(stderr, "Pokitto : SP=%08x PC=%08x\n", regs[13], regs[15]);
        return;
    }
    vectorBase = 0x4000;
    regs[13] = fetchWord(vectorBase);
    regs[14] = 0xffffffffu;
    regs[15] = fetchWord(vectorBase + 4) & ~1u;
    incrementPc();
    sysTickVector = fetchWord(vectorBase + 0x3c) & ~1u;
    dmacVector = fetchWord(vectorBase + 0x58) & ~1u;
    tc4Vector = fetchWord(vectorBase + 0x8c) & ~1u;
    fprintf(stderr, "SP=%08x PC=%08x systick=%08x dmac=%08x tc4=%08x\n",
            regs[13], regs[15], sysTickVector, dmacVector, tc4Vector);
}

static void refresh_title(void) {
    char title[1200];
    if (fwLoaded) snprintf(title, sizeof(title), "%.900s", fwName);
    else snprintf(title, sizeof(title), "déposez un firmware .bin");
    SDL_SetWindowTitle(emuWin, title);
}

static void audio_start(void) {
    audioPending = 1; /* gate fermé : audio_resume_when_ready l'ouvrira */
}

/* ------------------------------------------------ SDL/HTML5 ------------ */

/* dimensions SDL selon la cible (appelé aussi au changement de cible) */
static void pk_screen_reconfig(void) {
    SCR_W = emuTarget == TGT_POKITTO ? 220 : 160;
    SCR_H = emuTarget == TGT_POKITTO ? 176 : 128;
    if (!emuRen) return;
    SDL_SetWindowSize(emuWin, (int)(SCR_W * 2), (int)(SCR_H * 2));
    SDL_RenderSetLogicalSize(emuRen, SCR_W, SCR_H);
    SDL_RenderSetIntegerScale(emuRen, SDL_TRUE);
    if (tex) { SDL_DestroyTexture(tex); tex = NULL; }
    tex = SDL_CreateTexture(emuRen, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, (int)SCR_W, (int)SCR_H);
}

/* audio HLE : réouvre le périphérique au taux réel du firmware (référence) */
static void pk_audio_reopen(int freq) {
    if (!audioOk || freq <= 0) return;
    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq = freq; want.format = AUDIO_U8; want.channels = 1;
    want.samples = 512; want.callback = pk_hle_audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!dev) return;
    if (audioDev) { SDL_PauseAudioDevice(audioDev, 1); SDL_CloseAudioDevice(audioDev); }
    audioDev = dev;
    SDL_PauseAudioDevice(dev, 0);
}

static int noPad(void) {
    static int v = -1;
    if (v < 0) v = getenv("EMU_NO_PAD") ? 1 : 0;
    return v;
}

static int sdl_init_all(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |
                 (noPad() ? 0 : SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK)) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_CreateWindowAndRenderer((int)(SCR_W * 2), (int)(SCR_H * 2),
                                    SDL_WINDOW_RESIZABLE, &emuWin, &emuRen) != 0) {
        fprintf(stderr, "fenêtre: %s\n", SDL_GetError());
        return 1;
    }
    /* échelle logique = écran console : redimensionnable, rendu entier */
    SDL_RenderSetLogicalSize(emuRen, SCR_W, SCR_H);
    SDL_RenderSetIntegerScale(emuRen, SDL_TRUE);

    /* manette déjà branchée : contrôleur sinon joystick brut */
    for (int i = 0; !noPad() && i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }
        else if (!joyFb) joyFb = SDL_JoystickOpen(i);
    }
    if (pad) printf("manette : %s\n", SDL_GameControllerName(pad));
    else if (joyFb) printf("joystick : %s\n", SDL_JoystickName(joyFb));

    refresh_title();
    tex = SDL_CreateTexture(emuRen, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, (int)SCR_W, (int)SCR_H);

    SDL_AudioSpec want, got;
    memset(&want, 0, sizeof(want));
    want.freq = 22049; want.format = AUDIO_S16SYS; want.channels = 1;
    /* 512 fige l'émulateur sur emscripten (SPN audio) : ne pas descendre */
    want.samples = 1024; want.callback = audio_cb;
    audioDev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    audioOk = audioDev != 0;
    if (audioOk) SDL_PauseAudioDevice(audioDev, 0); /* tourne en silence ;
        le pré-buffer est géré par audioPending dans le callback */
    if (!audioOk) fprintf(stderr, "audio indisponible : %s\n", SDL_GetError());
    if (audioOk && fwLoaded) audio_start();
    return 0;
}

/* boucle d'événements ; renvoie 0 pour quitter */
static int poll_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) return 0;
#ifndef __EMSCRIPTEN__
        else if (ev.type == SDL_DROPFILE) { /* le navigateur gère ses drops */
            char *fp = ev.drop.file;
            const char *ext = strrchr(fp, '.');
            struct stat st;
            printf("déposé : %s\n", fp);
            if (stat(fp, &st) == 0 && S_ISDIR(st.st_mode)) {
                load_sd_from_path(fp);
            } else if (ext && (strcasecmp(ext, ".img") == 0 || strcasecmp(ext, ".zip") == 0)) {
                load_sd_from_path(fp);
                if (fwNeedsBoot) {
                    fwNeedsBoot = 0;
                    boot_vectors();
                    audio_start();
                    machineEpoch++;
                    refresh_title();
                }
            } else {
                /* tout le reste = firmware : la carte SD devient son
                 * répertoire, la machine repart de zéro */
                reset_machine();
                load_firmware(fp, 1);
                if (fwLoaded) {
                    boot_vectors();
                    audio_start();
                    machineEpoch++;
                }
            }
            SDL_free(fp);
            refresh_title();
        }
#endif
        else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_F5) {
            if (fwLoaded) { fw_restart(); printf("redémarrage\n"); }
        }
        else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            uint8_t m = key_bit(ev.key.keysym.sym);
            if (m) {
                if (ev.type == SDL_KEYDOWN) btn_press(m);
                else btn_release(m);
            }
        }
        else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
            if (!pad && SDL_IsGameController(ev.cdevice.which)) {
                pad = SDL_GameControllerOpen(ev.cdevice.which);
                if (pad) printf("manette : %s\n", SDL_GameControllerName(pad));
            }
        }
        else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (pad && SDL_GameControllerGetAttached(pad) == SDL_FALSE) {
                SDL_GameControllerClose(pad); pad = NULL;
            }
        }
        else if (ev.type == SDL_JOYDEVICEADDED) {
            if (!pad && !joyFb && !SDL_IsGameController(ev.jdevice.which))
                joyFb = SDL_JoystickOpen(ev.jdevice.which);
        }
        else if (ev.type == SDL_JOYDEVICEREMOVED) {
            if (joyFb && SDL_JoystickGetAttached(joyFb) == SDL_FALSE) {
                SDL_JoystickClose(joyFb); joyFb = NULL;
            }
        }
        else if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) {
            uint8_t m = pad ? pad_button_mask(ev.cbutton.button) : 0;
            if (m) {
                if (ev.type == SDL_CONTROLLERBUTTONDOWN) btn_press(m);
                else btn_release(m);
            }
        }
        else if (ev.type == SDL_CONTROLLERAXISMOTION) {
            if (pad && (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ||
                        ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)) {
                uint8_t want = pad_dir_bits(pad) & BTN_DIRMASK;
                btn_release(padDirBits & ~want);  /* directions quittées */
                btn_press(want & ~padDirBits);    /* directions nouvellement dans la zone */
                padDirBits = want;
            }
        }
        else if (ev.type == SDL_JOYBUTTONDOWN || ev.type == SDL_JOYBUTTONUP) {
            uint8_t m = joyFb ? joy_button_mask(ev.jbutton.button) : 0;
            if (m) {
                if (ev.type == SDL_JOYBUTTONDOWN) btn_press(m);
                else btn_release(m);
            }
        }
        else if (ev.type == SDL_JOYHATMOTION) {
            if (joyFb) {
                uint8_t want = joy_hat_bits(joyFb);
                btn_release(padDirBits & ~want);
                btn_press(want & ~padDirBits);
                padDirBits = want;
            }
        }
    }
    return 1;
}

/* remontées console (512 frames, mode EMU_TRACE) + marqueurs 2M ticks */
static void update_diagnostics(Uint32 frame) {
    if (emuTarget == TGT_POKITTO) return; /* sondes Millis/TC4 = META */
    static int traceDiag = -1;
    if (traceDiag < 0) traceDiag = getenv("EMU_TRACE") ? 1 : 0;
    if ((frame & 511) == 0 && traceDiag) { /* remontée console toutes les 512 frames */
        static int maInit = -1;
        static uint32_t ma;
        if (maInit < 0) {
            maInit = 0;
            ma = getenv("MILLIS_ADDR") ? (uint32_t)strtoul(getenv("MILLIS_ADDR"), NULL, 16) : 0x20002c48u;
        }
        uint32_t millisVal = fetchWord(ma);
        fprintf(stderr, "[f%u] tick=%u pc=%08x millis=%u sysT=%ld stWr=%ld tc4f=%u tc4w=%u msWr=%ld\n",
                frame, tickCount, regs[15], millisVal, sysTickEntries,
                stWrites, tc4Fires, tc4Writes, millisWrites);
    }
    /* marqueurs toutes les 2M ticks, comparable au traceur TS */
    static uint32_t nextMark = 2000000;
    static int markTrace = -1;
    if (markTrace < 0) markTrace = getenv("EMU_TRACE") ? 1 : 0;
    if (markTrace && tickCount >= nextMark) {
        uint32_t ma = getenv("MILLIS_ADDR") ? (uint32_t)strtoul(getenv("MILLIS_ADDR"), NULL, 16) : 0x20002c48u;
        fprintf(stderr, "T %u M %u PC %x SYST %ld DACW %u FIRE %u\n",
                nextMark, fetchWord(ma), regs[15], sysTickEntries, tc4Writes, tc4Fires);
        nextMark += 2000000;
    }
}

/* % de vitesse dans la barre de titre (échantillonné toutes les 500 ms) */
static void update_title_pct(void) {
    Uint32 nowMs = SDL_GetTicks();
    if (nowMs - titleMs < 500) return;
    if (fwLoaded) {
        double emuMs = (double)(tickCount - titleTick) / ticks_per_sec();
        double wallMs = (double)(nowMs - titleMs);
        int pct = wallMs > 0.0 ? (int)(emuMs / wallMs * 100.0 + 0.5) : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100; /* jamais plus vite que le temps réel */
        titlePct = pct;
        snprintf(titleBuf, sizeof(titleBuf), "%.900s", fwName);
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "déposez un firmware .bin");
    }
    SDL_SetWindowTitle(emuWin, titleBuf);
    titleMs = nowMs;
    titleTick = tickCount;
}

static void run_emulated_frame(void) {
    uint32_t target = emu_nextFrameTick;
    while (tickCount < target) step();
    emu_nextFrameTick += frame_ticks();
}

#if defined(EMU_NODE_HEADLESS)
/* ------------------------------------------- node headless (debug) --- */

int main(int argc, char **argv) {
    memset(sram, 0xff, SRAM_SIZE);
    hashMode = 1;
    hashInterval = 5000000;
    ihashOn = 1;
    if (argc < 3) { fprintf(stderr, "usage: prog firmware.bin carte_dir [frames]\n"); return 1; }
    load_firmware(argv[1], 0);
    if (fwLoaded) boot_vectors();
    int frames = argc > 3 ? atoi(argv[3]) : 700;
    for (int f = 0; f < frames; f++) run_emulated_frame();
    fprintf(stderr, "FINAL %u %08x\n", tickCount, state_hash());
    return 0;
}

#elif !defined(__EMSCRIPTEN__)
/* ------------------------------------------------------------ natif --- */

int main(int argc, char **argv) {
    if (argc < 2)
        fprintf(stderr, "meta_emu : lancé sans firmware — déposez un .bin "
                        "dans la fenêtre (carte SD = son répertoire).\n");
    memset(sram, 0xff, SRAM_SIZE); /* comme le TS (constructeur Atsamd21) */
    if (getenv("MILLIS_WATCH")) millisWatchAddr = (uint32_t)strtoul(getenv("MILLIS_WATCH"), NULL, 16);
    if (getenv("WATCH_ADDR")) watchAddr = (uint32_t)strtoul(getenv("WATCH_ADDR"), NULL, 16);
    if (getenv("TRACE_FROM")) traceFrom = (uint32_t)strtoul(getenv("TRACE_FROM"), NULL, 10);
    hashMode = getenv("STATE_HASH") ? 1 : 0;
#ifdef EMU_IHASH
    ihashOn = 1;
#endif
    if (getenv("HASH_INTERVAL")) hashInterval = (uint32_t)strtoul(getenv("HASH_INTERVAL"), NULL, 10);
    const char *dumpAt = getenv("SRAM_DUMP_AT");
    if (dumpAt) sramDumpAt = (uint32_t)strtoul(dumpAt, NULL, 10);
    if (getenv("EMU_TARGET")) {
        if (strcasecmp(getenv("EMU_TARGET"), "pokitto") == 0) emuTarget = TGT_POKITTO;
        if (strcasecmp(getenv("EMU_TARGET"), "meta") == 0) emuTarget = TGT_META;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--build-vcard") == 0 && i + 3 < argc) {
            /* debug : --build-vcard out.img fichier1 fichier2 ... */
            for (int j = i + 2; j < argc; j++) {
                FILE *g = fopen(argv[j], "rb");
                if (!g) { fprintf(stderr, "vcard: %s illisible\n", argv[j]); continue; }
                fseek(g, 0, SEEK_END); long sz = ftell(g); fseek(g, 0, SEEK_SET);
                uint8_t *data = malloc((size_t)sz);
                fread(data, 1, (size_t)sz, g); fclose(g);
                vfiles_add(argv[j], data, (size_t)sz);
                free(data);
            }
            fat_build_from_vfiles();
            FILE *out = fopen(argv[i + 1], "wb");
            if (out) { fwrite(fatImage, 1, fatImageSize, out); fclose(out); }
            printf("vcard écrite : %s\n", argv[i + 1]);
            return 0;
        }
        if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            snprintf(wavPathStr, sizeof(wavPathStr), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            snprintf(shotPath, sizeof(shotPath), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            ++i;
            if (strcasecmp(argv[i], "pokitto") == 0) emuTarget = TGT_POKITTO;
            else if (strcasecmp(argv[i], "meta") == 0) emuTarget = TGT_META;
            else { fprintf(stderr, "cible inconnue : %s (meta|pokitto)\n", argv[i]); return 1; }
            targetForced = 1;
        } else if (strcmp(argv[i], "--out-img") == 0 && i + 1 < argc) {
            snprintf(outImgPath, sizeof(outImgPath), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-W") == 0) {
            pk_ignoreBadWrites = ~0u;
        } else if (strcmp(argv[i], "-w") == 0) {
            if (i + 1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
                pk_ignoreBadWrites = (uint32_t)strtoul(argv[++i], NULL, 10);
            else pk_ignoreBadWrites = 1;
        } else {
            const char *ext = strrchr(argv[i], '.');
            struct stat st;
            if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) { load_sd_from_path(argv[i]); sd_explicit = 1; }
            else if (ext && (strcasecmp(ext, ".img") == 0 || strcasecmp(ext, ".zip") == 0)) {
                load_sd_from_path(argv[i]); sd_explicit = 1;
                fwNeedsBoot = 0; /* le boot se fait après le parsing */
            }
            else if (!fwLoaded) load_firmware(argv[i], 0);
            else { load_sd_from_path(argv[i]); sd_explicit = 1; }
        }
    }
    if (fwLoaded) boot_vectors();
    fwNeedsBoot = 0;

    static int trace = -1;
    if (trace < 0) trace = getenv("EMU_TRACE") ? 1 : 0;
    Uint32 frame = 0;
    int running = 1;
    uint64_t perfFreq = SDL_GetPerformanceFrequency();
    const uint64_t frameDur = (uint64_t)((double)perfFreq / 59.7275 + 0.5); /* 1/59,7 s */
    uint64_t nextPace = 0; /* échéance temps réel de la prochaine frame */
    unsigned seenEpoch = 0;
    titleMs = SDL_GetTicks();
    titleTick = tickCount;

    if (sdl_init_all() != 0) return 1;
    if (audioOk && fwLoaded) audio_start(); /* lancé au boot */

    if (wavPathStr[0]) {
        wavFile = fopen(wavPathStr, "wb");
        if (wavFile) {
            uint8_t hdr[44] = {0};
            memcpy(hdr, "RIFF", 4); memcpy(hdr + 8, "WAVEfmt ", 8);
            hdr[16] = 16; hdr[20] = 1; hdr[22] = 1;
            uint32_t rate = emuTarget == TGT_POKITTO ? 22050 : 22049;
            hdr[24] = rate & 0xff; hdr[25] = (rate >> 8) & 0xff;
            uint32_t br = rate * 2;
            hdr[28] = br & 0xff; hdr[29] = (br >> 8) & 0xff;
            hdr[32] = 2; hdr[34] = 16;
            memcpy(hdr + 36, "data", 4);
            fwrite(hdr, 1, 44, wavFile);
        }
    }
    while (running) {
        running = poll_events();
        update_diagnostics(frame);
        if (homeHeld && SDL_GetTicks() - homeHeld > 3000) { /* reset maison */ }
        if (machineEpoch != seenEpoch) {
            /* un drop a réinitialisé la machine : resynchronise le pas de
             * frame et le chronométrage du titre */
            seenEpoch = machineEpoch;
            emu_nextFrameTick = tickCount + frame_ticks();
            titleTick = tickCount;
            titleMs = SDL_GetTicks();
        }
        if (maxFrames && frame >= maxFrames) break;
#ifdef __EMSCRIPTEN__
#else
        /* test headless : EMU_PRESS_A=<frame> appuie sur A 6 frames */
        static int pressA = -1;
        if (pressA < 0) pressA = getenv("EMU_PRESS_A") ? atoi(getenv("EMU_PRESS_A")) : 0;
        if (pressA && (frame == pressA)) btn_press(BTN_A);
        if (pressA && (frame == pressA + 6)) btn_release(BTN_A);
#endif

        if (fwLoaded) run_emulated_frame();
        frame++;

        if (emuTarget == TGT_POKITTO) pk_adc_frame();
        update_title_pct();
        audio_resume_when_ready();

        blit(emuRen);
        if (trace) { /* empreinte d'écran périodique */
            static uint32_t lastPrint = 0;
            if (frame - lastPrint >= 60) {
                lastPrint = frame;
                const uint16_t *srcp = emuTarget == TGT_POKITTO ? pk_lcd : pix;
                uint32_t h = 0x811c9dc5;
                int distinct = 0;
                uint16_t seen[16] = {0};
                for (unsigned i = 0; i < SCR_W * SCR_H; i++) {
                    h = (h ^ srcp[i]) * 0x01000193u;
                    int k = 0;
                    for (; k < 16; k++) if (seen[k] == srcp[i]) break;
                    if (k == 16) distinct++;
                }
                uint32_t nz = 0;
                for (unsigned q = 0; q < SCR_W * SCR_H; q += 7) if (srcp[q]) nz++;
                            fprintf(stderr, "  derniers octets SPI: %02x %02x %02x %02x %02x %02x %02x %02x (nz=%ld/%ld)\n",
                        serLast[0], serLast[1], serLast[2], serLast[3],
                        serLast[4], serLast[5], serLast[6], serLast[7], serNz, stWrites);
                fprintf(stderr, "[frame %u] hash=%08x distinct<=%d nz=%u stWr=%ld ramwr=%ld x=%u y=%u cmd=%02x\n",
                        frame, h, distinct, nz, stWrites, ramwrTotal, lcd_x, lcd_y, lcd_lastCommand);
            }
        }
        /* temps réel : une frame = 1/59,7 s, jamais plus vite.  Échéance
         * sans dette : en retard de plus de 4 frames (stall, drag de
         * fenêtre, drop), on repart de maintenant au lieu de rattraper. */
        {
            uint64_t now = SDL_GetPerformanceCounter();
            if (nextPace == 0) nextPace = now + frameDur;
            nextPace += frameDur;
            if (now > nextPace + 4 * frameDur) nextPace = now + frameDur;
            if (now < nextPace) {
                Uint32 ms = (Uint32)(((nextPace - now) * 1000) / perfFreq);
                if (ms > 1) SDL_Delay(ms - 1);
                while (SDL_GetPerformanceCounter() < nextPace) { /* affinage */ }
            }
        }

    }

    if (shotPath[0]) {
        FILE *sf = fopen(shotPath, "wb");
        if (sf) {
            const uint16_t *srcp = emuTarget == TGT_POKITTO ? pk_lcd : pix;
            fprintf(sf, "P6\n%u %u\n255\n", SCR_W, SCR_H);
            for (unsigned i = 0; i < SCR_W * SCR_H; i++) {
                uint16_t pc = srcp[i];
                uint8_t rgb[3] = { (uint8_t)((((pc >> 11) & 0x1f) << 3)),
                                   (uint8_t)((((pc >> 5) & 0x3f) << 2)),
                                   (uint8_t)(((pc & 0x1f) << 3)) };
                fwrite(rgb, 1, 3, sf);
            }
            fclose(sf);
            printf("capture : %s\n", shotPath);
        }
    }
    wav_finish();
    pk_debug_dump();
    pk_eeprom_save();
    pk_card_export();
    { const char *fd = getenv("FLASH_DUMP"); /* flash après auto-patch du jeu */
      if (fd && emuTarget == TGT_META) {
        FILE *g = fopen(fd, "wb");
        if (g) { fwrite(flash, 1, FLASH_SIZE, g); fclose(g); }
      } }
    if (audioDev) SDL_CloseAudioDevice(audioDev);
    SDL_DestroyRenderer(emuRen); SDL_DestroyTexture(tex); SDL_DestroyWindow(emuWin);
    SDL_Quit();
    return 0;
}

#else
/* ------------------------------------------------------------ wasm ---- */

#include <emscripten.h>

/* points d'entrée JS -> C (drop / sélection de dossier) */

/* ---- sélecteur de jeux : énumération + changement à chaud ---- */

static int vfile_is_game(const VFile *v) {
    size_t l = strlen(v->path);
    return l > 4 && strcasecmp(v->path + l - 4, ".bin") == 0;
}

EMSCRIPTEN_KEEPALIVE
int emu_games_count(void) {
    int n = 0;
    for (int i = 0; i < nvfiles; i++) if (vfile_is_game(&vfiles[i])) n++;
    return n;
}

EMSCRIPTEN_KEEPALIVE
int emu_game_name(int idx, char *out, int outlen) {
    for (int i = 0, k = 0; i < nvfiles; i++) {
        if (!vfile_is_game(&vfiles[i])) continue;
        if (k++ == idx) {
            snprintf(out, outlen, "%s", vfiles[i].path);
            return (int)strlen(vfiles[i].path);
        }
    }
    return 0;
}

/* l'icône du jeu = ICON.BMP du même dossier ; renvoie la taille copiée */
EMSCRIPTEN_KEEPALIVE
int emu_game_icon(int idx, char *out, int outlen) {
    char bin[1024];
    if (emu_game_name(idx, bin, sizeof(bin)) == 0) return 0;
    char dir[1024] = "";
    char *sl = strrchr(bin, '/');
    if (sl) snprintf(dir, sizeof(dir), "%.*s", (int)(sl - bin), bin);
    char icon[1100];
    snprintf(icon, sizeof(icon), "%s%sICON.BMP", dir, dir[0] ? "/" : "");
    for (int i = 0; i < nvfiles; i++) {
        if (strcasecmp(vfiles[i].path, icon) == 0) {
            int n = vfiles[i].size < outlen ? (int)vfiles[i].size : outlen;
            memcpy(out, vfiles[i].data, n);
            return n;
        }
    }
    return 0;
}

/* boot différé : le firmware est préparé (emu_firmware) mais la machine ne
 * démarre qu'une fois la carte SD montée (emu_card_finish), sinon le guest
 * rate l'init SD et reste sur son écran d'erreur */
static int booted;

static void emu_boot(void) {
    if (!fwLoaded || booted) return;
    booted = 1;
    boot_vectors();
    audio_start();
    machineEpoch++;
    refresh_title();
}

/* change de jeu : la carte reste montée, seul le firmware change */
EMSCRIPTEN_KEEPALIVE
int emu_select_game(const char *name) {
    for (int i = 0; i < nvfiles; i++) {
        if (strcasecmp(vfiles[i].path, name) != 0) continue;
        if (!vfile_is_game(&vfiles[i])) return 0;
        reset_core();
        booted = 0;
        char base[1024];
        const char *sl = strrchr(name, '/');
        snprintf(base, sizeof(base), "%s", sl ? sl + 1 : name);
        load_firmware_data(vfiles[i].data, vfiles[i].size, base);
        if (!fwLoaded) return 0;
        emu_boot();
        return 1;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int emu_firmware(uint8_t *data, int len, const char *name) {
    reset_machine();
    booted = 0;
    load_firmware_data(data, (size_t)len, name);
    if (!fwLoaded) return 0;
    return 1; /* le boot attend emu_card_finish (carte) — éventuellement vide */
}

EMSCRIPTEN_KEEPALIVE
void emu_card_file(const char *path, uint8_t *data, int len) {
    vfiles_add(path, data, (size_t)len);
}

EMSCRIPTEN_KEEPALIVE
void emu_card_image(uint8_t *data, int len) {
    sd_unload();
    sd_image = malloc(len ? (size_t)len : 1);
    memcpy(sd_image, data, (size_t)len);
    sd_size = (size_t)len;
    printf("carte SD : image (%d Kio)\n", len / 1024);
}

EMSCRIPTEN_KEEPALIVE
int emu_zip_load(uint8_t *data, int len) {
    int r = zip_load_card(data, (size_t)len);
    if (r == 2) emu_boot();
    return r;
}

EMSCRIPTEN_KEEPALIVE
void emu_card_finish(void) {
    /* les vfiles viennent d'être ajoutées après le reset_machine() de
     * emu_firmware : sd_unload() les viderait juste avant la construction */
    if (nvfiles > 0) fat_build_from_vfiles();
    emu_boot();
}

static Uint32 wasmFrame = 0;
static unsigned seenEpoch = 0;
static double wasmLastFrame = 0;
static Uint32 hudMs; /* dernier push du HUD dans la page */

static int wasmPaused; /* bouton pause de la page */

EMSCRIPTEN_KEEPALIVE
void emu_pause(int on) { wasmPaused = on ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE
void emu_pause_toggle(void) { wasmPaused = !wasmPaused; } /* source de vérité unique : la page relit emu_paused() */

EMSCRIPTEN_KEEPALIVE
int emu_paused(void) { return wasmPaused; }

EMSCRIPTEN_KEEPALIVE
void emu_restart(void) {
    if (fwLoaded) fw_restart(); /* ⏹ de la page : redémarre le jeu courant */
}

static void wasm_loop(void) {
    if (!poll_events()) emscripten_cancel_main_loop();
    update_diagnostics(wasmFrame);
    if (machineEpoch != seenEpoch) {
        /* un drop a réinitialisé la machine : resynchronise le pas de frame */
        seenEpoch = machineEpoch;
        emu_nextFrameTick = tickCount + frame_ticks();
        titleTick = tickCount;
        titleMs = (Uint32)emscripten_get_now();
    }
    /* une frame émulée par rAF (59,94 Hz ≈ 59,73) ; en retard de plus de
     * 4 frames (onglet caché, stall) : pas de rattrapage */
    double now = emscripten_get_now();
    const double frameMs = 16.743;
    if (wasmLastFrame == 0) wasmLastFrame = now;
    int due = (int)((now - wasmLastFrame) / frameMs);
    if (due > 4) { due = 1; wasmLastFrame = now; }
    if (fwLoaded && booted && !wasmPaused) {
        for (int i = 0; i < due; i++) run_emulated_frame();
        wasmFrame += (Uint32)due;
    }
    if (emuTarget == TGT_POKITTO) pk_adc_frame();
    wasmLastFrame += due * frameMs;
    if (now - wasmLastFrame > frameMs) wasmLastFrame = now;
    update_title_pct();
    audio_resume_when_ready();
    /* % en haut à droite (derrière le canvas, visible dans les bandes) et
     * titre du jeu dans le texte du bas (mise à jour au rythme du %) */
    if (fwLoaded && titleMs != hudMs && titleBuf[0]) {
        hudMs = titleMs;
        EM_ASM({ const p = document.getElementById('pct');
                 if (p) { p.textContent = $0;   /* $0 = int */
                          p.style.display = 'block'; }
                 const g = document.getElementById('gamename');
                 if (g) g.textContent = UTF8ToString($1);
               }, titlePct, fwName);
    }
    blit(emuRen);
}

EMSCRIPTEN_KEEPALIVE
int main(void) {
    memset(sram, 0xff, SRAM_SIZE); /* comme le TS (constructeur Atsamd21) */
#ifdef WASM_DEBUG
    hashMode = 1; /* diagnostic : hachages d'état sur stderr */
    hashInterval = 5000000; /* 1 marque / 5 s murales à pleine vitesse */
    ihashOn = 1;
#else
    hashMode = 0; /* diagnostics natifs hors wasm (getenv y est muet) */
#endif
    titleMs = (Uint32)emscripten_get_now();
    titleTick = tickCount;
    if (sdl_init_all() != 0) return 1;
    emscripten_set_main_loop(wasm_loop, 0, 1);
    return 0;
}
#endif
