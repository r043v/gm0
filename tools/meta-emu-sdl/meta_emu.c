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
 */
#include <SDL.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* ---------------------------------------------------------------- état */

#define FLASH_SIZE 0x40000u
#define SRAM_SIZE  0x8000u
#define SCREEN_W   160u
#define SCREEN_H   128u

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

/* périphériques */
static uint32_t portA_out, portB_out, portA_dir, portB_dir;
static uint8_t  ser4_data = 0x80;
static uint8_t  buttonData = 0xff;

/* TC4 + DAC */
static int      tc4Enabled, tc4Armed;
static uint32_t tc4Top, tc4Counter, tc4Period = 907;
static uint32_t tc4Window, tc4Fires, tc4Writes;
static int      tc4Interrupt;

/* ST7735 */
static uint16_t pix[SCREEN_W * SCREEN_H];
static int lcd_xStart, lcd_xEnd, lcd_yStart, lcd_yEnd, lcd_x, lcd_y;
static int lcd_argIndex, lcd_lastCommand, lcd_tmp;
static int ramwrCount; /* compte les RAMWR : ~32 par frame rendue */

/* carte SD (PA27) */
static uint8_t *sd_image = NULL;
static size_t   sd_size = 0;
static uint8_t  sd_pending = 0xff;
static uint8_t  sd_out[512 + 16];
static int      sd_outLen = 0, sd_outPos = 0;
static uint8_t  sd_cmdBuf[6];
static int      sd_cmdIdx = 0;
static int      sd_writing = 0, sd_writeIdx = 0, sd_writeLba = 0;
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

static SDL_AudioDeviceID audioDev;
static int audioOk;
static FILE *wavFile;
static uint32_t wavSamples;
static char wavPathStr[512];
static char shotPath[512];
static uint32_t maxFrames;

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

static void sd_command(uint8_t cmd, uint32_t arg) {
    switch (cmd) {
        case 0:  sd_out[sd_outLen++] = sd_initialized ? 0x00 : 0x01; break; /* idle */
        case 8:  sd_out[sd_outLen++] = 0x01; sd_out[sd_outLen++] = 0x00;
                 sd_out[sd_outLen++] = 0x00; sd_out[sd_outLen++] = 0x01;
                 sd_out[sd_outLen++] = 0xaa; break;
        case 55: sd_out[sd_outLen++] = 0x01; break;
        case 41: sd_initialized = 1; sd_out[sd_outLen++] = 0x00; break;
        case 58: sd_out[sd_outLen++] = 0x00; sd_out[sd_outLen++] = 0xc0;
                 sd_out[sd_outLen++] = 0x00; sd_out[sd_outLen++] = 0x00;
                 sd_out[sd_outLen++] = 0x00; break;
        case 16: sd_out[sd_outLen++] = 0x00; break;
        case 17: { /* lecture d'un secteur */
            size_t base = (size_t)arg * 512;
            uint8_t *card = sd_card_data();
            if (card && base + 512 <= sd_card_size()) {
                sd_out[sd_outLen++] = 0x00;
                sd_out[sd_outLen++] = 0xfe;
                memcpy(sd_out + sd_outLen, card + base, 512);
                sd_outLen += 512;
                sd_out[sd_outLen++] = 0xff; sd_out[sd_outLen++] = 0xff;
            } else {
                sd_out[sd_outLen++] = 0x04;
            }
            break;
        }
        case 24: /* écriture : token 0xfe + 512 + crc2 puis réponse/busy */
            sd_writeLba = arg;
            sd_writeIdx = 0;
            sd_writing = 1;
            sd_out[sd_outLen++] = 0x00;
            break;
        default: sd_out[sd_outLen++] = 0x04; break;
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
            sd_out[sd_outLen++] = 0x05; sd_out[sd_outLen++] = 0x00;
            sd_out[sd_outLen++] = 0x00; sd_out[sd_outLen++] = 0xff;
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
static uint32_t fatSpc = 8;         /* secteurs / cluster (dynamique) */
static uint32_t fatFatsz = 65;      /* secteurs / FAT (dynamique) */
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
    for (int i = 0; i < clusters; i++) {
        uint32_t c = first + (uint32_t)i;
        uint16_t v = (i == clusters - 1) ? 0xffff : (uint16_t)(c + 1);
        fatTable[c * 2] = v & 0xff; fatTable[c * 2 + 1] = v >> 8;
    }
    fatNext += (uint32_t)clusters;
    return first;
}

static uint32_t fat_data_lba(uint32_t first) {
    return 1 + 2 * fatFatsz + (FAT_ROOT * 32 + 511) / 512 + (first - 2) * fatSpc;
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
            FILE *g = fopen(ents[i].path, "rb");
            if (g) {
                uint8_t *data = malloc((size_t)clusters * fatSpc * FAT_SECTOR);
                memset(data, 0, (size_t)clusters * fatSpc * FAT_SECTOR);
                size_t got = fread(data, 1, ents[i].size, g);
                (void)got;
                memcpy(fatImage + fat_data_lba(ents[i].first) * FAT_SECTOR, data,
                       (size_t)clusters * FAT_SPC * FAT_SECTOR);
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
    uint32_t fatsz = (uint32_t)(((clusters + 2) * 2 + FAT_SECTOR - 1) / FAT_SECTOR);
    uint32_t total = 1 + 2 * fatsz + 32 + clusters * spc;
    fatSpc = spc;
    fatFatsz = fatsz;
    fatTotalSectors = (int)total;
    fatImageSize = (size_t)total * FAT_SECTOR;
    fatImage = calloc(1, fatImageSize);
    fatTable = calloc((size_t)fatsz * FAT_SECTOR, 1);
    fatTable[0] = 0xf8; fatTable[1] = 0xff;
    fatTable[2] = 0xff; fatTable[3] = 0xff;
    fatImage[0] = 0xeb; fatImage[1] = 0x3c; fatImage[2] = 0x90;
    memcpy(fatImage + 3, "GBREMU", 6);
    fatImage[0x0b] = 0x00; fatImage[0x0c] = 0x02; /* 512 octets/secteur */
    fatImage[0x0d] = (uint8_t)spc;
    fatImage[0x0e] = 0x01; fatImage[0x0f] = 0x00;
    fatImage[0x10] = 0x02;
    fatImage[0x11] = FAT_ROOT & 0xff; fatImage[0x12] = FAT_ROOT >> 8;
    fatImage[0x15] = 0xf8;
    fatImage[0x16] = fatsz & 0xff; fatImage[0x17] = (fatsz >> 8) & 0xff;
    fatImage[0x18] = 32; fatImage[0x19] = 0;   /* secteurs/piste */
    fatImage[0x1a] = 8; fatImage[0x1b] = 0;    /* têtes */
    /* FAT16 : total en 16 bits (0x13) sous 65536 secteurs, sinon en 32
     * (0x20) — le driver du firmware ne lit que le champ 16 bits */
    if (total < 65536u) {
        fatImage[0x13] = total & 0xff; fatImage[0x14] = (total >> 8) & 0xff;
    } else {
        fatImage[0x20] = total & 0xff; fatImage[0x21] = (total >> 8) & 0xff;
        fatImage[0x22] = (total >> 16) & 0xff; fatImage[0x23] = (total >> 24) & 0xff;
    }
    fatImage[510] = 0x55; fatImage[511] = 0xaa;
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
    uint32_t rootLba = 1 + 2 * fatsz;
    memcpy(fatImage + rootLba * FAT_SECTOR, rootBuf, FAT_ROOT * 32);
    memcpy(fatImage + FAT_SECTOR, fatTable, fatsz * FAT_SECTOR);
    memcpy(fatImage + (1 + fatsz) * FAT_SECTOR, fatTable, fatsz * FAT_SECTOR);
    free(rootBuf); free(rootEnts); rootEnts = NULL; rootN = 0;
    if (getenv("FAT_DUMP")) {
        FILE *g = fopen(getenv("FAT_DUMP"), "wb");
        if (g) { fwrite(fatImage, 1, fatImageSize, g); fclose(g);
                 printf("image FAT test : %s\n", getenv("FAT_DUMP")); }
    }
    printf("carte SD : %s (%d fichiers)\n", label, fatFileCount);
}

static void fat_build_from_dir(const char *dir) {
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
                if (g) { fwrite(data, 1, 512, g); fclose(g); }
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

/* --------------------------------------------------- ST7735 -> pixels */

static long stWrites = 0, ramwrTotal = 0;
static uint8_t st7735_byte(uint8_t v) {
    if (portB_out & (1u << 22)) return 0xff; /* CS écran haut */
    stWrites++;
    if (!(portB_out & (1u << 23))) { /* commande */
        lcd_lastCommand = v;
        lcd_argIndex = 0;            /* comme st7735.ts : reset à chaque commande */
        if (v == 0x2c) ramwrTotal++;
        return 0xff;
    }
    { /* données */
        switch (lcd_lastCommand) {
            case 0x2c: /* RAMWR */
                if (lcd_argIndex % 2 == 0) lcd_tmp = v;
                else {
                    uint16_t p = (uint16_t)((lcd_tmp << 8) | v);
                    if (lcd_x < SCREEN_W && lcd_y < SCREEN_H) pix[lcd_y * SCREEN_W + lcd_x] = p;
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
static uint32_t periph_read(uint32_t a, int *handled) {
    *handled = 0;
    if ((a & ~0x1fu) == 0x41004400u) { *handled = 1; return port_read(0, a & 0x1f); }
    if ((a & ~0x1fu) == 0x41004480u) { *handled = 1; return port_read(1, a & 0x1f); }
    if (a == 0x42001818u || a == 0x42001c18u) { *handled = 1; return 0x07; } /* SERCOM4/5 INTFLAG */
    if (a == 0x42001828u) { *handled = 1; return ser4_data; }                /* SERCOM4 DATA */
    if (a == 0x42001c28u) { *handled = 1; return 0x80; }                     /* SERCOM5 DATA */
    if (a == 0x4100484eu) { *handled = 1; return 0x02; }                     /* DMAC CHINTFLAG TCMPL */
    return 0;
}

static uint32_t fetchWord(uint32_t a) {
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
    if (a < 0x20000000u) { if (a + 2 > FLASH_SIZE) return 0;
        return (uint16_t)(flash[a] | (flash[a+1] << 8)); }
    if (a < 0x40000000u) { a -= 0x20000000u; if (a + 2 > SRAM_SIZE) return 0;
        return (uint16_t)(sram[a] | (sram[a+1] << 8)); }
    /* demi-mot : le TS ne consulte que le hack ADC RESULT */
    if (a == 0x4200401au) return (uint16_t)adc_random();
    return 0;
}

static uint8_t fetchByte(uint32_t a) {
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

static int dbgTc4Cfg = 0, dbgDac = 0;
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
static void writeWord(uint32_t a, uint32_t v) {
    if (a == millisWatchAddr) millisWrites++;
    if (watchAddr && a == watchAddr && usbWatch < 40)
        fprintf(stderr, "[watch %x] tick=%u pc=%x val=%08x\n", a, tickCount, regs[15] - 2, v);
    if (a < 0x20000000u) return;
    if (a < 0x40000000u) { a -= 0x20000000u; if (a + 4 > SRAM_SIZE) return;
        sram[a] = v & 0xff; sram[a+1] = (v >> 8) & 0xff;
        sram[a+2] = (v >> 16) & 0xff; sram[a+3] = (v >> 24) & 0xff; return; }
    if ((a & ~0x1fu) == 0x41004400u) { port_write(0, a & 0x1f, v); return; }
    if ((a & ~0x1fu) == 0x41004480u) { port_write(1, a & 0x1f, v); return; }
    if (a == 0x42001828u) { sercom4_write((uint8_t)v); return; } /* SERCOM4 DATA */
    if (a == 0x42004808u) { dac_write((uint16_t)v); return; }    /* DAC DATA */
    if (a == 0x42003000u) { if (dbg() && dbgTc4Cfg < 8) { fprintf(stderr, "[dbg] CTRLA word <- %x\n", v); dbgTc4Cfg++; } tc4Enabled = (v & 0x02) != 0; if (!tc4Enabled) tc4Counter = 0; return; }
    if (a == 0x42003018u) { tc4Top = v; return; }                /* TC4 CC0 */
    if (a == 0x4200300du) { if (v & 0x10) tc4Armed = 1; return; }/* TC4 INTENSET */
    if (a == 0x41004834u) { dmac_baseAddr = v; return; }
    if (a == 0x41004838u) { dmac_wrbAddr = v; return; }
    if (a == 0x4100483fu) { dmac_chid = v; return; }
    if (a == 0x41004840u) { /* CHCTRLA == 2 : transfert via descripteur */
        if (v == 0x02) {
            if (!dmac_desc) dmac_desc = dmac_baseAddr + dmac_chid * 0x10;
            uint16_t btcnt = fetchHalf(dmac_desc + 0x02);
            uint32_t src = fetchWord(dmac_desc + 0x04);
            uint32_t dst = fetchWord(dmac_desc + 0x08);
            uint32_t nxt = fetchWord(dmac_desc + 0x0c);
            for (uint16_t i = 0; i < btcnt; i++)
                writeByte(dst, fetchByte(src + i - btcnt));
            dmac_desc = nxt;
            dmacInterrupt = 1; /* verrouillé, traité au prochain step (TS) */
        }
        return;
    }
}

static void writeHalf(uint32_t a, uint16_t v) {
    if (a < 0x20000000u) return;
    if (a < 0x40000000u) { a -= 0x20000000u; if (a + 2 > SRAM_SIZE) return;
        sram[a] = v & 0xff; sram[a+1] = (v >> 8) & 0xff; return; }
    if (a == 0x42004808u) { if (dbg() && dbgDac < 3) { fprintf(stderr, "[dbg] DAC half <- %x\n", v); dbgDac++; } dac_write(v); return; }
    if (a == 0x42003000u) { if (dbg() && dbgTc4Cfg < 8) { fprintf(stderr, "[dbg] CTRLA half <- %x\n", v); dbgTc4Cfg++; } tc4Enabled = (v & 0x02) != 0; if (!tc4Enabled) tc4Counter = 0; return; }
    if (a == 0x42003018u) { tc4Top = v; return; }
    if (a == 0x4200300du) { if (v & 0x10) tc4Armed = 1; return; }
    if (a == 0x40000c02u) return;                                /* GCLK CLKCTRL */
    if ((a & ~0x1fu) == 0x41004400u) { port_write(0, a & 0x1f, v); return; }
    if ((a & ~0x1fu) == 0x41004480u) { port_write(1, a & 0x1f, v); return; }
    writeWord(a, v);
}

static void writeByte(uint32_t a, uint8_t v) {
    if (a < 0x20000000u) return;
    if (a < 0x40000000u) { uint32_t sa = a - 0x20000000u; if (sa < SRAM_SIZE) sram[sa] = v; return; }
    if (a == 0x4200300du) { if (v & 0x10) tc4Armed = 1; return; } /* TC4 INTENSET */
    if (a == 0x4200300eu) return;                                 /* TC4 INTFLAG */
    if (a == 0x42001828u) { sercom4_write(v); return; }
    if (a == 0x4100483fu) { dmac_chid = v; return; }
    if ((a & ~0x1fu) == 0x41004400u) { port_write(0, a & 0x1f, v); return; }
    if ((a & ~0x1fu) == 0x41004480u) { port_write(1, a & 0x1f, v); return; }
    writeWord(a, v);
}

/* --------------------------------------------------- DMAC (écran) */


/* ---------------------------------------------------- SERCOM4 data */

static void buttons_apply(void);

static void sercom4_write(uint8_t v) {
    /* ordre du TypeScript (écran, boutons, carte SD) ; l'octet de réponse
     * repart à 0x80 à chaque échange, les périphériques sélectionnés le
     * remplacent (sercom-register.ts : this.data = 0x80 puis listeners) */
    ser4_data = 0x80;
    st7735_byte(v);
    if ((portB_out & (1u << 3)) == 0) ser4_data = buttonData; /* boutons PB03 */
    sd_byte(v);
}

/* ---------------------------------------------------------------- CPU */

static void incrementPc(void) {
    sysTickTrigger++;
    tickCount++;
    regs[15] += 2;

    if (tc4Enabled && tc4Armed && tc4Top > 0) {
        /* régulateur du TS : la carte doit produire ~369 échantillons par
         * frame ; aucun attente -> rapproche les tirs, trop -> les espace */
        tc4Window++;
        if (tc4Window >= 4000000u) {
            uint32_t holds = tc4Fires - tc4Writes;
            if (holds == 0 && tc4Period > 300) tc4Period -= 10;
            else if (holds > tc4Fires * 0.6 && tc4Period < 20000) tc4Period += 10;
            tc4Window = 0; tc4Fires = 0; tc4Writes = 0;
        }
        if (++tc4Counter >= tc4Period) {
            tc4Counter = 0;
            tc4Fires++;
            tc4Interrupt = 1;
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
    for (int i = 0; i < SRAM_SIZE; i++) { h = (h ^ sram[i]) * 0x01000193u; }
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

static void step(void) {
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
    stepNo++;
    if (tc4Interrupt) {
        tc4Interrupt = 0;
        irq_inject(tc4Vector);
    }
    if (dmacInterrupt) {
        dmacInterrupt = 0;
        irq_inject(dmacVector);
    }
    else if (sysTickTrigger >= 20000) { /* 1 ms émulée (hack du TS) */
        sysTickTrigger = 0;
        sysTickEntries++;
        irq_inject(sysTickVector);
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
        uint32_t cnvz = fetchWord(regs[13]);
        fC = (cnvz & 1) != 0; fN = (cnvz & 2) != 0;
        fV = (cnvz & 4) != 0; fZ = (cnvz & 8) != 0;
        regs[13] += 4;
        instAddr = regs[15] - 2;
    }
    if (instAddr >= 0x42000000u) { /* PC fou : le TS planterait ici — log fort */
        static int wildLogged = 0;
        if (wildLogged < 10)
            fprintf(stderr, "[pc fou] tick=%u pc=%x lr=%x sp=%x\n",
                    tickCount, instAddr, regs[14], regs[13]);
        wildLogged++;
        regs[15] = vectorBase + fetchWord(vectorBase + 4);
        return;
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
            /* décalages par registre : compte masqué à 5 bits et retenue
             * calculée sur le COMPTEUR comme le TS (quirk fidèle : le TS
             * teste readRegister(rs), pas la valeur décalée) */
            case 0x2: { uint32_t r = a << (b & 31); setReg(rd, r);
                        fC = (b & (1u << (b & 31))) != 0; setNZ(r); break; }
            case 0x3: { uint32_t r = a >> (b & 31); setReg(rd, r);
                        fC = (b & (1u << ((32 - b) & 31))) != 0; setNZ(r); break; }
            case 0x4: { uint32_t r = (uint32_t)((int32_t)a >> (b & 31));
                        setReg(rd, r);
                        fC = (b & (1u << ((32 - b) & 31))) != 0; setNZ(r); break; }
            case 0x5: setReg(rd, addSetCond(a, b, fC)); break;                 /* ADC */
            case 0x6: setReg(rd, addSetCond(a, ~b, fC)); break;                /* SBC */
            case 0x8: setNZ(a & b); break;                                     /* TST */
            case 0x9: setReg(rd, addSetCond(0, ~b, 1)); break;                 /* NEG */
            case 0xa: addSetCond(a, ~b, 1); break;                             /* CMP reg */
            case 0xb: addSetCond(a, b, 0); break;                              /* CMN */
            case 0xc: setReg(rd, a | b); setNZ(regs[rd]); break;               /* ORR */
            case 0xd: { /* MUL : le TS laisse le produit NON masqué (double
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
                        break; }
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
            case 0xe: case 0xf: /* BLX r<rm> */
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
    else if ((op & 0xf800) == 0xf000) { /* BL : second demi-mot lu paresseusement */
        uint16_t nextInst = fetchHalf(instAddr + 2);
        if ((nextInst & 0xf800) == 0xf800) {
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
        }
        /* demi-mot 0xf0xx isolé : non décodé, on continue (comme le TS) */
    }
    else if (op == 0xf3bf || (op & 0xffe0) == 0xf3e0) { /* DMB/MRS : no-op ; le TS ajoute un tick pour DMB seul */
        if (op == 0xf3bf) incrementPc();
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
static uint32_t px32[SCREEN_W * SCREEN_H];

static void blit(SDL_Renderer *ren) {
    for (unsigned i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint16_t p = pix[i];
        uint8_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
        /* même expansion que st7735.ts : décalages, pas de mise à l'échelle */
        px32[i] = 0xff000000u | ((b << 3) << 16) | ((g << 2) << 8) | (r << 3);
    }
    SDL_UpdateTexture(tex, NULL, px32, SCREEN_W * sizeof(uint32_t));
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

/* audio SDL : l'ISR écrit dans aq ; le callbackSDL consomme */
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    for (int i = 0; i < len / 2; i++) {
        if (aq_head != aq_tail) {
            audioHold = aq[aq_head];
            aq_head = (aq_head + 1) % AQ_SIZE;
        }
        out[i] = audioHold;
    }
}

/* boutons : bit0 bas, 1 gauche, 2 droite, 3 haut, 4 A, 5 B, 6 MENU, 7 HOME
 * (actifs bas : 0 = enfoncé) */
#define BTN_DOWN   (1u << 0)
#define BTN_LEFT   (1u << 1)
#define BTN_RIGHT  (1u << 2)
#define BTN_UP     (1u << 3)
#define BTN_A      (1u << 4)
#define BTN_B      (1u << 5)
#define BTN_MENU   (1u << 6)
#define BTN_HOME   (1u << 7)
#define BTN_DIRMASK (BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_UP)

static uint32_t homeHeld;
static unsigned machineEpoch; /* incrémenté quand un drop réinitialise la machine */

static uint8_t key_bit(SDL_Keycode sym) {
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
    buttonData &= (uint8_t)~mask;
    if (mask & BTN_HOME) homeHeld = SDL_GetTicks();
}

static void btn_release(uint8_t mask) {
    buttonData |= mask;
    if (mask & BTN_HOME) homeHeld = 0;
}

/* manette (SDL_GameController) */
static uint8_t pad_button_mask(uint8_t b) {
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
        case 2: return BTN_MENU;
        case 3: return BTN_HOME;
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

#define EMU_FRAME_TICKS 334860u /* 16743 µs émulées */
static uint32_t emu_nextFrameTick = EMU_FRAME_TICKS;
static Uint32 titleMs;
static uint32_t titleTick;
static char titleBuf[1200]; /* dernier titre construit (HUD wasm) */

/* décharge la carte SD courante (image ou image FAT d'un répertoire) */
static void sd_unload(void) {
    free(sd_image); sd_image = NULL; sd_size = 0;
    free(fatImage); fatImage = NULL; fatImageSize = 0;
    free(fatTable); fatTable = NULL;
    fatFileCount = 0; fatNext = 2;
    vfiles_reset();
    sd_reset_state();
    sd_initialized = 0;
}

/* réinitialise la machine en conservant la carte SD montée
 * (changement de jeu depuis le sélecteur) */
static void reset_core(void) {
    memset(sram, 0xff, SRAM_SIZE);
    memset(regs, 0, sizeof regs);
    memset(regD, 0, sizeof regD);
    fN = fZ = fC = fV = 0;
    tickCount = 0; sysTickTrigger = 0; sysTickEntries = 0;
    sysTickVector = dmacVector = tc4Vector = 0;
    dmacInterrupt = tc4Interrupt = 0;
    dmac_baseAddr = dmac_wrbAddr = dmac_desc = dmac_chid = 0;
    portA_out = portB_out = portA_dir = portB_dir = 0;
    ser4_data = 0x80;
    tc4Enabled = tc4Armed = 0;
    tc4Top = tc4Counter = 0; tc4Period = 907;
    tc4Window = tc4Fires = tc4Writes = 0;
    lcd_xStart = lcd_xEnd = lcd_yStart = lcd_yEnd = lcd_x = lcd_y = 0;
    lcd_argIndex = lcd_lastCommand = lcd_tmp = 0;
    memset(pix, 0, sizeof pix);
    millisWrites = 0;
    aq_head = aq_tail = 0; audioHold = 0;
    emu_nextFrameTick = tickCount + EMU_FRAME_TICKS;
    sd_reset_state(); /* pas de transaction SD résiduelle pour le jeu suivant */
    sd_initialized = 0; /* le firmware suivant rejoue toute l'init (CMD0 doit
                         * répondre « idle ») */
}

/* réinitialise toute la machine (drop d'un nouveau firmware) */
static void reset_machine(void) {
    reset_core();
    sd_unload();
}

static void load_firmware_data(const uint8_t *data, size_t len, const char *display) {
    memset(flash, 0xff, FLASH_SIZE); /* comme le TS : flash remplie de 0xff */
    size_t n = len < (FLASH_SIZE - 0x4000) ? len : (FLASH_SIZE - 0x4000);
    if (n == 0) { fprintf(stderr, "firmware vide\n"); return; }
    memcpy(flash + 0x4000, data, n);
    snprintf(fwPath, sizeof(fwPath), "%s", display);
    const char *b = strrchr(fwPath, '/');
    fwName = b ? b + 1 : fwPath;
    fwLoaded = 1;
    printf("firmware : %s (%zu Ko)\n", display, len / 1024);
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
     * passée en ligne de commande ne rebind pas au premier lancement */
    if (fwLoaded && (rebindCard || !sd_explicit)) {
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
    if (fwLoaded) snprintf(title, sizeof(title), "Gamebuino META — %.900s", fwName);
    else snprintf(title, sizeof(title), "Gamebuino META — déposez un firmware .bin");
    SDL_SetWindowTitle(emuWin, title);
}

static void audio_start(void) {
    if (audioOk) SDL_PauseAudioDevice(audioDev, 0);
}

/* ------------------------------------------------ SDL/HTML5 ------------ */

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
    if (SDL_CreateWindowAndRenderer(SCREEN_W * 2, SCREEN_H * 2,
                                    SDL_WINDOW_RESIZABLE, &emuWin, &emuRen) != 0) {
        fprintf(stderr, "fenêtre: %s\n", SDL_GetError());
        return 1;
    }
    /* échelle logique 160×128 : redimensionnable, rendu entier, centré */
    SDL_RenderSetLogicalSize(emuRen, SCREEN_W, SCREEN_H);
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
                            SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);

    SDL_AudioSpec want, got;
    memset(&want, 0, sizeof(want));
    want.freq = 22049; want.format = AUDIO_S16SYS; want.channels = 1;
    want.samples = 1024; want.callback = audio_cb;
    audioDev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    audioOk = audioDev != 0;
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
        double emuMs = (double)(tickCount - titleTick) / 20000.0;
        double wallMs = (double)(nowMs - titleMs);
        int pct = wallMs > 0.0 ? (int)(emuMs / wallMs * 100.0 + 0.5) : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100; /* jamais plus vite que le temps réel */
        snprintf(titleBuf, sizeof(titleBuf), "Gamebuino META — %.900s — %d%%", fwName, pct);
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "Gamebuino META — déposez un firmware .bin");
    }
    SDL_SetWindowTitle(emuWin, titleBuf);
    titleMs = nowMs;
    titleTick = tickCount;
}

static void run_emulated_frame(void) {
    uint32_t target = emu_nextFrameTick;
    while (tickCount < target) step();
    emu_nextFrameTick += EMU_FRAME_TICKS;
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
            uint32_t rate = 22049;
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
            emu_nextFrameTick = tickCount + EMU_FRAME_TICKS;
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

        update_title_pct();

        blit(emuRen);
        if (trace) { /* empreinte d'écran périodique */
            static uint32_t lastPrint = 0;
            if (frame - lastPrint >= 60) {
                lastPrint = frame;
                uint32_t h = 0x811c9dc5;
                int distinct = 0;
                uint16_t seen[16] = {0};
                for (unsigned i = 0; i < SCREEN_W * SCREEN_H; i++) {
                    h = (h ^ pix[i]) * 0x01000193u;
                    int k = 0;
                    for (; k < 16; k++) if (seen[k] == pix[i]) break;
                    if (k == 16) distinct++;
                }
                fprintf(stderr, "[frame %u] hash=%08x distinct<=%d\n", frame, h, distinct);
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
            fprintf(sf, "P6\n%u %u\n255\n", SCREEN_W, SCREEN_H);
            for (unsigned i = 0; i < SCREEN_W * SCREEN_H; i++) {
                uint16_t pc = pix[i];
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

static void wasm_loop(void) {
    if (!poll_events()) emscripten_cancel_main_loop();
    update_diagnostics(wasmFrame);
    if (machineEpoch != seenEpoch) {
        /* un drop a réinitialisé la machine : resynchronise le pas de frame */
        seenEpoch = machineEpoch;
        emu_nextFrameTick = tickCount + EMU_FRAME_TICKS;
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
    if (fwLoaded && booted) {
        for (int i = 0; i < due; i++) run_emulated_frame();
        wasmFrame += (Uint32)due;
    }
    wasmLastFrame += due * frameMs;
    if (now - wasmLastFrame > frameMs) wasmLastFrame = now;
    update_title_pct();
    /* titre + % visibles dans la page (mise à jour au rythme du %) */
    if (fwLoaded && titleMs != hudMs && titleBuf[0]) {
        hudMs = titleMs;
        EM_ASM({ const el = document.getElementById('hud');
                 if (el) { el.textContent = UTF8ToString($0);
                           el.style.display = 'block'; } }, titleBuf);
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
