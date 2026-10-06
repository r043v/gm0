/*
 * gm0.c — émulateur Gamebuino META (SAMD21 / Cortex-M0+) et Pokitto en
 * C/SDL2 + WebAssembly.  Né d'un port du fork TypeScript gamebuino-emulator ;
 * depuis 2026-10-05, le vrai matériel prime (README « Fidélité matérielle ») :
 *  - Cortex-M0+ à 48 MHz compté en cycles (cache NVM compris), NVIC fidèle ;
 *  - TC4/TC5 à la cadence exacte de leur config, DAC, DMAC (canaux
 *    déclenchés, descripteurs chaînés, CHINTEN/SWRST/FERR) ;
 *  - SERCOM4 : ST7735 (12/16/18 bpp) cadencé au baud SPI, carte SD
 *    (CMD17/18/24/12), boutons ; image FAT construite d'un dossier ou zip ;
 *  - écran ST7735 160x128 rendu dans une fenêtre SDL2 x2 ;
 *  - boutons sur PB03 : flèches, J=A, K=B, U=MENU, I=HOME, Entrée=Start
 *    (HOME tenu 3 s = reset du jeu, comme sur la console).
 *
 * Usage : gm0 <firmware.bin> [carte.img] [--wav out.wav]
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
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>

/* Interface : anglais par défaut ; français avec -DGM0_FR (CMake -DGM0_FR=ON).
 * Les traces de débogage étiquetées [xxx] restent en français. */
#ifdef GM0_FR
#define TR(fr, en) fr
#else
#define TR(fr, en) en
#endif
static const char *fwName; /* firmware courant (clé de sauvegarde wasm) */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* persistance des fichiers de carte modifiés (sauvegardes) en localStorage,
 * clé par jeu : « emusav:<firmware>:<fichier> » */
EM_JS(void, em_ls_set, (const char *key, const uint8_t *data, int len), {
    try {
        const k = UTF8ToString(key);
        let s = '';
        const h = HEAPU8;
        for (let i = 0; i < len; i += 8192)
            s += String.fromCharCode.apply(null, h.subarray(data + i, Math.min(data + i + 8192, data + len)));
        localStorage.setItem(k, btoa(s));
    } catch (e) { /* quota ou privacy : la sauvegarde reste en RAM */ }
});
EM_JS(int, em_ls_get, (const char *key, uint8_t *out, int maxlen), {
    try {
        const v = localStorage.getItem(UTF8ToString(key));
        if (!v) return 0;
        const s = atob(v);
        const n = Math.min(s.length, maxlen);
        for (let i = 0; i < n; i++) HEAPU8[out + i] = s.charCodeAt(i);
        return n;
    } catch (e) { return 0; }
});
static const char *em_save_key(const char *path) {
    static char key[1200];
    snprintf(key, sizeof(key), "emusav:%s:%s", fwName ? fwName : "jeu", path);
    return key;
}
#endif

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
static int fN, fZ, fC, fV;
static uint32_t tickCount;
static int sysTickTrigger;
static uint32_t vectorBase;
static uint32_t sysTickVector, dmacVector, tc4Vector, tc5Vector;
static int dmacInterrupt, tc4Interrupt;
static int irqWork = 1; /* le NVIC a peut-être quelque chose à prendre (voir nvic_service) */
static long sysTickEntries;

/* périphériques — PA27 (CS carte SD) et PA25 (CS boutons) hauts dès le
 * boot : pull-ups réelles de la carte, les périphériques n'écoutent le
 * SPI que si le firmware les sélectionne (le boot initial ne passe PAS
 * par reset_core ; les jeux sans lib standard ne sélectionnent jamais
 * ce qu'ils n'utilisent pas — sinon leur trafic SPI écran partait dans
 * la machine SD) */
static uint32_t portA_out = (1u << 27) | (1u << 25), portB_out, portA_dir, portB_dir;
static uint8_t  ser4_data = 0x80;
/* SERCOM4 SPI temporisé (datasheet 26.6.2.6) : un octet dure 16 x (BAUD+1)
 * cycles ; le registre DATA écrit part au registre à décalage dès qu'il est
 * libre, la réception remplit un tampon de 2 octets que le code doit vider
 * (les octets que l'écran DMA y laisse restent : d'où les purges des
 * firmwares).  INTFLAG : DRE (place pour un octet de plus), TXC (tout est
 * sorti), RXC (un octet reçu disponible). */
static struct { uint8_t d; uint32_t done; } spiRx[2];
static int spiRxN;
static uint32_t spiLastDone, spiPrevDone; /* fins des deux derniers octets émis */
static int spiDmaWrite; /* écriture DATA faite par le DMAC (déjà cadencée) */
/* CTRLA.SWRST, CTRLA.ENABLE=0 ou CTRLB.RXEN=0 vident le tampon de réception
 * (datasheet 27.8.1-2) : l'Arduino SPI.config() fait un SWRST à chaque
 * changement de vitesse, c'est ce qui purge les octets laissés par l'écran
 * avant la lecture des boutons des jeux lib */
static void sercom4_ctrl_write(uint32_t a, uint32_t v) {
    if (a == 0x42001800u && ((v & 1u) || !(v & 2u))) spiRxN = 0;
    if (a == 0x42001804u && !(v & (1u << 17))) spiRxN = 0;
}
static uint8_t  buttonData = 0xff;

/* TC4 + DAC */
static int      tc4Enabled, tc4Armed;
static uint8_t  tc4IntEnMask, tc4IntFlagMask; /* INTENSET/INTFLAG lisibles (jeux maison) */
static int      primask; /* CPSID/CPSIE : masque les injections d'interruptions */
static int      sysTickCountFlag; /* SysTick CSR.COUNTFLAG : wrap CVR depuis la dernière lecture */
static uint32_t tc4CtrlA; /* valeur complète de CTRLA (prescaler bits 8-10) */
static uint32_t tc4Top, tc4Counter, tc4Period = 907;
static uint32_t tc4Fires, tc4Writes;
static uint32_t audStarvedTicks, audRestarts; /* EMU_AUDIO_STATS */
static int      tc4Interrupt;

/* TC5 (0x42003400, IRQ20) : l'audio de la lib officielle (Sound::begin →
 * tcConfigure, TC5_Handler = Audio_Handler, INTENSET.MC0, CC0 =
 * 48 MHz/SOUND_FREQ − 1).  Sound_Handler_Wav::update stream les WAV
 * depuis cette ISR : sans elle, les jeux lib à musique (Picomon) vident
 * leur tampon et attendent pour toujours — écran noir après le titre. */
static int      tc5Enabled, tc5Armed;
static uint8_t  tc5IntEnMask, tc5IntFlagMask;
static uint32_t tc5CtrlA, tc5Top, tc5Counter, tc5Period;
static uint32_t tc5Fires;
static int      tc5Interrupt;

/* ST7735 */
static uint16_t pix[MAX_SCREEN_W * MAX_SCREEN_H];
static int lcd_xStart, lcd_xEnd, lcd_yStart, lcd_yEnd, lcd_x, lcd_y;
static uint32_t nvmAddr;         /* NVMCTRL ADDR (0x41004008) */
static uint8_t nvmIntFlag = 1;   /* INTFLAG (0x41004014) : bit0 READY */
static int lcd_argIndex, lcd_lastCommand, lcd_tmp;

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
static int      sd_multi;      /* lecture multi-blocs (CMD18) en cours */
static uint32_t sd_multiLba;   /* prochain bloc à émettre */
static uint8_t  sd_cmdBuf[6];
static int      sd_cmdIdx = 0;
static int      sd_writing = 0, sd_writeIdx = 0, sd_writeLba = 0;

#define SD_OUT_MAX (1u << 20)
/* réserve n octets dans la file (croissance doublée, plafonnée) : 0 = plein
 * ou hors ressources — on n'accumule plus (déraillement) */
static int sd_out_reserve(int n) {
    if (sd_outLen + n > (int)SD_OUT_MAX) return 0;
    if (sd_outLen + n <= sd_outCap) return 1;
    int cap = sd_outCap ? sd_outCap : 512;
    while (cap < sd_outLen + n) cap *= 2;
    if (cap > (int)SD_OUT_MAX) cap = (int)SD_OUT_MAX;
    uint8_t *b = realloc(sd_out, (size_t)cap);
    if (!b) return 0;
    sd_out = b; sd_outCap = cap;
    return 1;
}
static void sd_out_push(uint8_t b) {
    if (sd_out_reserve(1)) sd_out[sd_outLen++] = b;
}
static void sd_out_append(const uint8_t *p, int n) {
    if (n <= 0 || !sd_out_reserve(n)) return;
    memcpy(sd_out + sd_outLen, p, (size_t)n);
    sd_outLen += n;
}
static int      sd_initialized = 0;
static uint8_t  sd_writeBuf[515];

/* audio SDL : anneau producteur (ISR) -> consommateur (callback) */
#define AQ_SIZE 65536
/* plafond de latence son/image : au-delà, on rattrape doucement (2 éch.
 * par poussée, cf. audio_push) ; une seule reprise franche au-delà de
 * +8192 (stall catastrophe).  La consommation étant calée 250 ppm sous
 * la production, l'anneau se réépaissit seul après toute perte hôte. */
/* Plafond calé sur le buffer réellement obtenu de SDL (aq_configure) :
 * le callback vide `cb` échantillons d'un coup et l'émulation produit par
 * rafales d'une frame (~370 éch.) — un plafond fixe de 900 sous un callback
 * de 1024 garantissait un trou à chaque appel (~15 % du son en relâches). */
static int aqLatency = 1500, aqPrebuf = 900;
#define AQ_LATENCY aqLatency
static void aq_configure(int cb, double devHz, double guestHz) {
    if (cb <= 0) cb = 1024;
    int burst = (int)(guestHz / 59.7275) + 1; /* échantillons produits par frame émulée */
    aqLatency = cb + 2 * burst + (int)(devHz * 0.012); /* callback + 2 rafales + 12 ms de gigue */
    if (aqLatency > AQ_SIZE / 2) aqLatency = AQ_SIZE / 2;
    aqPrebuf = cb + burst;
}
static int16_t aq[AQ_SIZE];
static volatile int aq_head, aq_tail; /* tail = écrit, head = lu */
static int16_t audioHold = 0;
static int audioPending; /* device armé en pause : attend le pré-buffer */
static volatile uint32_t audUnder, audCbSamples, audCbCalls; /* EMU_AUDIO_STATS (hôte) */

static SDL_AudioDeviceID audioDev;
static int audioOk;
static int audioDevStandard; /* le device courant lit audio_cb (file aq) ;
                              * pk_audio_reopen (HLE) le met à 0 */
static int devRate = 48000;      /* taux du périphérique hôte : fixe, ouvert une seule fois */
static int devCbSamples = 1024;
static FILE *wavFile;
static uint32_t wavSamples;
static char wavPathStr[512];
static char shotPath[512];
static uint32_t maxFrames;

/* ------------------------------------------------------- cibles ------- */

/* drapeau d'environnement lu une seule fois par site d'appel : plusieurs
 * tests de débogage étaient des getenv() exécutés à chaque octet SPI ou
 * chargement de descripteur (des millions d'appels par seconde) */
#define ENVFLAG(name) __extension__({ static int envf_ = -1; \
    if (envf_ < 0) { envf_ = getenv(name) ? 1 : 0; } envf_; })

#define TGT_META    0
#define TGT_POKITTO 1
static int emuTarget = TGT_META;   /* fixé par --target ou détection */
static int targetForced;           /* --target explicite */
static int armIrqEnable = 1;       /* PRIMASK inversé (CPSIE/CPSID, Pokitto) */

static unsigned SCR_W = 160, SCR_H = 128;

/* 1 tick = 1 cycle CPU ; Pokitto : l'horloge configurée par le guest
 * (pk_core_hz : IRC 12 MHz ou sortie PLL telle que programmée) */
static double emuTicksPerSec = 48000000.0;
static uint32_t emuTicksPerMs = 48000u; /* 1 ms émulée = 48000 cycles */
static uint32_t emuTicksPerUs = 48u;
/* Cache du NVMCTRL (datasheet 22.6.7, actif au reset : CTRLB.CACHEDIS=0) :
 * direct-mapped, 8 lignes de 64 bits.  Un défaut coûte l'état d'attente
 * RWS=1 du runtime à 48 MHz ; un succès, rien.  Instructions et données. */
static uint32_t nvmTag[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
static uint32_t flashWaits; /* états d'attente de l'instruction courante */
static inline void nvm_access(uint32_t a) {
    uint32_t line = (a >> 3) & 7u, tag = a >> 6;
    if (nvmTag[line] != tag) { nvmTag[line] = tag; flashWaits++; }
}


static double pk_core_hz(void); /* fréquence du cœur telle que configurée */
static double ticks_per_sec(void) {
    return emuTarget == TGT_POKITTO ? pk_core_hz() : emuTicksPerSec;
}
static uint32_t frame_ticks(void) {
    return (uint32_t)(ticks_per_sec() / 59.7275 + 0.5);
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
    if (ahead > AQ_LATENCY + 8192) /* stall catastrophe : reprise franche */
        aq_head = (aq_head + ahead - AQ_LATENCY) % AQ_SIZE;
    /* le surplus ordinaire est absorbé par la régulation de débit du
     * callback (audio_cb) : plus de rognage par à-coups */
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
    if (ahead >= aqPrebuf) audioPending = 0;
}

/* fréquence DAC du jeu : cadence TC4 réelle dans le domaine actif
 * (ticks_per_sec / période en ticks), légèrement sous-consommée pour
 * laisser l'anneau se remplir après toute perte hôte (stall, onglet).
 * La sortie SDL est (ré)ouverte à cette fréquence : consommer 22 049
 * fixe contre une production à ~22 004/s dérivait de 45 éch/s en
 * continu — l'anneau se vidait en ~14 s puis tenait en sous-débittest
 * (holds + trims AQ_LATENCY) : LA famine audible de lapinou, alors que
 * le contenu produit est complet (rendu hors-ligne identique). */
static double emuDacRate = 22049.0;
static void meta_audio_set_rate(double freq); /* défini avec le bloc SDL */

/* période TC4/TC5 en ticks émulés : prescale × (CC0+1) cycles du GCLK
 * 48 MHz, ramenés au domaine de ticks (× emuTicksPerUs/48, arrondi au
 * plus proche ; ×1 au domaine natif 48 MHz, ×5/12 dans le domaine TS) */
static uint32_t tc_period_ticks(uint32_t ctrlA, uint32_t top) {
    static const uint16_t prescTab[8] = {1, 2, 4, 8, 16, 64, 256, 1024};
    uint32_t cycles = (uint32_t)prescTab[(ctrlA >> 8) & 7u] * (top + 1u);
    uint32_t per = (uint32_t)(((uint64_t)cycles * emuTicksPerUs + 24u) / 48u);
    return per < 2 ? 2 : per;
}
/* mises en cache : ces périodes étaient recalculées (division 64 bits) à
 * chaque instruction émulée par incrementPc */
static uint32_t tc4_period_ticks(void) {
    static uint32_t cA = ~0u, cT = ~0u, cU = ~0u, cP;
    if (tc4CtrlA != cA || tc4Top != cT || emuTicksPerUs != cU) {
        cA = tc4CtrlA; cT = tc4Top; cU = emuTicksPerUs;
        cP = tc_period_ticks(cA, cT);
    }
    return cP;
}
static uint32_t tc5_period_ticks(void) {
    static uint32_t cA = ~0u, cT = ~0u, cU = ~0u, cP;
    if (tc5CtrlA != cA || tc5Top != cT || emuTicksPerUs != cU) {
        cA = tc5CtrlA; cT = tc5Top; cU = emuTicksPerUs;
        cP = tc_period_ticks(cA, cT);
    }
    return cP;
}

static double tc4_config_rate(void) {
    return ticks_per_sec() / (double)tc4_period_ticks();
}

/* écritures CTRLA/CC0/INTEN* des TC4/TC5 : la cadence DAC du jeu est
 * redérivée à chaque changement — un seul jeu d'aides (les blocs étaient
 * dupliqués en huit copies dans writeWord/writeHalf) */
static void tc4_rate_update(void) {
    if (!tc4Enabled || tc4Top == 0) return;
    double r = tc4_config_rate();
    if (r != emuDacRate) { emuDacRate = r; meta_audio_set_rate(r); }
}
static void tc5_rate_update(void) {
    if (!tc5Enabled || tc5Top == 0) return;
    double r = ticks_per_sec() / (double)tc5_period_ticks();
    if (r != emuDacRate) { emuDacRate = r; meta_audio_set_rate(r); }
}
static void tc4_write_ctrla(uint32_t v) {
    tc4CtrlA = v; tc4Enabled = (v & 0x02) != 0;
    if (!tc4Enabled) tc4Counter = 0;
    tc4_rate_update();
}
static void tc4_write_cc0(uint32_t v) { tc4Top = v; tc4_rate_update(); }
static void tc5_write_ctrla(uint32_t v) {
    tc5CtrlA = v; tc5Enabled = (v & 0x02) != 0;
    if (!tc5Enabled) tc5Counter = 0;
    tc5_rate_update();
}
static void tc5_write_cc0(uint32_t v) { tc5Top = v; tc5_rate_update(); }
static void tc_inten(int is5, uint32_t v) {
    if (is5) { tc5IntEnMask |= (uint8_t)v; tc5Armed = (tc5IntEnMask & 0x33) != 0; }
    else     { tc4IntEnMask |= (uint8_t)v; tc4Armed = (tc4IntEnMask & 0x33) != 0; }
}
static void tc_intclr(int is5, uint32_t v) {
    if (is5) { tc5IntEnMask &= (uint8_t)~v; tc5Armed = (tc5IntEnMask & 0x33) != 0; }
    else     { tc4IntEnMask &= (uint8_t)~v; tc4Armed = (tc4IntEnMask & 0x33) != 0; }
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
    printf(TR("WAV : %u échantillons (%.1f s à %.0f Hz)\n", "WAV: %u samples (%.1f s at %.0f Hz)\n"), wavSamples,
           (double)wavSamples / emuDacRate, emuDacRate);
}

static void dac_write(uint16_t v) {
    tc4Writes++;
    v &= 0x3ffu; /* le TS masque sur 10 bits (DAC->DATA & 0x3ff) */
    /* v : 256..766 (milieu 512) -> s16, SATURÉ : la lib officielle écrit
     * DATA=0 au repos (« output 0 when not in use », Sound.cpp) et le
     * cast int16 wrappait -49056 en +16480 — un rail de DC à 50 % sous
     * tout jeu lib : LE bruit parasite permanent (Celeste comprise). */
    int s = (v - 512) * 64; /* plage DAC entière (0..1023) = pleine échelle 16 bits : plus d'écrêtage (×96 saturait au-delà de ±341 pas) */
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    /* couplage AC de l'ampli META : le 0 V au repos est du silence, pas
     * un rail ; un passe-haut 1er ordre (~35 Hz) bloque la composante
     * continue de n'importe quel jeu sans toucher le signal */
    static float dcX, dcY; static int dcInit;
    if (!dcInit) { dcX = (float)s; dcInit = 1; } /* pas de plop au boot */
    dcY = 0.995f * dcY + (float)s - dcX;
    dcX = (float)s;
    int16_t out = (int16_t)(dcY > 32767.f ? 32767.f : (dcY < -32768.f ? -32768.f : dcY));
    audio_push(out);
    wav_put(out);
}

/* --------------------------------------------------- carte SD (PA27) */

static int sd_selected(void) { return (portA_out & (1u << 27)) == 0; }

/* même modèle que sdcard.ts : l'octet que la carte pilote pendant l'échange
 * N a été décidé par l'octet reçu pendant l'échange N-1.  sd_pending tient
 * ce décalage, sd_out est la file des octets à venir. */
static void sd_reset_state(void) {
    sd_pending = 0xff;
    sd_outLen = 0; sd_outPos = 0;
    sd_multi = 0;
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

/* CMD18 (READ_MULTIPLE_BLOCK) : la carte enchaîne les blocs (jeton 0xFE +
 * 512 + CRC) jusqu'au CMD12.  Le SdFat de la lib officielle s'en sert pour
 * tout read() de plusieurs secteurs contigus ; rejeté (0x04) auparavant, la
 * lecture échouait — le lecteur WAV de Picomon rejouait alors en boucle son
 * tampon de 2047 octets jamais rafraîchi (saturation + claquements sans fin
 * après la première fin de musique, rembobinage = seekSet + gros read). */
static void sd_multi_feed(void) {
    uint8_t *card = sd_card_data();
    size_t base = (size_t)sd_multiLba * 512;
    if (!card || base + 512 > sd_card_size()) { /* hors carte : erreur d'adresse */
        sd_out_push(0x0d); /* data error token : out of range */
        sd_multi = 0;
        return;
    }
    if (sd_outPos >= sd_outLen) sd_outPos = sd_outLen = 0; /* file consommée : on recompacte */
    sd_out_push(0xff);           /* Nac */
    sd_out_push(0xfe);
    sd_out_append(card + base, 512);
    sd_out_push(0xff); sd_out_push(0xff);
    sd_multiLba++;
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
        case 13: /* SEND_STATUS (R2) : le SdFat du guest interroge le statut
                  * après chaque écriture — « illegal command » (0x04) le
                  * faisait boucler sur la réécriture du secteur */
            sd_out_push(0x00); sd_out_push(0x00); break;
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
        case 18: /* lecture multi-blocs : R1 puis blocs jusqu'au CMD12 */
            sd_outPos = sd_outLen = 0;
            sd_out_push(0x00);
            sd_multi = 1;
            sd_multiLba = arg;
            sd_multi_feed();
            break;
        case 12: /* STOP_TRANSMISSION : le flux s'arrête, octet de bourrage, R1 */
            sd_multi = 0;
            sd_outPos = sd_outLen = 0;
            sd_out_push(0xff);
            sd_out_push(0x00);
            break;
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
            { static int wd2 = -1;
              if (wd2 < 0) wd2 = getenv("SD_DEBUG") ? (getenv("SD_DEBUG")[0] == '2' ? 1 : 0) : 0;
              if (wd2) {
                int lfn = 0;
                for (int i = 130; i <= 150; i++) if (sd_writeBuf[i]) lfn++;
                fprintf(stderr, "[wr-done] t=%u lba=%u lfn=%d octets[128..159]=", tickCount, sd_writeLba, lfn);
                for (int i = 129; i <= 160; i++) fprintf(stderr, "%02x", sd_writeBuf[i]);
                fprintf(stderr, "\n"); } }
            if (card && sd_writeBuf[0] == 0xfe) {
                memcpy(card + (size_t)sd_writeLba * 512, sd_writeBuf + 1, 512);
                sd_write_persist(sd_writeLba, card + (size_t)sd_writeLba * 512);
            }
            /* accepté, puis busy (comme sdcard.ts).  Le token de réponse est
             * déposé dans sd_pending — donc conduit DO dès l'échange suivant —
             * car la lib officielle (SdFat, SdSpiCard::writeData) lit la
             * réponse d'écriture en UN seul échange après le CRC ; le décalage
             * d'un échange du modèle lui faisait lire 0xFF, jugeait l'écriture
             * échouée et réécrivait le secteur en boucle (« SAVE ERROR Invalid
             * save file. ») */
            sd_pending = 0x05;
            sd_out_push(0x00);
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
    if (sd_multi && sd_outLen - sd_outPos < 4) sd_multi_feed(); /* bloc suivant */
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

static void to83(const char *name, char used[][13], int nUsed, char out[12]) {
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
    snprintf(used[nUsed < 256 ? nUsed : 255], 13, "%s", final);
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
            fprintf(stderr, TR("carte SD : pleine, contenu tronqué\n", "SD card: full, content truncated\n"));
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


static uint32_t fat_alloc_dir_data(void) { return fat_alloc(1); }

/* somme de contrôle 8.3 d'une entrée LFN */
static unsigned char fat_lfn_csum(const char *name83) {
    unsigned char c = 0;
    for (int i = 0; i < 11; i++)
        c = (unsigned char)(((c & 1) << 7) + (c >> 1) + (unsigned char)name83[i]);
    return c;
}

/* écrit les entrées LFN (nom long VFAT) se terminant à *off — juste avant
 * l'entrée 8.3 — quand le nom ne tient pas en 8.3.  Les loaders du site
 * scannent les noms longs (CatsAndCoinsDemo, TITLESCREEN.BMP...) ; gbrecomp
 * (meta_fat.c) saute les entrées 0x0F, donc rétrocompatible. */
static int fat_write_lfn_entries(uint8_t *buf, int off, int cap, const char *longname, const char *name83) {
    int len = (int)strlen(longname);
    int nents = (len + 12) / 13;
    unsigned char csum = fat_lfn_csum(name83);
    for (int k = nents; k >= 1; k--) {
        if (off > cap - 32) return off; /* plus de place : sans LFN */
        uint8_t *e = buf + off;
        memset(e, 0xff, 32);
        /* l'indicateur « dernière partie » (0x40) marque la PREMIÈRE entrée
         * physique, qui porte l'ordre le plus haut (spéc FAT, et le SdFat du
         * guest l'exige : FatFileLFN.cpp refuse une chaîne sans lui) */
        e[0] = (uint8_t)(k == nents ? 0x40 | nents : k);
        uint16_t chars[13];
        for (int i = 0; i < 13; i++) {
            int idx = (k - 1) * 13 + i;
            chars[i] = idx < len ? (uint16_t)(unsigned char)longname[idx]
                                 : (idx == len ? 0x0000u : 0xFFFFu);
        }
        for (int i = 0; i < 5; i++) { e[1 + i*2] = (uint8_t)chars[i]; e[2 + i*2] = (uint8_t)(chars[i] >> 8); }
        e[11] = 0x0F;
        e[12] = 0x00;
        e[13] = csum;
        for (int i = 0; i < 6; i++) { e[14 + i*2] = (uint8_t)chars[5 + i]; e[15 + i*2] = (uint8_t)(chars[5 + i] >> 8); }
        e[26] = 0x00; e[27] = 0x00; /* FstClusLO : toujours 0 (spéc VFAT) */
        for (int i = 0; i < 2; i++) { e[28 + i*2] = (uint8_t)chars[11 + i]; e[29 + i*2] = (uint8_t)(chars[11 + i] >> 8); }
        off += 32;
    }
    return off;
}

/* le nom long (basename du chemin) mérite-t-il une entrée LFN ? */
static const char *fat_longname(const char *path) {
    const char *sl = strrchr(path, '/');
    return sl ? sl + 1 : path;
}

static int fat_needs_lfn(const char *longname, const char *name83) {
    char up[1024];
    snprintf(up, sizeof(up), "%s", longname);
    for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
    char canon[16];
    int bl = 0;
    while (bl < 11 && name83[bl] != ' ') { canon[bl] = name83[bl]; bl++; }
    int el = 0;
    while (el < 3 && name83[11 - 3 + el] != ' ') el++;
    canon[bl] = 0;
    if (el) { canon[bl] = '.'; memcpy(canon + bl + 1, name83 + 8, el); canon[bl + 1 + el] = 0; }
    return strcmp(up, canon) != 0 && strcmp(longname, canon) != 0;
}

/* écrit une entrée de répertoire (LFN éventuel puis 8.3) à buf+off ;
 * renvoie l'offset suivant, ou -1 si plus de place.  Une seule définition :
 * le corps était dupliqué entre fat_write_dir_data et fat_finish. */
static int fat_put_entry(uint8_t *buf, int cap, int off, const FatEnt *e) {
    if (e->path[0]) {
        const char *ln = fat_longname(e->path);
        if (fat_needs_lfn(ln, e->name83))
            off = fat_write_lfn_entries(buf, off, cap, ln, e->name83);
    }
    if (off > cap - 32) return -1;
    uint8_t *d = buf + off;
    memcpy(d, e->name83, 11);
    d[11] = e->isDir ? 0x10 : 0x20;
    d[26] = e->first & 0xff;
    d[27] = e->first >> 8;
    d[28] = e->size & 0xff; d[29] = (e->size >> 8) & 0xff;
    d[30] = (e->size >> 16) & 0xff; d[31] = (e->size >> 24) & 0xff;
    return off + 32;
}

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
    for (int i = 0; i < n; i++) {
        int next = fat_put_entry(buf, (int)sizeof(buf), off, &entries[i]);
        if (next < 0) break;
        off = next;
    }
    memcpy(fatImage + lba * FAT_SECTOR, buf, csz);
}

/* place le contenu du répertoire ; renvoie les entrées allouées */
static void fat_walk(const char *dir, uint32_t parentFirst, int isRoot,
                     FatEnt **outEntries, int *outN) {
    DIR *d = opendir(dir);
    if (!d) { *outEntries = NULL; *outN = 0; return; }
    struct dirent *e;
    char used[256][13]; int nUsed = 0; /* 8.3 avec point : 12 caractères + NUL */
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
                fprintf(stderr, TR("carte SD : %s hors capacité, ignoré\n", "SD card: %s over capacity, ignored\n"), ents[i].path);
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
        fprintf(stderr, TR("carte SD : image trop grande (%zu Mio) — abandon\n", "SD card: image too large (%zu MiB) — aborting\n"),
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
    /* BPB étendu FAT12/16 : signature 0x29, n° de volume, étiquette et
     * type « FAT16   » à l'offset 54 — la PetitFatFs de PokittoLib (et
     * ChaN FatFs) ne reconnaît un volume qu'à cette chaîne (check_fs) :
     * sans elle, sept lectures du secteur 0 puis « pas de système de
     * fichiers » (Galaxy Fighters sans musique) */
    bs[0x24] = 0x80;                                  /* lecteur */
    bs[0x26] = 0x29;                                  /* signature étendue */
    bs[0x27] = 0x45; bs[0x28] = 0x4d; bs[0x29] = 0x55; bs[0x2a] = 0x31; /* n° de volume */
    memcpy(bs + 0x2b, "NO NAME    ", 11);
    memcpy(bs + 0x36, "FAT16   ", 8);
    bs[510] = 0x55; bs[511] = 0xaa;
    /* --- MBR : une partition FAT16 occupant tout le reste (pas en
     * superfloppy : le secteur 0 est alors le secteur de boot lui-même) --- */
    if (fatPartStart) {
        fatImage[0x1be] = 0x00; /* non amorçable */
        /* CHS calculés avec la géométrie du BPB (8 têtes, 32 secteurs/piste) ;
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
        int next = fat_put_entry(rootBuf, FAT_ROOT * 32, off, &rootEnts[i]);
        if (next < 0) break;
        off = next;
    }
    uint32_t rootLba = fatPartStart + 1 + 2 * fatsz;
    memcpy(fatImage + rootLba * FAT_SECTOR, rootBuf, FAT_ROOT * 32);
    memcpy(fatImage + (fatPartStart + 1) * FAT_SECTOR, fatTable, fatsz * FAT_SECTOR);
    memcpy(fatImage + (fatPartStart + 1 + fatsz) * FAT_SECTOR, fatTable, fatsz * FAT_SECTOR);
    free(rootBuf); free(rootEnts); rootEnts = NULL; rootN = 0;
#ifdef __EMSCRIPTEN__
    /* restaure les fichiers modifiés lors d'une session précédente
     * (localStorage, clé par jeu) */
    for (int i = 0; i < fatFileCount; i++) {
        if (!fatFiles[i].mem || !fatFiles[i].path[0]) continue;
        int n = em_ls_get(em_save_key(fatFiles[i].path), fatFiles[i].mem,
                          (int)fatFiles[i].bytes);
        if (n > 0)
            memcpy(fatImage + fatFiles[i].lba * FAT_SECTOR, fatFiles[i].mem,
                   (size_t)n);
    }
#endif
    if (getenv("FAT_DUMP")) {
        FILE *g = fopen(getenv("FAT_DUMP"), "wb");
        if (g) { fwrite(fatImage, 1, fatImageSize, g); fclose(g);
                 printf(TR("image FAT test : %s\n", "FAT image test: %s\n"), getenv("FAT_DUMP")); }
    }
    printf(TR("carte SD : %s (%d fichiers)\n", "SD card: %s (%d files)\n"), label, fatFileCount);
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
    char used[256][13]; int nUsed = 0; /* 8.3 avec point : 12 caractères + NUL */
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
                /* fichier virtuel (navigateur) : écrit dans le tampon et
                 * persiste le fichier complet en localStorage (clé par jeu) */
                memcpy(fatFiles[i].mem + (lba - fatFiles[i].lba) * 512, data, 512);
#ifdef __EMSCRIPTEN__
                em_ls_set(em_save_key(fatFiles[i].path), fatFiles[i].mem,
                          (int)fatFiles[i].bytes);
#endif
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
    s.next_in = (Bytef *)(uintptr_t)src; s.avail_in = (uInt)csize; /* zlib ne l'écrit pas */
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
    /* tas (pas pile) : 256 ZipEnt (~270 Ko) débordaient la pile wasm (64 Ko)
     * et corrompaient l'état en silence — lapinou passait par chance de
     * layout, les autres zips restaient à 0 %. */
    static ZipEnt *ents = 0;
    if (!ents) ents = malloc(sizeof(ZipEnt) * 256);
    int n = zip_parse(data, len, ents, 256);
    if (n == 0) return 0;
    /* décompresse TOUT d'abord : les pointeurs cdata pointent dans `data`,
     * que reset_machine va libérer (sd_unload) */
    static uint8_t **datas = 0;
    if (!datas) datas = malloc(sizeof(uint8_t *) * 256);
    int fwb = -1, fpop = -1;
    for (int i = 0; i < n; i++) {
        datas[i] = zip_entry_data(&ents[i]);
        if (!datas[i]) { /* entrée illisible : retirée de la liste */
            memmove(ents + i, ents + i + 1, sizeof(ZipEnt) * (size_t)(n - i - 1));
            n--; i--; continue;
        }
        size_t l = strlen(ents[i].name);
        if (fwb < 0 && l > 4 && strcasecmp(ents[i].name + l - 4, ".bin") == 0) fwb = i;
        if (fpop < 0 && l > 4 && strcasecmp(ents[i].name + l - 4, ".pop") == 0) fpop = i;
    }
    /* Pokitto : jeux distribués en .pop (conteneur du loader) + assets */
    if (fwb < 0) fwb = fpop;
    /* PAS d'aplatissement : les jeux ouvrent leurs assets avec le préfixe
    * de leur dossier (PICOMON/MUSICS/...), comme dans le dossier posé
    * à la racine de la carte ; l'ancien retrait du préfixe tuait la
    * musique du cas zip (la « boucle /PICOMON » d'avant était le bug BLX) */
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
    { /* EMU_DUMP_CARD=<fichier> : image FAT construite, pour audit de chaîne */
        const char *dc = getenv("EMU_DUMP_CARD");
        if (dc && fatImage) {
            FILE *df = fopen(dc, "wb");
            if (df) { fwrite(fatImage, 1, fatImageSize, df); fclose(df); }
            fprintf(stderr, TR("carte dumped : %s (%zu o)\n", "card dumped: %s (%zu B)\n"), dc, fatImageSize);
        }
    }
    printf(TR("carte SD : zip (%d fichiers)", "SD card: zip (%d files)"), n);
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
 * (actifs bas : 0 = enfoncé).  Ordre du fork TS de référence et de la
 * lib officielle (Buttons.h : down=0, left, right, up, a, b, menu, home),
 * c'est lui qui fait répondre les jeux lib (Celeste, Reuben...) ;
 * lapinou (homebrew) lit un ordre propre, cf. EMU_BTN_ORDER plus bas. */
#define BTN_DOWN   (1u << 0)
#define BTN_LEFT   (1u << 1)
#define BTN_RIGHT  (1u << 2)
#define BTN_UP     (1u << 3)
#define BTN_A      (1u << 4)
#define BTN_B      (1u << 5)
#define BTN_MENU   (1u << 6)
#define BTN_HOME   (1u << 7)
#define BTN_DIRMASK (BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_UP)

/* octet boutons réordonné pour un pad lu à 24 MHz (ordre historique des
 * jeux maison : left,right,up,a,b,menu,down,home) ; l'entrée b = ordre lib
 * (down,left,right,up,a,b,menu,home) */
static uint8_t pad_byte_24(uint8_t b) {
    uint8_t r = 0;
    if (b & 0x01u) r |= 1u << 6;   /* down */
    if (b & 0x02u) r |= 1u << 0;   /* left */
    if (b & 0x04u) r |= 1u << 1;   /* right */
    if (b & 0x08u) r |= 1u << 2;   /* up */
    if (b & 0x10u) r |= 1u << 3;   /* a */
    if (b & 0x20u) r |= 1u << 4;   /* b */
    if (b & 0x40u) r |= 1u << 5;   /* menu */
    if (b & 0x80u) r |= 1u << 7;   /* home */
    return r;
}


static SDL_GameController *pad; /* définitions complètes dans la section SDL */
static SDL_Joystick *joyFb;
static char fwPath[1024];
static char outImgPath[512];
static uint32_t emu_nextFrameTick = 334860u; /* pas initial (frame_ticks suit
                                              * la cible) — la META ne passe
                                              * par aucun reset avant le
                                              * premier run_emulated_frame */
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
#define PK_SYSCON_MAINCLKSEL   28u
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
/* file de réponse en anneau : un pop = avance de tête.  L'ancien stockage
 * compacté (memmove de jusqu'à 516 octets à CHAQUE octet SPI pendant le
 * streaming CMD18) coûtait plus que le reste de la machine SD réunie. */
static uint8_t pk_sd_response[600];
static int pk_sd_respHead, pk_sd_respLen;
static int pk_sd_dirty; /* export via --out-img */
static uint16_t pk_sd_crc[256];

static uint8_t *pk_sd_card(void) { return sd_card_data(); }
static size_t pk_sd_card_size(void) { return sd_card_size(); }

static void pk_sd_resp(const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) {
        if (pk_sd_respLen >= (int)sizeof(pk_sd_response)) return;
        pk_sd_response[(pk_sd_respHead + pk_sd_respLen) % (int)sizeof(pk_sd_response)] = b[i];
        pk_sd_respLen++;
    }
}
static uint8_t pk_sd_resp_front(void) { return pk_sd_response[pk_sd_respHead]; }
static void pk_sd_resp_pop(void) {
    if (pk_sd_respLen) {
        pk_sd_respHead = (pk_sd_respHead + 1) % (int)sizeof(pk_sd_response);
        pk_sd_respLen--;
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
        pk_spi_in(&pk_spi0, pk_sd_resp_front(), 1);
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
            pk_spi_in(&pk_spi0, pk_sd_resp_front(), 1);
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
/* cadence mesurée des latches R2R (moyenne glissante) -> emuDacRate */
static float pk_rateEma;
static int pk_rateInit;
static unsigned long pk_latchCount, pk_ctIrqCount, pk_ctCross, pk_irq34, pk_latchMid, pk_latchSound;
static FILE *latchDump;
static int latchDumpTried; /* EMU_LATCH_DUMP lu une seule fois (getenv par
                            * latch = 22 000 appels/s sur le chemin audio) */
#define QADDR 0x20000000u /* .bss_ram1 : pokitto_audio_q (tail, head, ring) */


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

static void pk_r2r_reopen(void); /* défini dans la section SDL */
static void pk_audio_reopen(int freq); /* défini dans la section SDL */

static void pk_audio_check_hle(uint32_t rate) {
    uint32_t timerIRQ = pk_read_word(sys_VTOR + (34 << 2));
    if (pk_hleState != PK_HLE_DETECT) {
        if (pk_hleIrqAddress == timerIRQ) return;
        /* le handler a changé (loader -> jeu) : le device U8 du HLE ne doit
         * pas jouer le jeu — retour au R2R le temps de la re-détection */
        if (pk_hleState == PK_HLE_ENABLED) pk_r2r_reopen();
    }
    pk_hleState = PK_HLE_DISABLED;
    pk_hleIrqAddress = timerIRQ;
    /* le loader stock garde buffer + playhead à +0x70/+0x6c du handler :
     * détection structurelle (adresses résolubles) — le magic 0x32a90803
     * ne matche qu'un build exact du loader et rate les autres .pop */
    uint32_t vbuffer = pk_read_word(timerIRQ + 0x70 - 1);
    uint32_t vplay = pk_read_word(timerIRQ + 0x6c - 1);
    pk_hleBuffer = pk_audio_address(vbuffer);
    pk_hlePlayhead = (uint32_t *)pk_audio_address(vplay);
    if (ENVFLAG("EMU_PK_DEBUG"))
        fprintf(stderr, "[pkhle] handler=%x vbuffer=%x vplay=%x -> %d\n",
                timerIRQ, vbuffer, vplay,
                (pk_hleBuffer && pk_hlePlayhead) ? 1 : 0);
    if (!pk_hleBuffer || !pk_hlePlayhead) return;
    pk_hleState = PK_HLE_ENABLED;
    pk_audio_reopen((int)(pk_core_hz() / rate));
    fprintf(stderr, TR("audio HLE actif (%d Hz)\n", "HLE audio active (%d Hz)\n"), (int)(pk_core_hz() / rate));
}

/* valeur écrite sur le R2R : POUT1[31:28] | POUT2[23:20] */
static void pk_audio_gpio_write(void);

static void pk_audio_write(uint8_t data) {
    if (pk_hleState == PK_HLE_ENABLED) return;
    pk_prevData = data;
    pk_latchCount++;
    if (!latchDump && !latchDumpTried) {
        const char *p = getenv("EMU_LATCH_DUMP");
        latchDumpTried = 1;
        if (p) latchDump = fopen(p, "w");
    }
    if (latchDump) fprintf(latchDump, "%u %u\n", tickCount, data);
    if (pk_latchCount > 44000) { /* après le boot : ce qui est réellement joué */
        if (data == 128) pk_latchMid++;
        else { pk_latchSound++;
            if (pk_latchSound == 1 || pk_latchSound == 1000 || pk_latchSound == 100000)
                fprintf(stderr, "[val] latch#%lu val=%u tick=%u\n", pk_latchSound, data, tickCount);
        }
    }
    float clock = (float)pk_core_hz();
    float delta = (float)(uint32_t)(tickCount - pk_prevTicks) / clock;
    pk_prevTicks = tickCount;
    /* trou dans les latches (pause, menu, stall hôte) : il ne doit pas
     * s'encoder comme un retard permanent de la file — le décalage audible
     * se cumulait de la durée de chaque pause, session après session */
    if (delta > 3.0f * PK_IFREQ) delta = 3.0f * PK_IFREQ;
    /* cadence réelle des latches -> emuDacRate : GF tique à 8 kHz (CT32B0,
     * MR1) et le consommateur hôte restait calé sur le défaut 22049 — il
     * mangeait 2,75x trop vite, la file tombait à sec en permanence et le
     * son du jeu (fanfares d'écran et musiques streamées de la SD
     * comprises) sortait haché.  Moyenne glissante courte + hystérésis. */
    if (delta > 0.0f) {
        float r = 1.0f / delta;
        if (!pk_rateInit) { pk_rateEma = r; pk_rateInit = 1; }
        else pk_rateEma += 0.25f * (r - pk_rateEma);
        /* l'EMA (alpha 0,25) est déjà lissée : affectation directe, la
         * consommation suit la cadence réelle du jeu sans dérive */
        if (pk_rateEma > 3000.0f && pk_rateEma < 48000.0f)
            emuDacRate = pk_rateEma;
    }
    pk_aqDelta[pk_aqEnd] = delta;
    pk_aqData[pk_aqEnd] = data;
    pk_aqEnd = (pk_aqEnd + 1) & PK_AQ_MASK;
    if (pk_aqStart == pk_aqEnd) pk_aqStart = (pk_aqStart + 1) & PK_AQ_MASK;
    else pk_aqSize++;
}

static int pk_audio_ready(void) { return pk_aqSize >= 600; }

/* Fréquence du cœur telle que le firmware l'a configurée : IRC 12 MHz tant
 * que MAINCLKSEL ne pointe pas la sortie PLL, sinon 12 MHz x M (SYSPLLCTRL).
 * L'ancien modèle cadençait à 45 MHz en dur : les firmwares 72 MHz (et tout
 * ce qui n'est pas 0x25) tournaient à la mauvaise vitesse — le son GB
 * étiré de 60 %, le jeu au ralenti. */
static double pk_core_hz(void) {
    if ((pk_syscon[PK_SYSCON_MAINCLKSEL] & 3u) != 3u) return 12e6;
    /* la fréquence est celle que le guest programme lui-même (12 MHz IRC
     * x MSEL+1 quand MAINCLKSEL pointe le PLL) — le vrai SoC encaisse les
     * configs au-delà du spec sheet (50 MHz), donc le modèle aussi. */
    return 12e6 * (double)((pk_syscon[PK_SYSCON_SYSPLLCTRL] & 0x1Fu) + 1u);
}

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
                if (cmdId > 62) fprintf(stderr, TR("IAP invalide : %u\n", "invalid IAP: %u\n"), cmdId);
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
    pushStack(regs[15] - 2u); /* adresse de reprise réelle (exc_return la relit) */
    pushStack(regs[14]);
    pushStack(regs[12]);
    pushStack(regs[3]);
    pushStack(regs[2]);
    pushStack(regs[1]);
    pushStack(regs[0]);
    regs[14] = 0xfffffff9u;
    regs[15] = pk_read_word(sys_VTOR + (id << 2)) & ~1u;
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

static void pk_ct_tick(struct pk_ct *ct, uint32_t num, uint32_t delta) {
    if (!(ct->r[PK_CT_TCR] & 1)) return;
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
            if (num == 0 && m == 1) pk_ctCross++;
            if (ct->r[5] & mri) ct->r[PK_CT_IR] |= 1u << m;
            if (ct->r[5] & mrs) ct->r[PK_CT_TCR] &= ~1u;
            if (ct->r[5] & mrr) ct->r[PK_CT_TC] -= mr;
        }
    }

    if (ct->r[PK_CT_IR] && armIrqEnable) {
        if (num == 0) {
            pk_irq34++;
            /* détection HLE du handler stock (loader) : auto-limitée (le
             * même vecteur ne re-détecte pas), le taux CT32B0 courant sert
             * au taux de lecture du buffer HLE */
            uint32_t mr0 = ct->r[6];
            if (mr0 > 0) pk_audio_check_hle((uint32_t)(pk_core_hz() / ((uint64_t)(mr0 + 1) * pr)));
        }
        pk_interrupt(34 + num);
    }
}

/* ticks restants avant le prochain événement timer (IRQ en attente, SysTick
 * CVR à court, prochain match CT armé) — sert à ne traiter les timers qu'à
 * leurs échéances : par instruction, ils coûtaient 4 à 5 fois le reste. */
static uint32_t pk_timers_next(void) {
    uint32_t next = ~0u;
    if ((pk_systickCSR & 1u) && pk_systickCVR + 1u < next) next = pk_systickCVR + 1u;
    /* COUNTFLAG : urgence seulement si l'IRQ SysTick est activée (sinon le
     * drapeau reste posé jusqu'à une lecture, et le Runtime ne le lit pas) */
    if ((pk_systickCSR & 3u) == 3u && (pk_systickCSR & (1u << 16))) next = 0;
    for (uint32_t n = 0; n < 2; n++) {
        struct pk_ct *ct = &pk_ct[n];
        if (!(ct->r[PK_CT_TCR] & 1u)) continue;
        uint32_t pr = ct->r[PK_CT_PR] + 1u;
        if (ct->r[PK_CT_IR]) return 0; /* IRQ en attente : maintenant */
        for (uint32_t m = 0; m < 4; m++) {
            if (!((ct->r[5] >> (m * 3)) & 1u)) continue; /* match sans IRQ */
            uint32_t mr = ct->r[6 + m], tc = ct->r[PK_CT_TC];
            uint32_t t = tc < mr ? (mr - tc) * pr : 0;
            if (t < next) next = t;
        }
    }
    return next == ~0u ? 1u << 20 : next; /* rien d'actif : re-regarder plus tard */
}

static uint32_t pkTimerNext;         /* tick de la prochaine échéance */
static int       pkTimerNextValid;

static void pk_timers_update(void) {
    /* COUNTFLAG posé pendant une section critique (IRQ masquées) : l'IRQ
     * SysTick part dès le ré-enable — sinon le drapeau restait posé pour
     * toujours (le handler ne lit pas CSR) et l'échéance restait à 0. */
    if ((pk_systickCSR & 3u) == 3u && (pk_systickCSR & (1u << 16)) && armIrqEnable) {
        pk_systickCSR &= ~(1u << 16);
        pk_interrupt(15);
    }
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
    if (!pkTimerNextValid || (int32_t)(tickCount - pkTimerNext) >= 0) {
        pk_timers_update();
        pk_gpio_update();
        pkTimerNext = tickCount + pk_timers_next();
        pkTimerNextValid = 1;
    }
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

/* noinline : chemin lent volontaire (MMIO) — inliné dans step, le grand
 * switch noierait le i-cache du chemin chaud (même logique que le noinline
 * de step_batch pour wasm). */
__attribute__((noinline))
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
        if (off <= 0x1Cu) pkTimerNextValid = 0; /* SysTick réécrit */
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

__attribute__((noinline))
static uint32_t pk_reg_read(uint32_t a) {
    uint32_t v = pk_reg_peek(a);
    if (a < 0x50000000u) {
        uint32_t al = a & ~3u;
        if (al == 0x4001C024u) return pk_prng() & 0xFFF; /* ADC DAT1 */
        if ((al == 0x40014008u || al == 0x40018008u) ||
            (al == 0x40024008u && pk_rtcEnabled)) {
            pk_timers_update();
            pkTimerNextValid = 0; /* TC/RTC relus : l'échéance peut changer */
        }
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

__attribute__((noinline))
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
                pkTimerNextValid = 0; /* l'échéance peut changer */
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
                       if (i < 256) {
                           if (ENVFLAG("EMU_PK_DEBUG") &&
                               (i == PK_SYSCON_SYSPLLCTRL || i == PK_SYSCON_MAINCLKSEL || i == 1))
                               fprintf(stderr, "[pkclk] t=%u SYSPLLCTRL/SEL[%u] <- %x\n", tickCount, i, v);
                           pk_syscon[i] = v;
                       }
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

/* Fast-path flash + SRAM principale : ce sont les MÊMES tampons que le
 * cœur META (factorisation), et ces lectures n'ont pas d'effet de bord —
 * sinon chaque accès pokitto traversait pk_reg_peek et le grand switch
 * LPC, et le cœur tombait à 62-80 % du temps réel (l'audio META fait
 * 295 %).  Les autres régions (MMIO, banques 2 Ko) gardent la voie lente :
 * leurs lectures ont des effets (FIFO, ADC...). */
static uint32_t pk_rd16le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static int pk_in_ram(uint32_t a, uint32_t n) {
    uint32_t o = a - 0x10000000u;
    return a >= 0x10000000u && o <= SRAM_SIZE - n;
}
/* inline : ces accès sont le chemin chaud du cœur Pokitto — hors-ligne, le
 * no-LTO les laissait en appels (fetchHalf -> pk_read_half : 12 % du temps
 * hôte sur Pandemic) ; le grand switch MMIO reste de son côté hors-ligne. */
static inline uint32_t pk_read_word(uint32_t a) {
    a &= ~3u;
    if (a + 4 <= FLASH_SIZE) return pk_rd32le(flash + a);
    if (pk_in_ram(a, 4)) return pk_rd32le(sram + (a - 0x10000000u));
    return pk_reg_read(a);
}
static inline uint16_t pk_read_half(uint32_t a) {
    a &= ~1u;
    if (a + 2 <= FLASH_SIZE) return (uint16_t)pk_rd16le(flash + a);
    if (pk_in_ram(a, 2)) return (uint16_t)pk_rd16le(sram + (a - 0x10000000u));
    uint32_t v = pk_reg_read(a & ~3u);
    return (uint16_t)(v >> ((a & 2) << 3));
}
static inline uint8_t pk_read_byte(uint32_t a) {
    if (a < FLASH_SIZE) return flash[a];
    if (pk_in_ram(a, 1)) return sram[a - 0x10000000u];
    uint32_t v = pk_reg_read(a & ~3u); /* mot aligné, puis lane d'octet */
    return (uint8_t)(v >> ((a & 3) << 3));
}
/* écritures : seule la SRAM principale est court-circuitée (store direct) ;
 * la flash a un effet (HardFault, comme la référence) et les MMIO aussi. */
static inline void pk_write_word(uint32_t a, uint32_t v) {
    a &= ~3u;
    if (pk_in_ram(a, 4)) {
        uint8_t *p = sram + (a - 0x10000000u);
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
        return;
    }
    pk_reg_write(a, v);
}
static inline void pk_write_half(uint32_t a, uint16_t v) {
    if ((a & 1u) == 0u && pk_in_ram(a, 2)) {
        uint8_t *p = sram + (a - 0x10000000u);
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        return;
    }
    uint32_t al = a & ~3u;
    uint32_t old = pk_reg_peek(al);
    uint32_t lane = (uint32_t)(a & 2) << 3;
    pk_reg_write(al, (old & ~(0xFFFFu << lane)) | ((uint32_t)v << lane));
}
static inline void pk_write_byte(uint32_t a, uint8_t v) {
    if (pk_in_ram(a, 1)) { sram[a - 0x10000000u] = v; return; }
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
    fN = fZ = fC = fV = 0;
    armIrqEnable = 1;
    tickCount = 0;
    pk_lastTick = 0;
    pkTimerNextValid = 0;

    /* SYSCON / périphériques (valeurs de la référence) */
    memset(pk_syscon, 0, sizeof pk_syscon);
    pk_syscon[PK_SYSCON_SYSPLLCTRL] = 0x23;
    pk_syscon[PK_SYSCON_MAINCLKSEL] = 3u; /* le loader rend la main sur le PLL */
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
    pk_rateEma = 0;
    pk_rateInit = 0; /* la cadence apprise sur le jeu précédent ne fuit pas */
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
    fprintf(stderr, TR("eeprom : %s\n", "eeprom: %s\n"), path);
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
    printf(TR("eeprom : %s\n", "eeprom: %s\n"), path);
#endif
}

/* DEBUG temporaire : état IRQ/timers */
static void pk_debug_dump(void) {
    if (!getenv("EMU_PK_DEBUG")) return;
    for (int i = 0; i < 64; i++)
        if (pk_irqCount[i]) fprintf(stderr, TR("IRQ %d : %ld tirs\n", "IRQ %d: %ld shots\n"), i, pk_irqCount[i]);
    fprintf(stderr, TR("HLE audio : %s\n", "HLE audio: %s\n"),
            pk_hleState == PK_HLE_ENABLED ? TR("actif (firmware stock)", "active (stock firmware)")
                                          : TR("inactif (R2R)", "inactive (R2R)"));
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
    printf(TR("carte exportée : %s (%zu Kio)\n", "card exported: %s (%zu KiB)\n"), outImgPath, sz / 1024);
#endif
}



static long stWrites = 0, ramwrTotal = 0;
/* le panneau META est câblé BGR ; une init qui déclare MADCTL.BGR=1
 * (jeux maison à init custom, ex. lapinou : 0xC8) envoie des couleurs
 * d'ordre natif — l'émulateur doit inverser R/B à l'affichage.  La lib
 * standard ne déclare jamais BGR et pré-swappe elle-même. */
static int lcdBgrSwapped;
/* COLMOD (0x3A, bits 2:0) : 3 = 12 bpp RGB444 (2 pixels sur 3 octets),
 * 5 = 16 bpp RGB565 (défaut des firmwares), 6 = 18 bpp RGB666 (3 octets par
 * pixel, 6 bits en haut de chaque octet) — datasheet ST7735 §9.7.19-22 */
static uint8_t lcdColmod = 5;
static uint8_t lcdPixBuf[3];
static void lcd_put_pixel(uint16_t p) {
    if (lcdBgrSwapped) p = (uint16_t)((p >> 11) | (p & 0x07e0u) | ((p & 0x1fu) << 11));
    if (lcd_x < 160 && lcd_y < 128) pix[lcd_y * 160 + lcd_x] = p;
    if (++lcd_x > lcd_xEnd) { lcd_x = lcd_xStart; if (++lcd_y > lcd_yEnd) lcd_y = lcd_yStart; }
}
static uint16_t rgb444_to_565(uint32_t c) {
    uint32_t r = (c >> 8) & 15u, g = (c >> 4) & 15u, b = c & 15u;
    return (uint16_t)(((r << 1 | r >> 3) << 11) | ((g << 2 | g >> 2) << 5) | (b << 1 | b >> 3));
}
static uint8_t st7735_byte(uint8_t v) {
    if (portB_out & (1u << 22)) return 0xff; /* CS écran haut */
    stWrites++;
    { /* trace D/C + octet (débogage flux LCD) */
        static int bFrom = -1, bTo = -1;
        if (bFrom < 0) {
            const char *s = getenv("EMU_BYTES_FROM");
            const char *e = getenv("EMU_BYTES_TO");
            bFrom = s ? (int)atoi(s) : 0x7fffffff;
            bTo = e ? (int)atoi(e) : 0x7fffffff;
        }
        if ((int)tickCount >= bFrom && (int)tickCount <= bTo)
            fprintf(stderr, "[b] t=%u dc=%d v=%02x\n", tickCount,
                    (portB_out >> 23) & 1, v);
    }
    if (!(portB_out & (1u << 23))) { /* commande */
        lcd_lastCommand = v;
        lcd_argIndex = 0;            /* comme st7735.ts : reset à chaque commande */
        if (v == 0x2c) ramwrTotal++;
        if (v == 0x36 && ENVFLAG("EMU_LCD_DEBUG")) fprintf(stderr, "[lcd] MADCTL cmd\n");
        return 0xff;
    }
    if (lcd_lastCommand == 0x3a && lcd_argIndex == 0) lcdColmod = v & 7u; /* COLMOD */
    if (lcd_lastCommand == 0x2c && lcdColmod != 5) { /* RAMWR 12 ou 18 bpp */
        lcdPixBuf[lcd_argIndex % 3] = v;
        if (lcdColmod == 3) { /* RGB444 : R1G1 B1R2 G2B2 */
            if (lcd_argIndex % 3 == 1)
                lcd_put_pixel(rgb444_to_565(((uint32_t)lcdPixBuf[0] << 4) | (lcdPixBuf[1] >> 4)));
            else if (lcd_argIndex % 3 == 2)
                lcd_put_pixel(rgb444_to_565(((uint32_t)(lcdPixBuf[1] & 15u) << 8) | lcdPixBuf[2]));
        } else if (lcd_argIndex % 3 == 2) { /* RGB666 : 6 bits en haut de chaque octet */
            lcd_put_pixel((uint16_t)(((lcdPixBuf[0] >> 3) << 11) | ((lcdPixBuf[1] >> 2) << 5) | (lcdPixBuf[2] >> 3)));
        }
        lcd_argIndex++;
        return 0xff;
    }
    if (lcd_lastCommand == 0x36) {
        if (v & 0x08) lcdBgrSwapped = 1;
        if (ENVFLAG("EMU_LCD_DEBUG"))
            fprintf(stderr, "[lcd] MADCTL <- %02x (BGR=%d, verrou=%d)\n", v, (v >> 3) & 1, lcdBgrSwapped);
    }
    { /* données */
        switch (lcd_lastCommand) {
            case 0x2c: /* RAMWR */
                if (lcd_argIndex % 2 == 0) lcd_tmp = v;
                else {
                    /* dump du flux RAMWR : fichier (EMU_FB_DUMP) ou descripteur
                     * hérité (EMU_FB_FD=<n>, ex. `3>/tmp/dump.raw`) — le fd
                     * contourne le bac à sable qui avale les fwrite vers un
                     * fichier créé par le processus lui-même */
                    static FILE *fbDump; static int fbFd = -1;
                    static unsigned fbPx; static int fbDone;
                    static int fbStartTick = -1;
                    if (fbStartTick < 0) {
                        const char *st = getenv("EMU_FB_DUMP_START");
                        const char *fde = getenv("EMU_FB_FD");
                        fbStartTick = st ? (int)atoi(st) : 0;
                        fbFd = fde ? atoi(fde) : -1;
                    }
                    if (!fbDone && !fbDump && fbFd < 0) { /* getenv par pixel sinon */
                        const char *p = getenv("EMU_FB_DUMP");
                        if (p && (int)tickCount >= fbStartTick) {
                            fbDump = fopen(p, "wb");
                            if (fbDump) setvbuf(fbDump, NULL, _IONBF, 0);
                        }
                        else if (!p) fbDone = 1;
                    }
                    uint16_t p = (uint16_t)((lcd_tmp << 8) | v);
                    if (!fbDone && (int)tickCount >= fbStartTick) {
                        if (fbDump) {
                            fwrite(&p, 2, 1, fbDump);
                            if (++fbPx >= 20480 * 2) { fclose(fbDump); fbDump = NULL; fbDone = 1; }
                        } else if (fbFd >= 0) {
                            unsigned char b[2] = { (unsigned char)(p >> 8), (unsigned char)p };
                            if (write(fbFd, b, 2) != 2) fbDone = 1;
                            if (++fbPx >= 20480 * 2) fbDone = 1;
                        }
                    }
                    lcd_put_pixel(p);
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
        if (ENVFLAG("EMU_WIN_DEBUG")) {
            static uint32_t winRamwr;
            static int wnRemain = -2;
            static long wnTotal;
            if (wnRemain == -2) {
                const char *wl = getenv("EMU_WIN_DEBUG_LIMIT");
                wnRemain = wl ? atoi(wl) : 120;
            }
            if (lcd_lastCommand == 0x2c && (lcd_argIndex & 1)) winRamwr++;
            if (lcd_lastCommand != 0x2c || (lcd_argIndex == 0 && winRamwr)) {
                if (wnRemain > 0 || (wnTotal % 200) == 0)
                    fprintf(stderr, "[win] t=%u cmd=%02x arg=%d v=%02x fen(x %d-%d, y %d-%d) ramwr=%u\n",
                            tickCount, lcd_lastCommand, lcd_argIndex, v,
                            lcd_xStart, lcd_xEnd, lcd_yStart, lcd_yEnd, winRamwr);
                if (wnRemain > 0) wnRemain--;
                wnTotal++;
                if (lcd_lastCommand != 0x2c) winRamwr = 0;
            }
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
#define DMAC_TRIG_TC5_OVF 0x1cu /* TC5_DMAC_ID_OVF */
#define DMAC_TRIG_SERCOM4_TX 0x0au /* SERCOM4_DMAC_ID_TX (écran) */
#define DMAC_TRIG_SERCOM4_RX 0x09u /* SERCOM4_DMAC_ID_RX (carte SD, lib récente) */
/* DMA SPI cadencé : les beats sortent au rythme du baud SERCOM4 (le CPU
 * continue de tourner pendant le transfert — sur hardware le DMA écran
 * prend ~6,8 ms par demi-frame à 24 MHz, d'où les 40-55 fps réels) */
static int      spiDmaCh = -1;   /* canal SERCOM4-TX en cours, -1 = aucun */
/* durée d'un octet SPI = 8 bits × 2(BAUD+1) / 48 MHz = (BAUD+1)/3 µs, soit
 * (BAUD+1) × ticks/µs / 3 ticks — non entier (6,67 ticks à 24 MHz dans le
 * domaine 20 M) : l'arrondi entier à 7 ralentissait l'écran de 5 %, assez
 * pour qu'une trame pleine (40 960 octets, 13,7 ms) déborde de la frame et
 * affame l'audio (Gargoyle's Quest).  Accumulateur en tiers de tick. */
static uint32_t spiBeatAcc;      /* tiers de tick accumulés */
static uint32_t spiBeatTicks = 20; /* tiers de tick par beat = (BAUD+1) × ticks/µs */
static uint32_t spiBaud;         /* SERCOM4 BAUD (f = 48 MHz / (2×(b+1))) */
static uint8_t  dmaTrig[DMAC_CHANNELS], dmaOn[DMAC_CHANNELS];
static uint8_t  dmacIntFlag[DMAC_CHANNELS]; /* INTFLAG par canal : TCMPL=0x02, SUSP=0x04 */
static uint8_t  dmacIntEn[DMAC_CHANNELS];   /* CHINTENSET par canal (lecture) */
/* Fin de bloc / suspension d'un canal : le flag se pose toujours, mais la
 * ligne NVIC ne monte que si l'interruption correspondante est activée
 * (CHINTENSET, fidèle au SAMD21).  Le canal audio TC4 des firmwares
 * gbrecomp 0.5.0 n'active rien : avant ce filtre, chacune de ses fins de
 * bloc appelait le DMAC_Handler du runtime, qui (ne servant que le canal
 * écran) prenait ça pour la fin d'une bande d'affichage — file LCD
 * désynchronisée, bandes jamais envoyées, lignes périmées ou en double. */
static void dmac_raise(uint32_t ch, uint8_t bits) {
    if (ch < DMAC_CHANNELS && (dmacIntEn[ch] & bits)) { dmacInterrupt = 1; irqWork = 1; }
}
static uint8_t  dmaFerr[DMAC_CHANNELS];     /* CHSTATUS.FERR : descripteur invalide chargé */
static uint32_t dmaResumeAt[DMAC_CHANNELS]; /* descripteur à charger au prochain RESUME (suspend après bloc, BLOCKACT 0x2/0x3) */
static uint8_t  dmaSkipSuspend[DMAC_CHANNELS]; /* RESUME reçu pendant un bloc : le prochain suspend est sauté */
static uint16_t dmaCtrl[DMAC_CHANNELS], dmaCnt[DMAC_CHANNELS], dmaIdx[DMAC_CHANNELS];
static uint32_t dmaSrc[DMAC_CHANNELS], dmaDst[DMAC_CHANNELS], dmaNext[DMAC_CHANNELS];

/* CHCTRLA.SWRST (datasheet 20.8.18) : tous les registres du canal reviennent
 * à leur état initial — CHCTRLB (TRIGSRC), CHINTENSET, CHINTFLAG et
 * CHSTATUS.FERR compris.  Sans l'effacement des flags, un SUSP posé par un
 * seul sous-débit audio (descripteur invalide atteint) restait collé : le
 * channel_running() du runtime gbrecomp voyait alors le canal arrêté à
 * chaque frame et le relançait — 60 relances/s, chacune coupant la file et
 * insérant 256 échantillons figés : son haché en permanence. */
static void dmac_chan_swrst(uint32_t ch) {
    if (ch >= DMAC_CHANNELS) return;
    dmaOn[ch] = 0;
    if (spiDmaCh == (int)ch) spiDmaCh = -1;
    dmacIntFlag[ch] = 0;
    dmacIntEn[ch] = 0;
    dmaFerr[ch] = 0;
    dmaTrig[ch] = 0;
    dmaResumeAt[ch] = 0;
    dmaSkipSuspend[ch] = 0;
}

static void dma_sercom4_rx_beat(void);
static int dmaBeatSkipSd; /* un beat DMA d'affichage ne doit pas horloger la carte */
static int sdTxCh = -1;   /* canal TX de la carte SD pendant une écriture de secteur */
static int dma_is_tc4(uint32_t ch) {
    return ch < DMAC_CHANNELS && dmaTrig[ch] == DMAC_TRIG_TC4_OVF;
}
static int dma_is_tc5(uint32_t ch) {
    return ch < DMAC_CHANNELS && dmaTrig[ch] == DMAC_TRIG_TC5_OVF;
}

static void dma_load(uint32_t ch, uint32_t desc) {
    if (!desc) { /* DESCADDR=0 : fin normale de la transaction, canal
                  * désactivé — ni SUSP ni FERR (datasheet 20.6.2.8 : ceux-là
                  * ne concernent qu'un descripteur invalide ou un RESUME) */
        dmaOn[ch] = 0;
        return;
    }
    uint16_t ctrl = fetchHalf(desc);
    if (!(ctrl & 1u)) { /* VALID absent : fin de chaîne -> canal suspendu ;
                         * le RESUME matériel re-fetch CE descripteur (le
                         * driver le réécrit avant de reprendre) */
        dmaOn[ch] = 0;
        dmacIntFlag[ch] |= 0x04; /* SUSP */
        dmaFerr[ch] = 1;         /* CHSTATUS.FERR (datasheet 20.6.2.8) */
        dmac_raise(ch, 0x04);
        dmaResumeAt[ch] = desc;
        return;
    }
    dmaFerr[ch] = 0;
    dmaCtrl[ch] = ctrl;
    dmaCnt[ch] = fetchHalf(desc + 0x02);
    dmaSrc[ch] = fetchWord(desc + 0x04);
    dmaDst[ch] = fetchWord(desc + 0x08);
    dmaNext[ch] = fetchWord(desc + 0x0c);
    dmaIdx[ch] = 0;
    dmaOn[ch] = 1;
    if (ENVFLAG("EMU_DESC_DEBUG") && (dmaDst[ch] == 0x42001828u || dmaSrc[ch] == 0x42001828u || dmaTrig[ch] == DMAC_TRIG_SERCOM4_TX)) {
        static int dn;
        if (dn < 4000)
            fprintf(stderr, "[desc] t=%u ch=%u desc=%x ctrl=%04x n=%u src=%x dst=%x nxt=%x\n",
                    tickCount, ch, desc, dmaCtrl[ch], dmaCnt[ch], dmaSrc[ch], dmaDst[ch], dmaNext[ch]);
        dn++;
    }
}

/* CHCTRLA de canal indexé : SWRST/enable/désactivation — identique en
 * écriture mot et octet (la lib officielle utilise les deux) */
static void dmac_chctrla_write(uint32_t ch, uint32_t v) {
    if ((v & 0x03u) == 0x02u) {
        uint32_t desc = dmac_baseAddr ? dmac_baseAddr + ch * 0x10 : 0;
        if (desc) dma_load(ch, desc);
    } else if (v & 0x01u) {
        dmac_chan_swrst(ch);
    } else {
        dmaOn[ch] = 0; /* disable */
    }
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
    if (dmaDbg < 0) dmaDbg = ENVFLAG("EMU_DMA_DEBUG") ? 1 : 0;
    if (dmaDbg && (dst == 0x42001828u || dst < 0x20000000u)) /* SPI ou canal égaré */
        fprintf(stderr, "[dma] tick=%u ch=%u beat src=%x dst=%x ctrl=%04x idx=%u cnt=%u\n",
                tickCount, ch, src, dst, dmaCtrl[ch], dmaIdx[ch], dmaCnt[ch]);
    if (dst == 0x42001828u && size == 1) {
        /* beat SPI : n'horloge la carte SD que pour un dummy 0xFF pendant
         * une transaction active — sinon c'est un pixel du display, et le
         * collisionneur viderait la file SD au milieu d'un échange CPU */
        uint8_t b = fetchByte(src);
        /* pendant l'écriture d'un secteur (CMD24), chaque beat du canal TX
         * de la carte porte un octet de données ; les beats des autres
         * canaux (affichage) ne doivent rien horloger ni perturber */
        int sdClock = sd_selected() &&
                      (sd_writing ? (int)ch == sdTxCh
                                  : (b == 0xffu && (sd_outLen != 0 || sd_cmdIdx != 0)));
        dmaBeatSkipSd = !sdClock;
        spiDmaWrite = 1;
        writeByte(dst, b);
        spiDmaWrite = 0;
        dmaBeatSkipSd = 0;
        if (sdClock) dma_sercom4_rx_beat(); /* plein-duplex SD */
    } else {
        spiDmaWrite = 1;
        if (size == 1) writeByte(dst, fetchByte(src));
        else if (size == 2) writeHalf(dst, fetchHalf(src));
        else writeWord(dst, fetchWord(src));
        spiDmaWrite = 0;
    }
    if (++dmaIdx[ch] >= dmaCnt[ch]) {
        /* TCMPL PAR DESCRIPTEUR : le matériel lève l'interruption à chaque
         * fin de bloc (chaîné ou pas) — les libs comptent les descripteurs
         * libres via les callbacks de l'ISR (dma_desc_free_count) */
        dmacIntFlag[ch] |= 0x02;
        dma_wrb_write(ch);
        dmac_raise(ch, 0x02);
        /* BLOCKACT (datasheet 20.6.3.2) : 0x2/0x3 suspend le canal après le
         * bloc (lib officielle : descripteurs 0x0419/0x04f9) — il reste
         * activé mais hors arbitrage jusqu'au CHCTRLB.CMD=RESUME ; sinon
         * chaînage automatique vers le descripteur suivant */
        if (((dmaCtrl[ch] >> 3) & 3u) >= 0x2u && !dmaSkipSuspend[ch]) {
            dmaOn[ch] = 0;
            dmacIntFlag[ch] |= 0x04; /* SUSP (sans ré-armement : pas de tempête) */
            dmaResumeAt[ch] = dmaNext[ch];
        } else {
            dmaSkipSuspend[ch] = 0;
            dma_load(ch, dmaNext[ch]); /* suit la chaîne, ou suspend si invalide */
        }
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
            dmac_raise(ch, 0x02);
            dma_load(ch, dmaNext[ch]);
        }
    }
}

static void advance(uint32_t n);
static void timers_sync(void);

/* ------------------------------------------------- interruptions */


/* ------------------------------------------------ NVIC (Cortex-M0+) ----
 * Modèle du vrai cœur (ARMv6-M), qui remplace l'injection « à tout moment »
 * héritée de l'émulateur TS :
 *  - PRIMASK (CPSID/CPSIE, MSR) masque toute prise d'exception configurable ;
 *  - une exception ne préempte que si sa priorité est STRICTEMENT plus
 *    haute que la priorité courante — jamais elle-même ni une égale (le
 *    DMAC_Handler ne se réentre plus : fin de la « tempête » du 05/10) ;
 *  - ISER/ICER (activation), ISPR/ICPR, IPR0-7, SHPR3 (SysTick), ICSR ;
 *  - trame de 8 mots alignée sur 8 octets (bit 9 du xPSR empilé), xPSR
 *    réel (N31 Z30 C29 V28 T24 + IPSR), EXC_RETURN 0xFFFFFFF9 (vers thread)
 *    ou 0xFFFFFFF1 (exception imbriquée).
 * Numéros d'exception : SysTick 15, IRQ n -> 16 + n (DMAC 6, TC4 19, TC5 20).
 */
#define IRQ_DMAC 6
#define IRQ_TC4 19
#define IRQ_TC5 20
static uint32_t nvicIser, nvicPend;     /* IRQ activées / en attente (bit n = IRQ n) */
static uint8_t  nvicIpr[32];            /* priorité par IRQ (bits 7:6) */
static uint8_t  shprSysTick;            /* SHPR3[31:24] */
static int      sysTickPend;            /* ICSR.PENDSTSET */
static uint8_t  excNum[16], excPri[16]; /* pile des exceptions actives */
static int      excDepth;

static void nvic_reset(void) {
    nvicIser = nvicPend = 0;
    memset(nvicIpr, 0, sizeof nvicIpr);
    shprSysTick = 0;
    sysTickPend = 0;
    excDepth = 0;
    irqWork = 1;
}

static int exc_priority(int exc) {
    if (exc == 15) return shprSysTick >> 6;
    return nvicIpr[(exc - 16) & 31] >> 6;
}
static int exc_current_priority(void) { return excDepth ? excPri[excDepth - 1] : 4; }
static int exc_active(void) { return excDepth ? excNum[excDepth - 1] : 0; }

static uint32_t exc_vector(int exc) {
    if (exc == 15) return sysTickVector;
    if (exc == 16 + IRQ_DMAC) return dmacVector;
    if (exc == 16 + IRQ_TC4) return tc4Vector;
    if (exc == 16 + IRQ_TC5) return tc5Vector;
    return fetchWord(vectorBase + 4u * (uint32_t)exc) & ~1u;
}

static void nvic_enter(int exc) {
    uint32_t psr = (fN ? 1u << 31 : 0) | (fZ ? 1u << 30 : 0) | (fC ? 1u << 29 : 0) |
                   (fV ? 1u << 28 : 0) | (1u << 24) | (uint32_t)exc_active();
    if (regs[13] & 4u) { regs[13] -= 4u; psr |= 1u << 9; } /* alignement 8 (STKALIGN) */
    pushStack(psr);
    pushStack(regs[15] - 2u); /* adresse de l'instruction à reprendre */
    pushStack(regs[14]);
    pushStack(regs[12]);
    pushStack(regs[3]);
    pushStack(regs[2]);
    pushStack(regs[1]);
    pushStack(regs[0]);
    regs[14] = excDepth ? 0xfffffff1u : 0xfffffff9u;
    if (excDepth < 16) { excNum[excDepth] = (uint8_t)exc; excPri[excDepth] = (uint8_t)exc_priority(exc); excDepth++; }
    if (exc == 15) { sysTickPend = 0; sysTickEntries++; }
    else nvicPend &= ~(1u << (exc - 16));
    regs[15] = exc_vector(exc);
    incrementPc(); /* même convention de PC que le reste du cœur */
    advance(14); /* latence d'entrée du M0+ : 15 cycles */
}

/* lignes de niveau : un flag encore levé (et activé) à la sortie du handler
 * remet l'IRQ en attente, comme la ligne du périphérique sur le NVIC */
static int dmac_line(void) {
    for (uint32_t ch = 0; ch < DMAC_CHANNELS; ch++)
        if (dmacIntFlag[ch] & dmacIntEn[ch] & 0x07u) return 1;
    return 0;
}

/* exceptions périphériques posées par le reste de l'émulateur (drapeaux
 * historiques) -> bits d'attente du NVIC, puis prise éventuelle */
static void nvic_service(void) {
    if (!irqWork) return; /* rien n'a changé depuis le dernier examen */
    if (dmacInterrupt) { dmacInterrupt = 0; nvicPend |= 1u << IRQ_DMAC; }
    if (tc4Interrupt) { tc4Interrupt = 0; nvicPend |= 1u << IRQ_TC4; }
    if (tc5Interrupt) { tc5Interrupt = 0; nvicPend |= 1u << IRQ_TC5; }
    /* bloqué (PRIMASK, priorité) ou rien de prêt : on attend le prochain
     * changement (nouvelle IRQ, SysTick, CPSIE/MSR, retour d'exception,
     * écriture NVIC), qui relève irqWork */
    irqWork = 0;
    if (primask) return;
    uint32_t ready = nvicPend & nvicIser;
    if (!ready && !sysTickPend) return;
    int best = 0, bestPri = exc_current_priority();
    if (sysTickPend && exc_priority(15) < bestPri) { best = 15; bestPri = exc_priority(15); }
    for (int n = 0; ready; n++, ready >>= 1)
        if ((ready & 1u) && exc_priority(16 + n) < bestPri) { best = 16 + n; bestPri = exc_priority(16 + n); }
    if (best) { nvic_enter(best); irqWork = 1; /* d'autres peuvent attendre (enchaînement) */ }
}

/* retour d'exception (PC sur EXC_RETURN & ~1) */
static void nvic_return(void) {
    setReg(0, popStack());
    setReg(1, popStack());
    setReg(2, popStack());
    setReg(3, popStack());
    setReg(12, popStack());
    setReg(14, popStack());
    uint32_t pc = popStack();
    uint32_t psr = popStack();
    regs[15] = (pc & ~1u) + 2u;
    fN = (psr >> 31) & 1; fZ = (psr >> 30) & 1; fC = (psr >> 29) & 1; fV = (psr >> 28) & 1;
    if (psr & (1u << 9)) regs[13] += 4u;
    advance(10); /* dépilement de la trame + rechargement */
    irqWork = 1;
    if (excDepth) {
        int exc = excNum[--excDepth];
        if (exc == 16 + IRQ_DMAC && dmac_line()) nvicPend |= 1u << IRQ_DMAC;
        if (exc == 16 + IRQ_TC4 && (tc4IntFlagMask & tc4IntEnMask)) nvicPend |= 1u << IRQ_TC4;
        if (exc == 16 + IRQ_TC5 && (tc5IntFlagMask & tc5IntEnMask)) nvicPend |= 1u << IRQ_TC5;
    }
}

/* registres du SCS (0xE000E000-0xE000EFFF), accès mot */
static uint32_t scs_read(uint32_t a) {
    switch (a) {
    case 0xe000e010u: { /* SYST_CSR : ENABLE|TICKINT|CLKSOURCE (+ COUNTFLAG) */
        uint32_t v = 0x7u;
        if (sysTickCountFlag) { v |= 1u << 16; sysTickCountFlag = 0; }
        return v; }
    case 0xe000e014u: return emuTicksPerMs - 1u;                                        /* SYST_RVR */
    case 0xe000e018u: return (emuTicksPerMs - 1u) - ((uint32_t)sysTickTrigger % emuTicksPerMs); /* SYST_CVR */
    case 0xe000e01cu: return 0;                                                         /* SYST_CALIB */
    case 0xe000e100u: return nvicIser;                                                  /* ISER */
    case 0xe000e180u: return nvicIser;                                                  /* ICER */
    case 0xe000e200u: case 0xe000e280u: return nvicPend;                                /* ISPR/ICPR */
    case 0xe000ed00u: return 0x410cc601u;                                               /* CPUID : Cortex-M0+ r0p1 */
    case 0xe000ed04u:                                                                   /* ICSR */
        return (sysTickPend ? 1u << 26 : 0) | ((nvicPend & nvicIser) ? 1u << 22 : 0) | (uint32_t)exc_active();
    case 0xe000ed20u: return (uint32_t)shprSysTick << 24;                               /* SHPR3 */
    }
    if (a >= 0xe000e400u && a < 0xe000e420u) {                                          /* IPR0-7 */
        uint32_t i = a - 0xe000e400u;
        return nvicIpr[i] | (uint32_t)nvicIpr[i + 1] << 8 | (uint32_t)nvicIpr[i + 2] << 16 | (uint32_t)nvicIpr[i + 3] << 24;
    }
    return 0;
}
static void scs_write(uint32_t a, uint32_t v) {
    irqWork = 1;
    switch (a) {
    case 0xe000e018u: sysTickTrigger = 0; return;          /* SYST_CVR : toute écriture le remet à 0 */
    case 0xe000e100u: nvicIser |= v; return;
    case 0xe000e180u: nvicIser &= ~v; return;
    case 0xe000e200u: nvicPend |= v; return;
    case 0xe000e280u: nvicPend &= ~v; return;
    case 0xe000ed04u:                                       /* ICSR : PENDSTSET/PENDSTCLR */
        if (v & (1u << 26)) sysTickPend = 1;
        if (v & (1u << 25)) sysTickPend = 0;
        return;
    case 0xe000ed20u: shprSysTick = (uint8_t)((v >> 24) & 0xc0u); return;
    }
    if (a >= 0xe000e400u && a < 0xe000e420u) {
        uint32_t i = a - 0xe000e400u;
        for (int k = 0; k < 4; k++) nvicIpr[i + k] = (uint8_t)((v >> (8 * k)) & 0xc0u);
    }
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
    if (a == 0x42001818u) { /* SERCOM4 INTFLAG : DRE, TXC, RXC selon le temps émulé */
        uint32_t now = tickCount, v = 0;
        *handled = 1;
        if ((int32_t)(spiPrevDone - now) <= 0) v |= 0x01u;            /* DRE : au plus un octet en vol */
        if ((int32_t)(spiLastDone - now) <= 0) v |= 0x02u;            /* TXC */
        if (spiRxN && (int32_t)(spiRx[0].done - now) <= 0) v |= 0x04u; /* RXC */
        return v;
    }
    if (a == 0x42001c18u) { *handled = 1; return 0x07; } /* SERCOM5 INTFLAG */
    if (a == 0x42001828u) { *handled = 1;
        if (spiRxN) { /* lecture : retire l'octet le plus ancien du tampon */
            uint8_t d = spiRx[0].d;
            spiRx[0] = spiRx[1];
            spiRxN--;
            return d;
        }
        return ser4_data; }                /* SERCOM4 DATA */
    if (a == 0x42001c28u) { *handled = 1; return 0x80; }                     /* SERCOM5 DATA */
    if (a == 0x41004840u) { /* CHCTRLA : état réel du canal.
                             * La lib officielle lit ENABLE avant de réarmer (sendBuffer :
                             * start = !(CHCTRLA.bit.ENABLE)) — renvoyer 0 pendant un
                             * transfert la poussait à réarmer en plein vol, tronquant
                             * le bloc en cours (écran cisaillé/frozen des jeux INDEX). */
        *handled = 1;
        return dmaOn[dmac_chid] ? 2 : 0;
    }
    if (a == 0x41004844u) { /* CHCTRLB : TRIGSRC relu — le guest fait
                             * « CHCTRLB.reg |= CMD_RESUME » en RMW ; renvoyer 0
                             * lui faisait écrire une valeur sans TRIGSRC, qui
                             * effaçait le déclencheur du canal (RX SD mort). */
        *handled = 1;
        return (uint32_t)dmaTrig[dmac_chid & 0xfu] << 8;
    }
    if (a == 0x4100484eu) { *handled = 1; return dmacIntFlag[dmac_chid & 0xfu]; } /* DMAC CHINTFLAG (fenêtre CHID) */
    if (a == 0x4100484fu) { /* CHSTATUS (fenêtre CHID) : FERR=bit2, BUSY=bit1 */
        uint32_t ch = dmac_chid & 0xfu;
        *handled = 1;
        return ch < DMAC_CHANNELS ? (uint32_t)((dmaFerr[ch] ? 0x04u : 0u) | (dmaOn[ch] ? 0x02u : 0u)) : 0u;
    }
    if (a >= 0x4100485eu && a < 0x41004900u && ((a - 0x4100484eu) & 0xfu) == 0) {
        *handled = 1; return dmacIntFlag[(a - 0x4100484eu) >> 4]; } /* CHINTFLAG indexé (canaux 1+) */
    if (a >= 0x4100485du && a < 0x41004900u && ((a - 0x4100484du) & 0xfu) == 0) {
        *handled = 1; return dmacIntEn[(a - 0x4100484du) >> 4]; } /* CHINTENSET indexé */
    if (a == 0x41004014u) { *handled = 1; return nvmIntFlag; }  /* NVM INTFLAG : READY */
    /* SysTick système (0xE000E010+) : le micros() du core Arduino le lit
     * (GSFX du port Picomon notamment) — CSR avec COUNTFLAG, RVR, CVR */
    if (a == 0xe000e010u) { /* CSR : ENABLE|TICKINT (+ COUNTFLAG si wrap) */
        *handled = 1;
        uint32_t v = 0x7u;
        if (sysTickCountFlag) { v |= 1u << 16; sysTickCountFlag = 0; }
        return v;
    }
    if (a == 0xe000e014u) { *handled = 1; return emuTicksPerMs - 1u; }      /* RVR : 1 tick/ms du domaine émulé */
    if (a == 0xe000e018u) { *handled = 1; return (emuTicksPerMs - 1u) - (sysTickTrigger % emuTicksPerMs); } /* CVR */
    if (a == 0xe000e01cu) { *handled = 1; return 0; }           /* CALIB */
    if (a == 0x41004018u) { *handled = 1; return 0; }           /* NVM STATUS */
    if (a == 0x41004820u) { /* INTPEND : premier canal avec un drapeau levé
                             * (datasheet : bit4 TCMPL, bit5 SUSP, bit6 TERR —
                             * l'ancien encodage mettait TCMPL en bit6, le guest
                             * y lisait TERR et partait dans son chemin d'erreur) */
        *handled = 1;
        for (uint32_t ch = 0; ch < DMAC_CHANNELS; ch++) {
            if (dmacIntFlag[ch]) {
                return (ch & 0xfu)
                     | (((dmacIntFlag[ch] >> 1) & 1u) << 4)  /* TCMPL */
                     | (((dmacIntFlag[ch] >> 2) & 1u) << 5)  /* SUSP */
                     | ((dmacIntFlag[ch] & 1u) << 6);        /* TERR */
            }
        }
        return 0;
    }
    if (a == 0x4200300du) { *handled = 1; return tc4IntEnMask; }             /* TC4 INTENSET */
    if (a == 0x4200300eu) { *handled = 1; return tc4IntFlagMask; }           /* TC4 INTFLAG */
    if (a == 0x4200340du) { *handled = 1; return tc5IntEnMask; }             /* TC5 INTENSET */
    if (a == 0x4200340eu) { *handled = 1; return tc5IntFlagMask; }           /* TC5 INTFLAG */
    return 0;
}
/* alias physique de la flash (0x00400000) : certaines libs y accèdent
 * directement pour leurs données (images embarquées streamees en DMA) */
#define FLASH_PHYS_BASE 0x00400000u

/* accès mémoire du bus (maîtres DMA, pile du cœur hors chemins rapides) :
 * plage SRAM d'abord, puis voie Pokitto (pk_read_/pk_write_), puis segments
 * META — l'ancienne macro SRAM_FAST refaisait le test de cible à chaque
 * étage, et Pokitto traversait deux niveaux de fonctions non inlinées. */
static uint32_t fetchWord(uint32_t a) {
    if (emuTarget == TGT_POKITTO) return pk_read_word(a);
    uint32_t o = a - 0x20000000u;
    if (o <= SRAM_SIZE - 4u) { uint32_t v; memcpy(&v, sram + o, 4); return v; }
    if (a >= FLASH_PHYS_BASE && a < FLASH_PHYS_BASE + FLASH_SIZE) a -= FLASH_PHYS_BASE;
    if (a < 0x20000000u) { nvm_access(a); if (a + 4 > FLASH_SIZE) return 0;
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
    if (a >= 0xe000e000u && a < 0xe000f000u) return scs_read(a);
    return 0;
}

static uint16_t fetchHalf(uint32_t a) {
    if (emuTarget == TGT_POKITTO) return pk_read_half(a);
    uint32_t o = a - 0x20000000u;
    if (o <= SRAM_SIZE - 2u) { uint16_t v; memcpy(&v, sram + o, 2); return v; }
    if (a >= FLASH_PHYS_BASE && a < FLASH_PHYS_BASE + FLASH_SIZE) a -= FLASH_PHYS_BASE;
    if (a < 0x20000000u) { nvm_access(a); if (a + 2 > FLASH_SIZE) return 0;
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
    if (a >= 0xe000e000u && a < 0xe000f000u) return (uint16_t)(scs_read(a & ~3u) >> (8 * (a & 2u)));
    return 0;
}

static uint8_t fetchByte(uint32_t a) {
    if (emuTarget == TGT_POKITTO) return pk_read_byte(a);
    uint32_t o = a - 0x20000000u;
    if (o < SRAM_SIZE) return sram[o];
    if (a >= FLASH_PHYS_BASE && a < FLASH_PHYS_BASE + FLASH_SIZE) a -= FLASH_PHYS_BASE;
    if (a < 0x20000000u) { nvm_access(a); return RD8(a); }
    if (a < 0x40000000u) return RD8S(a - 0x20000000u);
    if (a == 0x40000c00u) return 0;           /* GCLK CTRL : pas de reset en cours */
    if (a == 0x42004018u) return 1;           /* ADC INTFLAG RESRDY */
    if (a >= 0xe000e000u && a < 0xe000f000u) return (uint8_t)(scs_read(a & ~3u) >> (8 * (a & 3u)));
    int handled;
    uint32_t v = periph_read(a, &handled);
    if (handled) return (uint8_t)v;
    return 0;
}

int dbgDac = 0;
static int dbgTc4Cfg = 0;
static int dbgEnabled = -1; /* EMU_DEBUG=1 : traces de config périphériques */
static int dbg(void) {
    if (dbgEnabled < 0) dbgEnabled = getenv("EMU_DEBUG") ? 1 : 0;
    return dbgEnabled;
}
/* montre générique d'écriture (WATCH_ADDR), pour le débogage */
static uint32_t prevInstPc;  /* PC à l'entrée du pas courant (la boucle le tient à jour) */
/* programmation flash (auto-patch des loaders) : effet net du NVMCTRL —
 * la valeur écrite est stockée en flash et le code fraîchement écrit est
 * exécuté aux pas suivants.  Le contrôleur (0x41004000+) est modélisé au
 * strict minimum : ADDR retenu, commande EP (effacement de page de 64 o)
 * appliquée, INTFLAG relu avec READY=1 — les installeurs des loaders
 * sondent READY avant de rendre la main (Picomon rend le CPSID sans le
 * CPSIE si le sondage ne passe pas : interruptions mortes, écran noir).
 * L'alias physique 0x00400000 est accepté en écriture aussi.
 *
 * Build wasm : écritures simplement IGNORÉES (comportement d'avant) — la
 * distribution web privilégie le démarrage partout ; les écrans de boot
 * sont identiques, seuls les auto-patchs des loaders restent inertes. */
#ifdef __EMSCRIPTEN__
static void flash_store(uint32_t a, uint32_t v, int bytes) {
    (void)a; (void)v; (void)bytes;
}
static void nvmctrl_write(uint32_t a, uint32_t v) {
    (void)a; (void)v; /* wasm : pas d'auto-patch, pas de contrôleur */
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

/* NVMCTRL minimal (0x41004000+) : ADDR + commande EP (effacement de page
 * de 64 o) ; INTFLAG relu avec READY — les installeurs des loaders
 * sondent ce drapeau avant de réactiver les interruptions */
static void nvmctrl_write(uint32_t a, uint32_t v) {
    if (a == 0x41004008u) { nvmAddr = v; return; }             /* ADDR */
    if (a == 0x41004004u || a == 0x41004005u) {                /* CTRLB.CMD */
        uint32_t cmd = (a == 0x41004004u) ? (v & 0x7fu) : (v >> 8) & 0x7fu;
        if (cmd == 0x03u && nvmAddr < FLASH_SIZE) {            /* EP */
            uint32_t page = nvmAddr & ~63u;
            if (page + 64 <= FLASH_SIZE) memset(flash + page, 0xff, 64);
        }
        nvmIntFlag = 1; /* commande acceptée : READY */
        return;
    }
    if (a == 0x41004014u) { nvmIntFlag = 1; return; }          /* INTFLAG clear */
}
#endif /* __EMSCRIPTEN__ */

/* ré-armement level-triggered : le NVIC ré-entre tant qu'un AUTRE canal a
 * une fin de transfert (TCMPL) à la fois levée (CHINTFLAG) et activée
 * (CHINTENSET).  Un canal dont l'interruption TCMPL n'est pas activée ne
 * tire pas la ligne du tout (le canal audio des firmwares gbrecomp : sa
 * fin de bloc est servie par le TC4, jamais par le DMAC Handler — sans ce
 * filtre, son flag TCMPL jamais acquitté ré-armait l'interruption en
 * boucle : tempête de réentrance du handler, la pile sortait de la SRAM
 * et le jeu déraillait en pc fou). */
static void dmac_rearm_from(uint32_t done_ch) {
    for (uint32_t k = 0; k < DMAC_CHANNELS; k++)
        if (k != done_ch && (dmacIntFlag[k] & 0x02u) && (dmacIntEn[k] & 0x02u)) { dmacInterrupt = 1; irqWork = 1; return; }
}

/* registres 8 bits de canal indexé (Channel[n].CHINTENCLR/SET/FLAG),
 * partagés par les écritures octet et mot (le mot à 0x...C porte CLR en
 * octet bas et SET en octet haut) */
static void dmac_indexed_intwrite(uint32_t ch, uint32_t off, uint32_t v, int width_bytes) {
    if (ch >= DMAC_CHANNELS) return;
    if (off == 0xc) { dmacIntEn[ch] &= (uint8_t)~v; return; }            /* CHINTENCLR */
    if (off == 0xd) { dmacIntEn[ch] |= (uint8_t)v; return; }             /* CHINTENSET */
    if (off == 0xe) {                                                    /* CHINTFLAG : acquitter */
        dmacIntFlag[ch] &= (uint8_t)~v;
        dmac_rearm_from(ch);
    }
    (void)width_bytes;
}

/* fenêtre CHID : les octets 0x...C/0x...D portent CHINTENCLR/SET du canal
 * sélectionné (le mot 0x...C les porte tous les deux) */
static void dmac_window_intwrite(uint32_t a, uint32_t v, int width_bytes) {
    uint32_t ch = dmac_chid & 0xfu;
    if (ch >= DMAC_CHANNELS) return;
    if (width_bytes == 4 && a == 0x4100484cu) {
        dmacIntEn[ch] &= (uint8_t)~v;
        dmacIntEn[ch] |= (uint8_t)(v >> 8);
        return;
    }
    if (a == 0x4100484cu) { dmacIntEn[ch] &= (uint8_t)~v; return; }      /* CHINTENCLR */
    if (a == 0x4100484du) { dmacIntEn[ch] |= (uint8_t)v; return; }       /* CHINTENSET */
}

static void writeWord(uint32_t a, uint32_t v) {
    uint32_t o = a - 0x20000000u;
    if (o <= SRAM_SIZE - 4u) { memcpy(sram + o, &v, 4); return; }
    if (emuTarget == TGT_POKITTO) { pk_write_word(a, v); return; }
    if (a >= 0x40000000u) timers_sync(); /* état des timers/DMA modifiable */
    if ((a & ~7u) == 0x42001800u) sercom4_ctrl_write(a & ~3u, (uint32_t)v << (8 * (a & 3u)));
    if (a >= 0xe000e000u && a < 0xe000f000u) { scs_write(a, v); return; } /* SCS : NVIC, SysTick, SCB */
    dmac_window_intwrite(a, v, 4); /* 0x4100484C : CHINTENCLR|SET (fenêtre, mot) */
    if (a >= 0x41004000u && a < 0x41004020u) { nvmctrl_write(a, v); return; }
    if (a < 0x20000000u) { flash_store(a, v, 4); return; }
    if (a < 0x40000000u) { a -= 0x20000000u; if (a + 4 > SRAM_SIZE) return;
        sram[a] = v & 0xff; sram[a+1] = (v >> 8) & 0xff;
        sram[a+2] = (v >> 16) & 0xff; sram[a+3] = (v >> 24) & 0xff; return; }
    if ((a & ~0x1fu) == 0x41004400u) { port_write(0, a & 0x1f, v); return; }
    if ((a & ~0x1fu) == 0x41004480u) { port_write(1, a & 0x1f, v); return; }
    if (a == 0x42001828u) { sercom4_write((uint8_t)v); return; } /* SERCOM4 DATA */
    if (a == 0x42004808u) { dac_write((uint16_t)v); return; }    /* DAC DATA */
    if (a == 0x42003000u) { if (dbg() && dbgTc4Cfg < 8) { fprintf(stderr, "[dbg] CTRLA word <- %x\n", v); dbgTc4Cfg++; } tc4_write_ctrla(v); return; }
    if (a == 0x42003018u) { tc4_write_cc0(v); return; }          /* TC4 CC0 */
    if (a == 0x42003400u) { tc5_write_ctrla(v); return; }        /* TC5 CTRLA */
    if (a == 0x42003418u) { tc5_write_cc0(v); return; }          /* TC5 CC0 */
    if (a == 0x4200340cu) { tc_intclr(1, v); return; }           /* TC5 INTENCLR */
    if (a == 0x4200340du) { tc_inten(1, v); return; }            /* TC5 INTENSET : MC0=0x10 (lib) */
    if (a == 0x4200340eu) { tc5IntFlagMask &= (uint8_t)~v; return; }  /* TC5 INTFLAG */
    if (a == 0x4200300cu) { tc_intclr(0, v); return; }           /* TC4 INTENCLR */
    if (a == 0x4200300du) { tc_inten(0, v); return; }            /* TC4 INTENSET : OVF=0x01 (jeux maison), MC0=0x10 (lib standard) */
    if (a == 0x4200300eu) { tc4IntFlagMask &= (uint8_t)~v; return; } /* TC4 INTFLAG (acquittement) */
    /* GCLK GENDIV/GENCTRL : sans effet (la cadence TC4 est dérivée de
     * CTRLA/CC0, et la fréquence du générateur vaut 48 MHz pour les
     * configurations audio rencontrées — lib standard comme jeux maison) */
    if (a == 0x40000c04u || a == 0x40000c08u) return;
    if (a == 0x41004834u) { dmac_baseAddr = v; return; }
    if (a == 0x41004838u) { dmac_wrbAddr = v; return; }
    if (a == 0x4100483fu) { dmac_chid = v; if (ENVFLAG("EMU_DESC_DEBUG")) fprintf(stderr, "[chid] t=%u chid=%u\n", tickCount, v); return; }
    /* la lib officielle adresse les canaux en INDEXÉ — 16 octets par canal :
     * Channel[n] = 0x41004840 + n*16, CHCTRLA@+0, CHCTRLB@+4, CHINTENCLR@+C,
     * CHINTENSET@+D, CHINTFLAG@+E.  Les canaux 1+ (0x50+) arrivent ici ; les
     * adresses 0x40-0x4F sont la fenêtre CHID (= canal 0 pour la lib, qui
     * n'écrit jamais CHID) et passent par les handlers CHID ci-dessous. */
    if (a >= 0x41004850u && a < 0x41004900u) {
        uint32_t ch = (a - 0x41004840u) >> 4;
        uint32_t off = (a - 0x41004840u) & 0xfu;
        if (ch < DMAC_CHANNELS && (off == 0 || off == 4 || (off >= 0xc && off <= 0xe))) {
            if (off == 0) { dmac_chctrla_write(ch, v); return; } /* CHCTRLA */
            if (off == 4) { dmaTrig[ch] = (uint8_t)((v >> 8) & 0x3fu); return; } /* CHCTRLB.TRIGSRC */
            if (off == 0xc || off == 0xd || off == 0xe) {
                dmac_indexed_intwrite(ch, off, v, 4);
                return;
            }
        }
        return;
    }
    if (a == 0x41004844u && dmac_chid < DMAC_CHANNELS) { /* CHCTRLB */
        /* CMD (bits 25:24, datasheet 20.8.19) : RESUME=0x2 charge le
         * descripteur en attente (suspend après bloc) ; reçu pendant un
         * bloc, il saute le prochain suspend (20.6.3.3).  SUSPEND=0x1 :
         * le canal s'arrête à la fin du bloc en cours. */
        uint32_t cmd = (v >> 24) & 3u;
        if (cmd == 0x2u) {
            if (dmaOn[dmac_chid]) dmaSkipSuspend[dmac_chid] = 1;
            else if (dmaResumeAt[dmac_chid]) {
                uint32_t at = dmaResumeAt[dmac_chid];
                dmaResumeAt[dmac_chid] = 0;
                dma_load(dmac_chid, at);
                /* bloc SPI : ré-armer le cadencement des beats (le RESUME
                 * sort le canal de sa suspension après bloc) */
                if (dmaOn[dmac_chid] && (dmaDst[dmac_chid] == 0x42001828u || dmaTrig[dmac_chid] == DMAC_TRIG_SERCOM4_TX)) {
                    uint32_t b = (spiBaud & 0xFFu) + 1;
                    spiBeatTicks = b * emuTicksPerUs; /* en tiers de tick */
                    if (spiBeatTicks < 3 || ENVFLAG("EMU_SPI_INSTANT")) spiBeatTicks = 3;
                    spiBeatAcc = 0;
                    spiDmaCh = (int)dmac_chid;
                }
            }
        }
        /* comme le TS : seul TRIGSRC est retenu (le reste du champ est
         * reconstitué par les écritures du guest) */
        dmaTrig[dmac_chid] = (uint8_t)((v >> 8) & 0x3fu);
        return;
    }
    if (a == 0x4100484eu && dmac_chid < DMAC_CHANNELS) { /* CHINTFLAG : acquittement */
        dmacIntFlag[dmac_chid] &= (uint8_t)~v;
        /* l'interruption DMAC est level-triggered sur le hardware : tant
         * qu'un autre canal attend une fin de transfert (TCMPL), le NVIC
         * ré-entre — les SUSP périmés ne doivent pas ré-armer ; voir
         * dmac_rearm_from pour le filtre CHINTENSET */
        dmac_rearm_from(dmac_chid);
        return;
    }
    if (a == 0x41004840u && (v & 0x01u)) { /* SWRST (fenêtre CHID) */
        dmac_chan_swrst(dmac_chid & 0xfu);
        return;
    }
    if (a == 0x41004840u && dma_is_tc4(dmac_chid)) { /* canal audio (TC4) */
        if ((v & 0x03u) == 0x02u) { if (!dmaOn[dmac_chid]) audRestarts++; if (!dmaOn[dmac_chid]) dma_load(dmac_chid, dmac_baseAddr + dmac_chid * 0x10); }
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
            if (ENVFLAG("EMU_DESC_DEBUG"))
                fprintf(stderr, "[arm] t=%u chid=%u on=%u trig=%u desc=%x res=%x\n",
                        tickCount, dmac_chid, dmaOn[dmac_chid], dmaTrig[dmac_chid],
                        dmac_desc, dmaResumeAt[dmac_chid]);
            /* matériel : ENABLE sur un canal déjà actif est sans effet —
             * recharge partir du descripteur de tête tronquerait le
             * transfert en cours (octets perdus -> lignes décalées) */
            if (dmaOn[dmac_chid]) return;
            if (!dmac_desc) dmac_desc = dmac_baseAddr + dmac_chid * 0x10;
            /* suspend après bloc en attente : la reprise se fait à la
             * position du canal, pas à la tête de la chaîne */
            if (dmaResumeAt[dmac_chid]) {
                dmac_desc = dmaResumeAt[dmac_chid];
                dmaResumeAt[dmac_chid] = 0;
            }
            if (dmaTrig[dmac_chid] == DMAC_TRIG_SERCOM4_RX) {
                /* réception SD : armé seulement — les beats arrivent au
                 * rythme des octets envoyés (miroir plein-duplex), une
                 * copie instantanée lirait ici 512x le même octet */
                dma_load(dmac_chid, dmac_desc);
                dmac_desc = 0;
                return;
            }
            uint32_t pdst = fetchWord(dmac_desc + 0x08);
            if (sd_writing) {
                /* écriture de secteur en cours : l'armement du canal TX de
                 * la carte doit passer (ce sont ses beats qui portent les
                 * données) ; ceux de l'affichage seront sautés au beat */
                if (dmaTrig[dmac_chid] == DMAC_TRIG_SERCOM4_TX || pdst == 0x42001828u)
                    sdTxCh = (int)dmac_chid;
            }
            if (dmaTrig[dmac_chid] == DMAC_TRIG_SERCOM4_TX || pdst == 0x42001828u) {
                /* écran : beats cadencés par le baud SPI, le CPU vit pendant */
                dma_load(dmac_chid, dmac_desc);
                if (dmaOn[dmac_chid]) {
                    uint32_t b = (spiBaud & 0xFFu) + 1;
                    spiBeatTicks = b * emuTicksPerUs; /* tiers de tick : 8 bits @ f/2(1+b), 3 Mo/s à BAUD=0 */
                    if (spiBeatTicks < 3 || ENVFLAG("EMU_SPI_INSTANT")) spiBeatTicks = 3; /* option : 1 octet/tick */
                    spiBeatAcc = 0;
                    spiDmaCh = (int)dmac_chid;
                    { static int n; if (n++ < 6)
                        fprintf(stderr, "[dma spi] ch=%u ctrl=%04x src=%x dst=%x n=%u beats=%u t\n",
                                (unsigned)dmac_chid, dmaCtrl[dmac_chid], dmaSrc[dmac_chid],
                                dmaDst[dmac_chid], dmaCnt[dmac_chid], spiBeatTicks); }
                    if (ENVFLAG("EMU_CHUNK_DEBUG") && dmaCnt[dmac_chid] == 320) {
                        static int cn;
                        uint32_t base = dmaSrc[dmac_chid] - 320; /* SRCINC : base du bloc */
                        if (cn < 300 || (cn % 64) == 0) {
                            fprintf(stderr, "[chunk %u] t=%u base=%x b0..23:", cn, tickCount, base);
                            for (int k = 0; k < 24; k++)
                                fprintf(stderr, " %02x", fetchByte(base + k));
                            fprintf(stderr, "\n");
                        }
                        cn++;
                    }
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
            if (ENVFLAG("EMU_DMA_DEBUG") && btcnt > 100) {
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
                spiDmaWrite = 1;
                if (size == 1) writeByte(d, fetchByte(s));
                else if (size == 2) writeHalf(d, fetchHalf(s));
                else writeWord(d, fetchWord(s));
                spiDmaWrite = 0;
            }
            dma_wrb_write(dmac_chid);
            dmac_desc = nxt;
            dmacIntFlag[dmac_chid & 0xfu] |= 0x02;
            dmac_raise(dmac_chid & 0xfu, 0x02); /* verrouillé, traité au prochain step (TS) */
        }
        return;
    }
}

static void writeHalf(uint32_t a, uint16_t v) {
    uint32_t o = a - 0x20000000u;
    if (o <= SRAM_SIZE - 2u) { memcpy(sram + o, &v, 2); return; }
    if (emuTarget == TGT_POKITTO) { pk_write_half(a, v); return; }
    if (a >= 0x40000000u) timers_sync();
    if ((a & ~7u) == 0x42001800u) sercom4_ctrl_write(a & ~3u, (uint32_t)v << (8 * (a & 3u)));
    if (a >= 0x41004000u && a < 0x41004020u) { nvmctrl_write(a, v); return; }
    if (a < 0x20000000u) { flash_store(a, v, 2); return; }
    if (a < 0x40000000u) { a -= 0x20000000u; if (a + 2 > SRAM_SIZE) return;
        sram[a] = v & 0xff; sram[a+1] = (v >> 8) & 0xff; return; }
    if (a == 0x42004808u) { if (dbg() && dbgDac < 3) { fprintf(stderr, "[dbg] DAC half <- %x\n", v); dbgDac++; } dac_write(v); return; }
    if (a >= 0x41004850u && a < 0x41004900u) { /* canaux indexés (demi-mot) */
        dmac_indexed_intwrite((a - 0x41004840u) >> 4, (a - 0x41004840u) & 0xfu, v, 2);
        return;
    }
    dmac_window_intwrite(a, v, 2); /* 0x4100484C/D : CHINTENCLR/SET (fenêtre) */
    if (a == 0x42003000u) { if (dbg() && dbgTc4Cfg < 8) { fprintf(stderr, "[dbg] CTRLA half <- %x\n", v); dbgTc4Cfg++; } tc4_write_ctrla(v); return; }
    if (a == 0x42003018u) { tc4_write_cc0(v); return; }
    if (a == 0x4200300du) { tc_inten(0, v); return; }
    if (a == 0x4200300eu) { tc4IntFlagMask &= (uint8_t)~v; return; }
    if (a == 0x42003400u) { tc5_write_ctrla(v); return; }
    if (a == 0x42003418u) { tc5_write_cc0(v); return; }
    if (a == 0x4200340du) { tc_inten(1, v); return; }
    if (a == 0x4200340eu) { tc5IntFlagMask &= (uint8_t)~v; return; }
    if (a == 0x40000c02u) { if (dbg() && dbgTc4Cfg < 16) { fprintf(stderr, "[gclk] CLKCTRL <- %x\n", v); dbgTc4Cfg++; } return; }
    if (a == 0x40000c04u) { fprintf(stderr, "[gclk] GENDIV <- %x\n", v); return; }
    if (a == 0x40000c08u) { fprintf(stderr, "[gclk] GENCTRL <- %x\n", v); return; }
    if ((a & ~0x1fu) == 0x41004400u) { port_write(0, a & 0x1f, v); return; }
    if ((a & ~0x1fu) == 0x41004480u) { port_write(1, a & 0x1f, v); return; }
    writeWord(a, v);
}

static void writeByte(uint32_t a, uint8_t v) {
    uint32_t o = a - 0x20000000u;
    if (o < SRAM_SIZE) { sram[o] = v; return; }
    if (emuTarget == TGT_POKITTO) { pk_write_byte(a, v); return; }
    if (a >= 0x40000000u) timers_sync();
    if ((a & ~7u) == 0x42001800u) sercom4_ctrl_write(a & ~3u, (uint32_t)v << (8 * (a & 3u)));
    if (a >= 0x41004000u && a < 0x41004020u) { nvmctrl_write(a, v); return; }
    if (a < 0x20000000u) { flash_store(a, v, 1); return; }
    if (a < 0x40000000u) { uint32_t sa = a - 0x20000000u; if (sa < SRAM_SIZE) sram[sa] = v; return; }
    if (a == 0x4200300du) { tc_inten(0, v); return; } /* TC4 INTENSET */
    if (a == 0x4200300eu) { tc4IntFlagMask &= (uint8_t)~v; return; }
    if (a == 0x4200340du) { tc_inten(1, v); return; } /* TC5 INTENSET */
    if (a == 0x4200340eu) { tc5IntFlagMask &= (uint8_t)~v; return; }
    if (a == 0x42001828u) { sercom4_write(v); return; }
    if (a == 0x4200180cu || a == 0x4200180au) { spiBaud = v; return; } /* SERCOM4 BAUD (SPI : 0x0C ; 0x0A = compat TS) */
    if (a == 0x4100484eu) { /* DMAC CHINTFLAG acquittement (fenêtre, octet) */
        dmacIntFlag[dmac_chid & 0xfu] &= (uint8_t)~v;
        dmac_rearm_from(dmac_chid & 0xfu);
        return;
    }
    dmac_window_intwrite(a, v, 1); /* 0x4100484C/D : CHINTENCLR/SET (fenêtre) */
    if (a >= 0x41004850u && a < 0x41004900u) { /* canaux indexés (octet) :
        * CHCTRLA 8 bits et les registres d'interruption — la lib officielle
        * écrit CHINTENSET/CHINTFLAG en strb, le mot seul ne suffit pas */
        uint32_t ch = (a - 0x41004840u) >> 4, off = (a - 0x41004840u) & 0xfu;
        if (ch < DMAC_CHANNELS && off == 0) { dmac_chctrla_write(ch, v); return; } /* CHCTRLA */
        if (ch < DMAC_CHANNELS && off >= 0xc && off <= 0xe) {
            dmac_indexed_intwrite(ch, off, v, 1);
            return;
        }
        return;
    }
    if (a == 0x4100483fu) { dmac_chid = v; if (ENVFLAG("EMU_DESC_DEBUG")) fprintf(stderr, "[chid] t=%u chid=%u\n", tickCount, v); return; }
    if ((a & ~0x1fu) == 0x41004400u) { port_write(0, a & 0x1f, v); return; }
    if ((a & ~0x1fu) == 0x41004480u) { port_write(1, a & 0x1f, v); return; }
    writeWord(a, v);
}

/* --------------------------------------------------- DMAC (écran) */


/* ---------------------------------------------------- SERCOM4 data */


static uint8_t serLast[8]; static long serIdx; static long serNz;
static void sercom4_write(uint8_t v) {
    if (v) serNz++;
    if (ENVFLAG("EMU_DMA_DEBUG")) { serLast[serIdx++ & 7] = v; }
    /* ordre du TypeScript (écran, boutons, carte SD) ; l'octet de réponse
     * repart à 0x80 à chaque échange, les périphériques sélectionnés le
     * remplacent (sercom-register.ts : this.data = 0x80 puis listeners).
     * Boutons : PB03 (lib standard) ; PA25 accepté aussi — même registre à
     * décalage sur le bus, certains jeux maison le pilotent en direct. */
    ser4_data = 0x80;
    st7735_byte(v);
    if ((portB_out & (1u << 3)) == 0 || (portA_out & (1u << 25)) == 0) {
        /* boutons : PB03 (lib standard) ou PA25 (certains jeux maison
         * pilotent ce CS en direct — même registre à décalage).  L'ordre
         * des bits dépend de la vitesse SPI de la lecture : 12 MHz
         * (BAUD=1, jeux lib) = ordre lib ; 24 MHz (BAUD=0, ex. lapinou)
         * = ordre historique des jeux maison. */
        int order24 = (spiBaud & 0xffu) != 1u;
        static int orderForce = -1;
        if (orderForce < 0)
            orderForce = getenv("EMU_BTN_ORDER") && getenv("EMU_BTN_ORDER")[0] == 'l' ? 1 : 0;
        if (orderForce) order24 = 1;
        ser4_data = order24 ? pad_byte_24(buttonData) : buttonData;
        if (ENVFLAG("EMU_BTN_DEBUG") && buttonData != 0xffu) {
            static int bn;
            if (bn++ < 40)
                fprintf(stderr, "[btnread] t=%u v=%02x boutons=%02x PB03=%d PA25=%d baud=%u\n",
                        tickCount, v, buttonData, (portB_out >> 3) & 1,
                        (portA_out >> 25) & 1, spiBaud & 0xFF);
        }
    }
    if (!dmaBeatSkipSd) sd_byte(v);
    { /* temporisation et tampon de réception */
        uint32_t now = tickCount, done;
        if (spiDmaWrite) done = now;
        else {
            uint32_t start = (int32_t)(spiLastDone - now) > 0 ? spiLastDone : now;
            done = start + 16u * ((spiBaud & 0xffu) + 1u);
        }
        spiPrevDone = spiLastDone;
        spiLastDone = done;
        if (spiRxN < 2) { spiRx[spiRxN].d = ser4_data; spiRx[spiRxN].done = done; spiRxN++; }
        /* tampon plein : l'octet est perdu (BUFOVF) */
    }
}

/* ---------------------------------------------------------------- CPU */

/* Avance le temps émulé de n ticks (cycles CPU dans le modèle par
 * défaut) : SysTick, TC4/TC5 (DMA ou interruption) et beats SPI, par lots —
 * les débordements gardent leur phase (compteur -= période). */
static uint32_t timerAcc;    /* ticks pas encore appliqués aux timers */
static uint32_t timerBudget; /* ticks avant le prochain événement (0 = à recalculer) */

static void timers_process(uint32_t n) {
    if (emuTarget == TGT_META) {
        while (sysTickTrigger >= (int)emuTicksPerMs) { /* enroulement de CVR : SysTick en attente */
            sysTickTrigger -= (int)emuTicksPerMs;
            sysTickPend = 1;
            sysTickCountFlag = 1;
            irqWork = 1;
        }
    }
    if (tc4Enabled && tc4Top > 0) {
        /* cadence dérivée de la vraie config (MFRQ) : F = GCLK_TC4 /
         * (prescale × (CC0+1)), GCLK audio = 48 MHz ; le timer tourne
         * librement comme le vrai TC4.  Sans interruption (tc4Armed = 0),
         * chaque débordement déclenche un beat des canaux DMA TC4. */
        uint32_t per = tc4_period_ticks();
        tc4Period = per;
        tc4Counter += n;
        while (tc4Counter >= per) {
            tc4Counter -= per;
            tc4Fires++;
            if (tc4Armed) {
                tc4IntFlagMask |= tc4IntEnMask & 0x11u; /* OVF/MC0 posés, lus par le handler */
                tc4Interrupt = 1; irqWork = 1;
            } else {
                int served = 0;
                for (uint32_t ch = 0; ch < DMAC_CHANNELS; ch++)
                    if (dmaTrig[ch] == DMAC_TRIG_TC4_OVF && dmaOn[ch]) { dma_beat(ch); served = 1; }
                if (!served) audStarvedTicks++; /* beat perdu : canal audio arrêté */
            }
        }
    }
    if (tc5Enabled && tc5Top > 0 && (tc5Armed || dma_is_tc5(dmac_chid))) {
        /* TC5 : interruption (Audio_Handler de la lib : mixer + WAV) ou
         * beats DMA (audio DMA lib) */
        uint32_t per = tc5_period_ticks();
        tc5Period = per;
        tc5Counter += n;
        while (tc5Counter >= per) {
            tc5Counter -= per;
            tc5Fires++;
            if (tc5Armed) {
                tc5IntFlagMask |= tc5IntEnMask & 0x11u; /* OVF/MC0 */
                tc5Interrupt = 1; irqWork = 1;
            } else {
                for (uint32_t ch = 0; ch < DMAC_CHANNELS; ch++)
                    if (dmaTrig[ch] == DMAC_TRIG_TC5_OVF) dma_beat(ch);
            }
        }
    }
    if (spiDmaCh >= 0) { /* beats SPI : un octet tous les spiBeatTicks/3 ticks */
        spiBeatAcc += 3u * n;
        while (spiDmaCh >= 0 && spiBeatAcc >= spiBeatTicks) {
            spiBeatAcc -= spiBeatTicks;
            if (!dmaOn[spiDmaCh]) { spiDmaCh = -1; break; }
            dma_beat(spiDmaCh);
            if (!dmaOn[spiDmaCh]) spiDmaCh = -1; /* bloc terminé : TCMPL (levé par dma_beat) */
        }
    }
}

/* distance (ticks) au prochain événement des timers ; UINT32_MAX si aucun */
static uint32_t timers_next_event(void) {
    uint32_t d = 0xffffffffu;
    if (emuTarget == TGT_META)
        d = sysTickTrigger < (int)emuTicksPerMs ? emuTicksPerMs - (uint32_t)sysTickTrigger : 1u;
    if (tc4Enabled && tc4Top > 0) {
        uint32_t per = tc4_period_ticks();
        uint32_t r = tc4Counter < per ? per - tc4Counter : 1u;
        if (r < d) d = r;
    }
    if (tc5Enabled && tc5Top > 0 && (tc5Armed || dma_is_tc5(dmac_chid))) {
        uint32_t per = tc5_period_ticks();
        uint32_t r = tc5Counter < per ? per - tc5Counter : 1u;
        if (r < d) d = r;
    }
    if (spiDmaCh >= 0) {
        uint32_t need = spiBeatAcc < spiBeatTicks ? spiBeatTicks - spiBeatAcc : 0u;
        uint32_t r = (need + 2u) / 3u; /* ticks pour atteindre le beat */
        if (r < 1u) r = 1u;
        if (r < d) d = r;
    }
    return d;
}

/* applique les ticks différés (avant toute écriture périphérique qui
 * pourrait changer l'état des timers, et à chaque échéance) */
static void timers_sync(void) {
    if (timerAcc) { uint32_t n = timerAcc; timerAcc = 0; timers_process(n); }
    timerBudget = 0;
}

/* Avance le temps émulé de n ticks.  Les timers ne sont traités qu'à
 * l'échéance de leur prochain événement (débordement TC4/TC5, beat SPI) :
 * entre deux, seuls les compteurs avancent — même résultat, beaucoup moins
 * de travail par instruction. */
static void advance_slow(void) {
    uint32_t a = timerAcc;
    timerAcc = 0;
    timers_process(a);
    timerBudget = timers_next_event();
}
static inline void advance(uint32_t n) {
    sysTickTrigger += (int)n;
    tickCount += n;
    if (emuTarget == TGT_POKITTO) return; /* timers LPC dans pk_machine_step */
    timerAcc += n;
    if (timerAcc >= timerBudget) advance_slow();
}

/* PC + 2 et un tick à appliquer : le temps n'est appliqué qu'une fois par
 * instruction, en fin de step (step_flush), avec son coût en cycles */
static uint32_t stepTicks;
static inline void incrementPc(void) {
    regs[15] += 2;
    stepTicks++;
}
static inline void step_flush(uint32_t minTicks) {
    uint32_t t = stepTicks > minTicks ? stepTicks : minTicks;
    stepTicks = 0;
    if (t) advance(t);
}

/* (traceAllStep est local à step) */
static void pushStack(uint32_t v) { regs[13] -= 4; writeWord(regs[13], v); }
static uint32_t popStack(void) { uint32_t v = fetchWord(regs[13]); regs[13] += 4; return v; }

#if defined(EMU_NODE_HEADLESS)
/* hachage d'état (registres + SRAM, FNV-1a) : empreinte de fin du harnais */
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
#endif
/* ------------------------------------------------ cœur Thumb (ARMv6-M) ----
 * Interpréteur du Cortex-M0+ : sémantique ARMv6-M (ARM DDI 0419) pour les
 * deux consoles, dispatch par table de saut sur l'octet haut, coût en
 * cycles par instruction (ARM DDI 0484 ; modèle de m0_estimate.py : ALU 1,
 * load/store 2, branchement pris 2, BL 3, PUSH/POP/LDM/STM 1+N, POP {pc}
 * 3+N, MRS/MSR/barrières 4) plus les défauts du cache NVM (nvm_access).
 * Convention de PC : entre deux pas, regs[15] = prochaine instruction + 2 ;
 * pendant l'exécution, regs[15] = instruction + 4 (valeur lue par le code).
 */
static inline void setReg(int i, uint32_t v) { regs[i] = v; }
static inline void setNZ(uint32_t r) { fN = (int)(r >> 31); fZ = r == 0; }
static inline uint32_t addSetCond(uint32_t a, uint32_t b, int carry) {
    uint64_t r64 = (uint64_t)a + b + (unsigned)carry;
    uint32_t r = (uint32_t)r64;
    fC = (int)(r64 >> 32);
    fV = (int)((~(a ^ b) & (a ^ r)) >> 31);
    fN = (int)(r >> 31);
    fZ = r == 0;
    return r;
}

/* accès mémoire du cœur : plage SRAM testée en premier (les deux cibles),
 * puis la voie Pokitto (pk_read_/pk_write_ inlinés, second test de cible),
 * puis flash/segments META.  Avant, chaque accès Pokitto traversait ces
 * tests puis les fonctions de bus qui recommençaient : deux sauts
 * hors-ligne de plus par accès. */
static inline uint32_t ld32(uint32_t a) {
    uint32_t o = a - 0x20000000u;
    if (o <= SRAM_SIZE - 4u) { uint32_t v; memcpy(&v, sram + o, 4); return v; }
    if (emuTarget == TGT_POKITTO) return pk_read_word(a);
    if (a <= FLASH_SIZE - 4u) { uint32_t v; nvm_access(a); memcpy(&v, flash + a, 4); return v; }
    return fetchWord(a);
}
static inline uint32_t ld16(uint32_t a) {
    uint32_t o = a - 0x20000000u;
    if (o <= SRAM_SIZE - 2u) { uint16_t v; memcpy(&v, sram + o, 2); return v; }
    if (emuTarget == TGT_POKITTO) return pk_read_half(a);
    if (a <= FLASH_SIZE - 2u) { uint16_t v; nvm_access(a); memcpy(&v, flash + a, 2); return v; }
    return fetchHalf(a);
}
static inline uint32_t ld8(uint32_t a) {
    uint32_t o = a - 0x20000000u;
    if (o < SRAM_SIZE) return sram[o];
    if (emuTarget == TGT_POKITTO) return pk_read_byte(a);
    if (a < FLASH_SIZE) { nvm_access(a); return flash[a]; }
    return fetchByte(a);
}
static inline void st32(uint32_t a, uint32_t v) {
    if (emuTarget == TGT_POKITTO) { pk_write_word(a, v); return; }
    uint32_t o = a - 0x20000000u;
    if (o <= SRAM_SIZE - 4u) { memcpy(sram + o, &v, 4); return; }
    writeWord(a, v);
}
static inline void st16(uint32_t a, uint32_t v) {
    if (emuTarget == TGT_POKITTO) { pk_write_half(a, (uint16_t)v); return; }
    uint32_t o = a - 0x20000000u;
    if (o <= SRAM_SIZE - 2u) { uint16_t h = (uint16_t)v; memcpy(sram + o, &h, 2); return; }
    writeHalf(a, (uint16_t)v);
}
static inline void st8(uint32_t a, uint32_t v) {
    if (emuTarget == TGT_POKITTO) { pk_write_byte(a, (uint8_t)v); return; }
    uint32_t o = a - 0x20000000u;
    if (o < SRAM_SIZE) { sram[o] = (uint8_t)v; return; }
    writeByte(a, (uint8_t)v);
}


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

/* retour d'exception : PC sur EXC_RETURN (0xFFFFFFF1/9) & ~1 */
static void exc_return(void) {
    if (emuTarget == TGT_META) { nvic_return(); return; }
    regs[0] = popStack(); regs[1] = popStack(); regs[2] = popStack(); regs[3] = popStack();
    regs[12] = popStack(); regs[14] = popStack();
    uint32_t pc = popStack(), psr = popStack();
    regs[15] = (pc & ~1u) + 2u;
    fN = (int)(psr >> 31) & 1; fZ = (int)(psr >> 30) & 1; fC = (int)(psr >> 29) & 1; fV = (int)(psr >> 28) & 1;
    if (psr & (1u << 9)) regs[13] += 4u;
    armIrqEnable = 1;
}

/* PC hors de la mémoire exécutable : journal, puis reset (META) */
static int step_wild_pc(uint32_t pc) {
    if (emuTarget == TGT_POKITTO) {
        if (pc < 0x00040000u || pc >= 0x10000000u) return 0;
        static int n;
        if (n++ < 10) fprintf(stderr, "[pc fou pk] tick=%u pc=%x lr=%x sp=%x\n", tickCount, pc, regs[14], regs[13]);
        regs[15] = (pk_read_word(4) & ~1u) + 2u;
        return 1;
    }
    if (pc < FLASH_SIZE || pc - 0x20000000u < SRAM_SIZE) return 0;
    static int n;
    if (n < 3)
        fprintf(stderr, "[pc fou] tick=%u pc=%x prev=%x lr=%x sp=%x r0=%x r1=%x r2=%x r3=%x\n",
                tickCount, pc, lastExecPc, regs[14], regs[13], regs[0], regs[1], regs[2], regs[3]);
    if (n++ == 0) trace_tail_dump();
    advance(2); /* le temps continue (interruptions vivantes) */
    regs[15] = (fetchWord(vectorBase + 4) & ~1u) + 2u; /* reset */
    return 1;
}

static int dbgStep = -1; /* TRACE_TAIL ou EMU_TRACE : chemin de débogage */
static void step_debug(uint32_t pc, uint32_t op) {
    static long stepNo;
    if (traceTailOn < 0) trace_tail_init();
    if (traceTailOn > 0)
        trace_tail_push("%ld %u %x %04x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x", stepNo, tickCount,
                        pc, op, regs[13], regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6],
                        regs[7], regs[8], regs[9], regs[10], regs[11], regs[12], regs[14]);
    if (getenv("EMU_TRACE") && (tickCount & 0x3ffff) < 4)
        fprintf(stderr, "[tick %u] pc=0x%x inst=%04x r0=%08x sp=%08x\n", tickCount, pc, op, regs[0], regs[13]);
    stepNo++;
}

#define BRANCH(t) do { regs[15] = ((t) & ~1u) + 2u; cyc = 2; } while (0)

/* EMU_PROF=<fichier> : cycles cumulés par instruction (flash et SRAM, les
 * deux cibles), écrits en fin de run (« adresse cycles » en hexa/décimal),
 * un fichier « adresse cycles » agrégeable à la main ou par script. */
static uint32_t *profFlash, *profSram;
static int profOn = -1;
static void prof_add(uint32_t pc, uint32_t cyc) {
    if (pc < FLASH_SIZE) profFlash[pc >> 1] += cyc;
    else if (emuTarget == TGT_POKITTO) {
        uint32_t o = pc - 0x10000000u; /* SRAM principale LPC */
        if (o < SRAM_SIZE) profSram[o >> 1] += cyc;
    } else if (pc - 0x20000000u < SRAM_SIZE) profSram[(pc - 0x20000000u) >> 1] += cyc;
}
static void prof_write(void) {
    const char *path = getenv("EMU_PROF");
    if (!path || !profFlash) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (uint32_t i = 0; i < FLASH_SIZE / 2; i++) if (profFlash[i]) fprintf(f, "%x %u\n", i * 2, profFlash[i]);
    uint32_t sramBase = emuTarget == TGT_POKITTO ? 0x10000000u : 0x20000000u;
    for (uint32_t i = 0; i < SRAM_SIZE / 2; i++) if (profSram[i]) fprintf(f, "%x %u\n", sramBase + i * 2, profSram[i]);
    fclose(f);
}

static void step(void) {
    if (stepTicks) step_flush(0); /* ticks d'une injection hors pas */
    if (emuTarget == TGT_POKITTO) pk_machine_step();
    else if (irqWork) nvic_service();
    while (regs[15] >= 0xfffffff2u) exc_return(); /* EXC_RETURN atteint */

    uint32_t pc = regs[15] - 2u;
    uint32_t op;
    flashWaits = 0;
    if (pc < FLASH_SIZE) { /* fetch direct en flash (les deux cibles) */
        if (emuTarget == TGT_META) nvm_access(pc); /* META : cache NVM */
        op = (uint32_t)flash[pc] | (uint32_t)flash[pc + 1] << 8;
    } else {
        if (step_wild_pc(pc)) return;
        op = fetchHalf(pc);
    }
    if (dbgStep) {
        if (dbgStep < 0) dbgStep = getenv("TRACE_TAIL") || getenv("EMU_TRACE");
        if (dbgStep) step_debug(pc, op);
    }
    lastExecPc = pc;
    regs[15] = pc + 4u;
    uint32_t cyc = 1;

    switch (op >> 8) {
    /* ---- décalages immédiats, add/sub 3 opérandes */
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: { /* LSLS imm */
        uint32_t v = regs[(op >> 3) & 7], n = (op >> 6) & 31;
        if (n) { fC = (int)(v >> (32 - n)) & 1; v <<= n; }
        regs[op & 7] = v; setNZ(v); break; }
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: { /* LSRS imm */
        uint32_t v = regs[(op >> 3) & 7], n = (op >> 6) & 31;
        if (!n) n = 32;
        fC = (int)(v >> (n - 1)) & 1;
        v = n == 32 ? 0 : v >> n;
        regs[op & 7] = v; setNZ(v); break; }
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: { /* ASRS imm */
        int32_t v = (int32_t)regs[(op >> 3) & 7];
        uint32_t n = (op >> 6) & 31;
        if (!n) n = 32;
        fC = (int)((uint32_t)(v >> (n - 1)) & 1);
        uint32_t r = (uint32_t)(n == 32 ? v >> 31 : v >> n);
        regs[op & 7] = r; setNZ(r); break; }
    case 0x18: case 0x19: regs[op & 7] = addSetCond(regs[(op >> 3) & 7], regs[(op >> 6) & 7], 0); break;  /* ADDS reg */
    case 0x1a: case 0x1b: regs[op & 7] = addSetCond(regs[(op >> 3) & 7], ~regs[(op >> 6) & 7], 1); break; /* SUBS reg */
    case 0x1c: case 0x1d: regs[op & 7] = addSetCond(regs[(op >> 3) & 7], (op >> 6) & 7, 0); break;        /* ADDS imm3 */
    case 0x1e: case 0x1f: regs[op & 7] = addSetCond(regs[(op >> 3) & 7], ~((op >> 6) & 7), 1); break;     /* SUBS imm3 */
    /* ---- immédiats 8 bits */
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:            /* MOVS */
        regs[(op >> 8) & 7] = op & 0xff; fN = 0; fZ = (op & 0xff) == 0; break;
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:            /* CMP */
        addSetCond(regs[(op >> 8) & 7], ~(op & 0xff), 1); break;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:            /* ADDS */
        regs[(op >> 8) & 7] = addSetCond(regs[(op >> 8) & 7], op & 0xff, 0); break;
    case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:            /* SUBS */
        regs[(op >> 8) & 7] = addSetCond(regs[(op >> 8) & 7], ~(op & 0xff), 1); break;
    /* ---- ALU registre */
    case 0x40: case 0x41: case 0x42: case 0x43: {
        uint32_t rd = op & 7, a = regs[rd], b = regs[(op >> 3) & 7], r, sh;
        switch ((op >> 6) & 15) {
        case 0x0: r = a & b; break;                                   /* ANDS */
        case 0x1: r = a ^ b; break;                                   /* EORS */
        case 0x2: sh = b & 0xff; r = a;                               /* LSLS reg */
            if (sh) { if (sh < 32) { fC = (int)(a >> (32 - sh)) & 1; r = a << sh; }
                      else { fC = sh == 32 ? (int)(a & 1) : 0; r = 0; } }
            break;
        case 0x3: sh = b & 0xff; r = a;                               /* LSRS reg */
            if (sh) { if (sh < 32) { fC = (int)(a >> (sh - 1)) & 1; r = a >> sh; }
                      else { fC = sh == 32 ? (int)(a >> 31) : 0; r = 0; } }
            break;
        case 0x4: sh = b & 0xff; r = a;                               /* ASRS reg */
            if (sh) { if (sh < 32) { fC = (int)((uint32_t)((int32_t)a >> (sh - 1)) & 1); r = (uint32_t)((int32_t)a >> sh); }
                      else { fC = (int)(a >> 31); r = (uint32_t)((int32_t)a >> 31); } }
            break;
        case 0x5: regs[rd] = addSetCond(a, b, fC); goto alu_done;     /* ADCS */
        case 0x6: regs[rd] = addSetCond(a, ~b, fC); goto alu_done;    /* SBCS */
        case 0x7: sh = b & 0xff; r = a;                               /* RORS */
            if (sh) { sh &= 31; if (sh) r = (a >> sh) | (a << (32 - sh)); fC = (int)(r >> 31); }
            break;
        case 0x8: setNZ(a & b); goto alu_done;                        /* TST */
        case 0x9: regs[rd] = addSetCond(0, ~b, 1); goto alu_done;     /* RSBS #0 */
        case 0xa: addSetCond(a, ~b, 1); goto alu_done;                /* CMP */
        case 0xb: addSetCond(a, b, 0); goto alu_done;                 /* CMN */
        case 0xc: r = a | b; break;                                   /* ORRS */
        case 0xd: r = a * b; break;                                   /* MULS (multiplieur 1 cycle) */
        case 0xe: r = a & ~b; break;                                  /* BICS */
        default:  r = ~b; break;                                      /* MVNS */
        }
        regs[rd] = r; setNZ(r);
    alu_done:
        break; }
    /* ---- registres hauts, BX/BLX (aucun drapeau, sauf CMP) */
    case 0x44: { /* ADD Rdn, Rm */
        uint32_t rdn = (op & 7) | ((op >> 4) & 8), v = regs[rdn] + regs[(op >> 3) & 15];
        if (rdn == 15) BRANCH(v); else regs[rdn] = v;
        break; }
    case 0x45: addSetCond(regs[(op & 7) | ((op >> 4) & 8)], ~regs[(op >> 3) & 15], 1); break; /* CMP hi */
    case 0x46: { /* MOV Rd, Rm */
        uint32_t rd = (op & 7) | ((op >> 4) & 8), v = regs[(op >> 3) & 15];
        if (rd == 15) BRANCH(v); else regs[rd] = v;
        break; }
    case 0x47: { /* BX / BLX Rm */
        uint32_t t = regs[(op >> 3) & 15];
        if (op & 0x80) {
            if (emuTarget == TGT_POKITTO) { pk_blx(op); stepTicks = 0; cyc = 2; break; } /* API ROM interceptée */
            regs[14] = (pc + 2u) | 1u;
        }
        BRANCH(t);
        break; }
    /* ---- LDR littéral */
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        regs[(op >> 8) & 7] = ld32(((pc + 4u) & ~3u) + ((op & 0xff) << 2)); cyc = 2; break;
    /* ---- load/store registre + registre */
    case 0x50: case 0x51: st32(regs[(op >> 3) & 7] + regs[(op >> 6) & 7], regs[op & 7]); cyc = 2; break;
    case 0x52: case 0x53: st16(regs[(op >> 3) & 7] + regs[(op >> 6) & 7], regs[op & 7]); cyc = 2; break;
    case 0x54: case 0x55: st8(regs[(op >> 3) & 7] + regs[(op >> 6) & 7], regs[op & 7]); cyc = 2; break;
    case 0x56: case 0x57: regs[op & 7] = (uint32_t)(int32_t)(int8_t)ld8(regs[(op >> 3) & 7] + regs[(op >> 6) & 7]); cyc = 2; break;
    case 0x58: case 0x59: regs[op & 7] = ld32(regs[(op >> 3) & 7] + regs[(op >> 6) & 7]); cyc = 2; break;
    case 0x5a: case 0x5b: regs[op & 7] = ld16(regs[(op >> 3) & 7] + regs[(op >> 6) & 7]); cyc = 2; break;
    case 0x5c: case 0x5d: regs[op & 7] = ld8(regs[(op >> 3) & 7] + regs[(op >> 6) & 7]); cyc = 2; break;
    case 0x5e: case 0x5f: regs[op & 7] = (uint32_t)(int32_t)(int16_t)ld16(regs[(op >> 3) & 7] + regs[(op >> 6) & 7]); cyc = 2; break;
    /* ---- load/store immédiat */
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
        st32(regs[(op >> 3) & 7] + ((op >> 4) & 0x7c), regs[op & 7]); cyc = 2; break;
    case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e: case 0x6f:
        regs[op & 7] = ld32(regs[(op >> 3) & 7] + ((op >> 4) & 0x7c)); cyc = 2; break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        st8(regs[(op >> 3) & 7] + ((op >> 6) & 31), regs[op & 7]); cyc = 2; break;
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
        regs[op & 7] = ld8(regs[(op >> 3) & 7] + ((op >> 6) & 31)); cyc = 2; break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
        st16(regs[(op >> 3) & 7] + ((op >> 5) & 0x3e), regs[op & 7]); cyc = 2; break;
    case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f:
        regs[op & 7] = ld16(regs[(op >> 3) & 7] + ((op >> 5) & 0x3e)); cyc = 2; break;
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        st32(regs[13] + ((op & 0xff) << 2), regs[(op >> 8) & 7]); cyc = 2; break;
    case 0x98: case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f:
        regs[(op >> 8) & 7] = ld32(regs[13] + ((op & 0xff) << 2)); cyc = 2; break;
    /* ---- ADR, ADD Rd, SP, #imm */
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
        regs[(op >> 8) & 7] = ((pc + 4u) & ~3u) + ((op & 0xff) << 2); break;
    case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
        regs[(op >> 8) & 7] = regs[13] + ((op & 0xff) << 2); break;
    /* ---- divers */
    case 0xb0: { uint32_t v = (op & 0x7f) << 2; regs[13] += (op & 0x80) ? -v : v; break; } /* ADD/SUB SP */
    case 0xb2: { uint32_t v = regs[(op >> 3) & 7];                                         /* extensions */
        switch ((op >> 6) & 3) {
        case 0: v = (uint32_t)(int32_t)(int16_t)v; break;  /* SXTH */
        case 1: v = (uint32_t)(int32_t)(int8_t)v; break;   /* SXTB */
        case 2: v &= 0xffff; break;                        /* UXTH */
        default: v &= 0xff; break;                         /* UXTB */
        }
        regs[op & 7] = v; break; }
    case 0xb4: case 0xb5: { /* PUSH {rlist[, lr]} */
        uint32_t n = (uint32_t)__builtin_popcount(op & 0x1ff), a = regs[13] - 4u * n;
        regs[13] = a;
        for (int i = 0; i < 8; i++) if (op & (1u << i)) { st32(a, regs[i]); a += 4; }
        if (op & 0x100) st32(a, regs[14]);
        cyc = 1 + n; break; }
    case 0xbc: case 0xbd: { /* POP {rlist[, pc]} */
        uint32_t n = (uint32_t)__builtin_popcount(op & 0x1ff), a = regs[13];
        for (int i = 0; i < 8; i++) if (op & (1u << i)) { regs[i] = ld32(a); a += 4; }
        if (op & 0x100) { uint32_t t = ld32(a); a += 4; regs[13] = a; BRANCH(t); cyc = 3 + n; }
        else { regs[13] = a; cyc = 1 + n; }
        break; }
    case 0xb6: /* CPSID i (B672) / CPSIE i (B662) */
        if ((op & 0xffef) == 0xb662) {
            int dis = (op >> 4) & 1;
            if (emuTarget == TGT_POKITTO) armIrqEnable = !dis;
            else { primask = dis; if (!dis) irqWork = 1; }
        }
        break;
    case 0xba: { uint32_t v = regs[(op >> 3) & 7];                                         /* REV* */
        switch ((op >> 6) & 3) {
        case 0: v = __builtin_bswap32(v); break;                                               /* REV */
        case 1: v = ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8); break;               /* REV16 */
        case 3: v = (uint32_t)(int32_t)(int16_t)(uint16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff)); break; /* REVSH */
        default: break;
        }
        regs[op & 7] = v; break; }
    case 0xbe: case 0xbf: break; /* BKPT, NOP/YIELD/WFE/WFI/SEV */
    /* ---- LDMIA / STMIA */
    case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5: case 0xc6: case 0xc7: {
        uint32_t rn = (op >> 8) & 7, a = regs[rn], n = 0;
        for (int i = 0; i < 8; i++) if (op & (1u << i)) { st32(a, regs[i]); a += 4; n++; }
        regs[rn] = a; cyc = 1 + n; break; }
    case 0xc8: case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: {
        uint32_t rn = (op >> 8) & 7, a = regs[rn], n = 0;
        for (int i = 0; i < 8; i++) if (op & (1u << i)) { regs[i] = ld32(a); a += 4; n++; }
        if (!(op & (1u << rn))) regs[rn] = a; /* pas d'écriture de base si Rn est chargé */
        cyc = 1 + n; break; }
    /* ---- branchements */
    case 0xd0: if (fZ) goto bcond; break;
    case 0xd1: if (!fZ) goto bcond; break;
    case 0xd2: if (fC) goto bcond; break;
    case 0xd3: if (!fC) goto bcond; break;
    case 0xd4: if (fN) goto bcond; break;
    case 0xd5: if (!fN) goto bcond; break;
    case 0xd6: if (fV) goto bcond; break;
    case 0xd7: if (!fV) goto bcond; break;
    case 0xd8: if (fC && !fZ) goto bcond; break;
    case 0xd9: if (!fC || fZ) goto bcond; break;
    case 0xda: if (fN == fV) goto bcond; break;
    case 0xdb: if (fN != fV) goto bcond; break;
    case 0xdc: if (!fZ && fN == fV) goto bcond; break;
    case 0xdd: if (fZ || fN != fV) goto bcond; break;
    bcond: BRANCH(pc + 4u + (uint32_t)((int32_t)(int8_t)(op & 0xff) * 2)); break;
    case 0xde: case 0xdf: break; /* UDF, SVC : sans effet */
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6: case 0xe7: /* B */
        BRANCH(pc + 4u + (uint32_t)(((int32_t)(op << 21)) >> 20)); break;
    /* ---- 32 bits : BL, MRS, MSR, barrières ; le reste est consommé */
    case 0xf0: case 0xf1: case 0xf2: case 0xf3: case 0xf4: case 0xf5: case 0xf6: case 0xf7: {
        uint32_t op2 = (pc + 2u < FLASH_SIZE)
                     ? (uint32_t)flash[pc + 2] | (uint32_t)flash[pc + 3] << 8 : fetchHalf(pc + 2u);
        if ((op2 & 0xd000) == 0xd000) { /* BL : S:I1:I2:imm10:imm11:0 */
            uint32_t s = (op >> 10) & 1, i1 = !(((op2 >> 13) & 1) ^ s), i2 = !(((op2 >> 11) & 1) ^ s);
            uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((op & 0x3ff) << 12) | ((op2 & 0x7ff) << 1);
            int32_t off = (int32_t)(imm << 7) >> 7;
            regs[14] = (pc + 4u) | 1u;
            BRANCH(pc + 4u + (uint32_t)off);
            cyc = 3;
            break;
        }
        cyc = 4;
        regs[15] = pc + 6u; /* paire consommée */
        if (op == 0xf3ef && (op2 & 0xf000) == 0x8000) { /* MRS Rd, spec */
            uint32_t sysm = op2 & 0xff, v = 0;
            uint32_t apsr = (uint32_t)fN << 31 | (uint32_t)fZ << 30 | (uint32_t)fC << 29 | (uint32_t)fV << 28;
            uint32_t ipsr = emuTarget == TGT_META ? (uint32_t)exc_active() : 0;
            if (sysm <= 3) v = apsr | ((sysm & 1) ? ipsr : 0);       /* APSR, IAPSR, EAPSR, xPSR */
            else if (sysm >= 5 && sysm <= 7) v = (sysm & 1) ? ipsr | (sysm == 7 ? apsr : 0) : 0;
            else if (sysm == 8 || sysm == 9) v = regs[13];          /* MSP (PSP non modélisé) */
            else if (sysm == 16) v = emuTarget == TGT_META ? (uint32_t)primask : (uint32_t)!armIrqEnable;
            regs[(op2 >> 8) & 15] = v;
        } else if ((op & 0xfff0) == 0xf380 && (op2 & 0xff00) == 0x8800) { /* MSR spec, Rn */
            uint32_t sysm = op2 & 0xff, v = regs[op & 15];
            if (sysm <= 3) { fN = (int)(v >> 31); fZ = (int)(v >> 30) & 1; fC = (int)(v >> 29) & 1; fV = (int)(v >> 28) & 1; }
            else if (sysm == 8 || sysm == 9) regs[13] = v & ~3u;
            else if (sysm == 16) {
                if (emuTarget == TGT_POKITTO) armIrqEnable = !(v & 1);
                else { primask = (int)(v & 1); irqWork = 1; }
            }
        }
        break; }
    case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xec: case 0xed: case 0xee: case 0xef:
    case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xfe: case 0xff:
        regs[15] = pc + 6u; break; /* 32 bits non ARMv6-M : paire consommée */
    default: break; /* 0xb1, 0xb3, 0xb7-0xb9, 0xbb : non alloués */
    }
    if (profOn) {
        if (profOn < 0) {
            profOn = getenv("EMU_PROF") != NULL;
            if (profOn) { profFlash = calloc(FLASH_SIZE / 2, 4); profSram = calloc(SRAM_SIZE / 2, 4); }
        }
        if (profOn) prof_add(pc, cyc + flashWaits);
    }
    step_flush(cyc + flashWaits);
}
#undef BRANCH

/* --------------------------------------------------------- SDL + main */

static SDL_Texture *tex;
static uint32_t px32[3 * MAX_SCREEN_W * 3 * MAX_SCREEN_H];

/* Mise à l'échelle de la fenêtre : 0 = échelle entière (pixels carrés,
 * défaut), 1 = adaptée (au plus grand multiple non entier qui garde le
 * ratio, interpolation linéaire), 2 = étirée (remplit la fenêtre).  F10
 * cycle les modes, F11 bascule le plein écran. */
static int dispScale, dispFull;

/* F8 : filtre Game Boy DMG — la luminance de chaque pixel est quantifiée
 * sur les 4 nuances de la dalle réflexe, puis chaque pixel émulé devient
 * une cellule 3x3 à bord sombre (la maille point-matrice de la photo).
 * La texture est alors 3x plus grande ; la taille logique de rendu ne
 * change pas, le tracé et les modes d'échelle restent identiques. */
static int dispFilter;
static const uint32_t dmgShade[4] = { /* ARGB, de la plus sombre à la plus claire */
    0xff0f380fu, 0xff306230u, 0xff8bac0fu, 0xff9bbc0fu };
#define DMG_SHADE(c, num) (0xff000000u | \
    ((((c) >> 16) & 0xff) * (num) / 100) << 16 | \
    ((((c) >> 8) & 0xff) * (num) / 100) << 8 | \
    (((c) & 0xff) * (num) / 100))

static void scale_apply(void);

static void blit(SDL_Renderer *ren) {
    const uint16_t *src = emuTarget == TGT_POKITTO ? pk_lcd : pix;
    if (!dispFilter) {
        for (unsigned i = 0; i < SCR_W * SCR_H; i++) {
            uint16_t p = src[i];
            uint8_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
            /* même expansion que st7735.ts : décalages, pas de mise à l'échelle */
            px32[i] = 0xff000000u | ((b << 3) << 16) | ((g << 2) << 8) | (r << 3);
        }
        SDL_UpdateTexture(tex, NULL, px32, SCR_W * sizeof(uint32_t));
    } else {
        /* quantifie la luminance sur les 4 nuances DMG, puis trame 3x3 :
         * centre de cellule à pleine teinte, bords assombris (la maille
         * point-matrice de la dalle) */
        for (unsigned ty = 0; ty < 3 * SCR_H; ty++) {
            uint32_t *out = px32 + ty * 3 * SCR_W;
            const uint16_t *line = src + (ty / 3) * SCR_W;
            unsigned j = ty % 3;
            for (unsigned x = 0; x < SCR_W; x++) {
                uint16_t p = line[x];
                uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
                /* luminance 8 bits depuis du 5/6/5 : sommes pré-multipliées */
                uint32_t lum = (r * 616 + g * 604 + b * 224) >> 8; /* 0..250 */
                uint32_t c = dmgShade[lum >= 176 ? 3 : lum >= 120 ? 2 : lum >= 64 ? 1 : 0];
                uint32_t e = DMG_SHADE(c, 52), m = DMG_SHADE(c, 74);
                out[0] = j == 1 ? m : e;
                out[1] = j == 1 ? c : m;
                out[2] = j == 1 ? m : e;
                out += 3;
            }
        }
        SDL_UpdateTexture(tex, NULL, px32, SCR_W * 3 * sizeof(uint32_t));
    }
    SDL_RenderClear(ren);
    if (dispScale) {
        int ow, oh;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_Rect dst = {0, 0, ow, oh};
        if (dispScale == 1) { /* adaptée : ratio de l'écran console conservé */
            double sw = (double)ow / SCR_W, sh = (double)oh / SCR_H;
            double s = sw < sh ? sw : sh;
            dst.w = (int)(SCR_W * s + 0.5);
            dst.h = (int)(SCR_H * s + 0.5);
            dst.x = (ow - dst.w) / 2;
            dst.y = (oh - dst.h) / 2;
        }
        SDL_RenderCopy(ren, tex, NULL, &dst);
    } else {
        SDL_RenderCopy(ren, tex, NULL, NULL);
    }
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
static unsigned long pk_cbCalls, pk_cbSamples;
static Uint32 pk_cbFirstMs;
static void pk_audio_cb(Uint8 *stream, int len) {
    /* le device est S16SYS : len octets = len/2 échantillons.  L'ancien
     * traitement U8 consommait deux fois trop de contenu par callback
     * (son 2x trop rapide, mesuré 44160 « échantillons »/s) et écrivait
     * des octets pleins échelle dans des int16 (distorsion générale). */
    int16_t *out = (int16_t *)stream;
    int n = len / 2;
    if (pk_cbFirstMs == 0) pk_cbFirstMs = SDL_GetTicks();
    pk_cbCalls++; pk_cbSamples += (uint32_t)n;
    if (ENVFLAG("EMU_DMA_TRACE") && (pk_cbCalls % 100 == 1))
        fprintf(stderr, "[cb] calls=%lu samples=%lu (%.1f/s mural) n=%d aqSize=%u\n",
                pk_cbCalls, pk_cbSamples,
                (double)pk_cbSamples * 1000.0 / (SDL_GetTicks() - pk_cbFirstMs + 1),
                n, pk_aqSize);
    /* Régulation du remplissage, comme le chemin META : la cible est ~55 ms
     * de latches ; au-delà, la consommation accélère d'autant (la latence
     * retombe en une fraction de seconde — la file pokitto plafonnait à
     * ~7000 latches, 0,32 s de retard constant, et montait sans retour
     * sous les rafales de production) ; en deçà, elle ralentit d'au plus
     * 0,4 % pour la faire remonter. */
    /* device à taux fixe : la base de consommation = taux DAC du jeu /
     * taux device (PK_IFREQ compte des latchs à 22050) ; la régulation
     * corrige le remplissage autour de la cible (~55 ms de latchs) */
    float base = (float)(emuDacRate / (double)devRate);
    float step = base;
    {
        float over = (float)pk_aqSize - 1200.0f;
        if (over > 0.0f) {
            step = base * (1.0f + over / 22050.0f);
            if (step > base * 1.5f) step = base * 1.5f;
        } else {
            float under = -over / 22050.0f;
            if (under > 0.004f) under = 0.004f;
            step = base * (1.0f - under);
        }
    }
    float err = 0;
    for (int i = 0; i < n; i++) {
        int16_t s;
        if (pk_aqSize) {
            while (pk_aqSize) {
                float d = pk_aqDelta[pk_aqStart] + err - PK_IFREQ * step;
                if (d > 0) { pk_aqDelta[pk_aqStart] = d; err = 0; break; }
                pk_audioHoldF = pk_aqData[pk_aqStart];
                err = -d;
                pk_aqStart = (pk_aqStart + 1) & PK_AQ_MASK;
                pk_aqSize--;
            }
        }
        /* à sec (pause, menu, stall) : la dernière valeur se tient, comme
         * le DAC matériel — l'ancien « saute au présent » rejouait 600
         * latches déjà consommés à chaque reprise */
        int raw = (int16_t)(((uint8_t)pk_audioHoldF ^ 0x80) << 8);
        /* couplage AC de l'ampli Pokitto, comme sur la META (d936f63) :
         * le loader tient un repos DC arbitraire (63) — sans blocage, ce
         * rail mange la moitié de la dynamique et claque à chaque
         * transition (le « son baisé » au loader) */
        static float pk_dcX, pk_dcY; static int pk_dcInit;
        if (!pk_dcInit) { pk_dcX = (float)raw; pk_dcInit = 1; }
        pk_dcY = 0.995f * pk_dcY + (float)raw - pk_dcX;
        pk_dcX = (float)raw;
        s = (int16_t)(pk_dcY > 32767.f ? 32767.f : (pk_dcY < -32768.f ? -32768.f : pk_dcY));
        out[i] = s;
        if (wavFile) wav_put(s);
    }
}

/* audio SDL : l'ISR écrit dans aq ; le callbackSDL consomme */
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    if (emuTarget == TGT_POKITTO) { pk_audio_cb(stream, len); return; }
    int16_t *out = (int16_t *)stream;
    /* régulation dynamique du débit : la lecture avance de `ratio`
     * échantillons de périphérique par sortie (interpolation linéaire).
     *  Le périphérique est ouvert UNE fois à taux fixe (48 kHz) : jamais de
     * renégociation système quand le jeu change sa cadence DAC.  Le ratio
     * de base = taux DAC du jeu / taux device, corrigé de ±0,5 % selon
     * l'écart du remplissage à la cible (dérive d'horloges). */
    static double frac;
    int ahead = aq_tail - aq_head;
    if (ahead < 0) ahead += AQ_SIZE;
    double target = AQ_LATENCY * 0.6;
    double err = (ahead - target) / target;
    if (err > 1.0) err = 1.0; else if (err < -1.0) err = -1.0;
    double ratio = (emuDacRate / (double)devRate) * (1.0 + 0.005 * err);
    if (ratio < 0.2) ratio = 0.2; else if (ratio > 5.0) ratio = 5.0;
    for (int i = 0; i < len / 2; i++) {
        if (audioPending) { out[i] = 0; continue; } /* pré-buffer : silence */
        int h = aq_head, t = aq_tail;
        int avail = t - h; if (avail < 0) avail += AQ_SIZE;
        if (avail >= 2) {
            int16_t a = aq[h], b = aq[(h + 1) % AQ_SIZE];
            audioHold = (int16_t)(a + (b - a) * frac);
            frac += ratio;
            while (frac >= 1.0 && avail > 1) { h = (h + 1) % AQ_SIZE; frac -= 1.0; avail--; }
            if (frac >= 1.0) frac = 0.999;
            aq_head = h;
        } else {
            audioHold /= 2; /* sous-débit : relâche vers le silence */
            audUnder++;
        }
        out[i] = audioHold;
    }
    audCbSamples += (uint32_t)(len / 2); audCbCalls++;
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
            /* comme sur META : Espace=A, Ctrl=B, Entrée=C (menu) */
            case SDLK_SPACE: return BTN_A;
            case SDLK_LCTRL: case SDLK_RCTRL: return BTN_B;
            case SDLK_RETURN: case SDLK_KP_ENTER: return BTN_MENU;
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
    /* buttonData reste toujours en ordre lib ; la conversion (pad lu à
     * 24 MHz) se fait à la réponse, cf. sercom4_write */
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
            /* croix directionnelle, comme sur META (les boutons du pad
             * pokitto sont des GPIO, via pk_btn_gpio) */
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return BTN_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return BTN_LEFT;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return BTN_RIGHT;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return BTN_UP;
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

static int sd_explicit; /* carte passée explicitement en ligne de commande */
static int fwNeedsBoot; /* un zip a chargé un firmware : vecteurs à remettre */

static SDL_Window *emuWin;
static SDL_Renderer *emuRen;
static uint8_t padDirBits; /* directions tenues par stick/chapeau */

static void scale_apply(void) {
    if (!emuRen) return;
    /* La qualité de filtre se lit à la création de la texture : on la
     * recrée (le contenu est de toute façon réécrit à chaque frame). */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, dispScale ? "linear" : "nearest");
    if (tex) SDL_DestroyTexture(tex);
    unsigned tw = dispFilter ? 3 * SCR_W : SCR_W, th = dispFilter ? 3 * SCR_H : SCR_H;
    tex = SDL_CreateTexture(emuRen, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, (int)tw, (int)th);
    if (dispScale) {
        SDL_RenderSetLogicalSize(emuRen, 0, 0);
        SDL_RenderSetIntegerScale(emuRen, SDL_FALSE);
    } else {
        SDL_RenderSetLogicalSize(emuRen, SCR_W, SCR_H);
        SDL_RenderSetIntegerScale(emuRen, SDL_TRUE);
    }
}

static Uint32 titleMs;
static uint32_t titleTick;
static double rawEmuMs, rawWallMs; /* temps émulé produit / temps mural passé à émuler */
static double rawEmuMsTotal, rawWallMsTotal; /* cumuls sur tout le run (ligne [bench] de sortie) */
static char titleBuf[1200]; /* dernier titre construit (HUD wasm) */
static char hudTitle[1240]; /* titre de la fenêtre native : jeu + % */
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
    /* les DEUX machines sont remises : la cible du prochain firmware n'est
     * lue qu'APRÈS ce reset (load_firmware_data examine le mot 0) — en
     * enchaînant Pokitto -> META, seul l'état LPC était remis et tout
     * l'état SAMD21 de la partie précédente restait (NVIC, TC4/TC5, DMAC,
     * CS SPI, fenêtre LCD) : le jeu suivant tournait à 100 % écran noir.
     * Les banques partagées (sram, regs, anneau audio) finissent dans
     * l'état posé ici ; pk_reset_core relit ses vecteurs après le
     * chargement de la flash quand la cible détectée est Pokitto. */
    pk_reset_core();
    stepTicks = 0; /* incrementPc de pk_reset_core : aucun tick résiduel */
    memset(sram, 0xff, SRAM_SIZE);
    memset(regs, 0, sizeof regs);
    fN = fZ = fC = fV = 0;
    primask = 0;
    nvic_reset();
    tickCount = 0; sysTickTrigger = 0; sysTickEntries = 0;
    sysTickVector = dmacVector = tc4Vector = tc5Vector = 0;
    dmacInterrupt = tc4Interrupt = 0;
    dmac_baseAddr = dmac_wrbAddr = dmac_desc = dmac_chid = 0;
    for (uint32_t k = 0; k < DMAC_CHANNELS; k++) {
        dmaResumeAt[k] = 0;
        dmaSkipSuspend[k] = 0;
    }
    memset(dmaTrig, 0, sizeof dmaTrig); memset(dmaOn, 0, sizeof dmaOn);
    /* PA27 (CS carte SD) et PA25 (CS boutons) hauts : lignes désélectionnées
     * (pull-ups réelles), les périphériques n'écoutent le bus SPI que si le
     * firmware les sélectionne.  Les jeux sans lib standard (pas de pinMode
     * early) restent sinon « sélectionnés » en permanence et le trafic SPI
     * écran part dans la machine SD. */
    portA_out = (1u << 27) | (1u << 25); /* PA27 (SD) et PA25 (boutons) hauts */
    portB_out = portA_dir = portB_dir = 0;
    ser4_data = 0x80;
    spiRxN = 0; spiLastDone = spiPrevDone = 0;
    tc4Enabled = tc4Armed = 0;
    tc4IntEnMask = tc4IntFlagMask = 0;
    tc4CtrlA = 0;
    tc5Enabled = tc5Armed = 0;
    tc5IntEnMask = tc5IntFlagMask = 0;
    tc5CtrlA = tc5Top = tc5Counter = 0;
    tc5Fires = 0; tc5Interrupt = 0;
    spiDmaCh = -1; spiBeatAcc = 0; spiBeatTicks = 20; spiBaud = 0;
    memset(dmacIntFlag, 0, sizeof dmacIntFlag);
    tc4Top = tc4Counter = 0; tc4Period = 907;
    timerAcc = 0; timerBudget = 0;
    tc4Fires = tc4Writes = 0;
    lcd_xStart = lcd_xEnd = lcd_yStart = lcd_yEnd = lcd_x = lcd_y = 0;
    lcd_argIndex = lcd_lastCommand = lcd_tmp = 0;
    lcdBgrSwapped = 0;
    lcdColmod = 5;
    memset(pix, 0, sizeof pix);
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
        if (n == 0) { fprintf(stderr, TR("firmware vide\n", "empty firmware\n")); return; }
        memcpy(flash, payload, n);
        snprintf(fwPath, sizeof(fwPath), "%s", display);
        const char *b = strrchr(fwPath, '/');
        fwName = b ? b + 1 : fwPath;
        fwLoaded = 1;
        pk_reset_core(); /* vecteurs lisibles ici */
        pk_eeprom_load();
        printf(TR("firmware Pokitto : %s (%zu Ko)\n", "Pokitto firmware: %s (%zu KB)\n"), display, len / 1024);
        return;
    }
    memset(flash, 0xff, FLASH_SIZE); /* comme le TS : flash remplie de 0xff */
    size_t n = plen < (FLASH_SIZE - 0x4000) ? plen : (FLASH_SIZE - 0x4000);
    if (n == 0) { fprintf(stderr, TR("firmware vide\n", "empty firmware\n")); return; }
    memcpy(flash + 0x4000, payload, n);
    snprintf(fwPath, sizeof(fwPath), "%s", display);
    const char *b = strrchr(fwPath, '/');
    fwName = b ? b + 1 : fwPath;
    fwLoaded = 1;
    printf(TR("firmware : %s (%zu Ko)\n", "firmware: %s (%zu KB)\n"), display, len / 1024);
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
    if (!f) { fprintf(stderr, TR("firmware introuvable : %s\n", "firmware not found: %s\n"), p); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)sz);
    if (fread(data, 1, (size_t)sz, f) == 0) {
        fprintf(stderr, TR("firmware vide\n", "empty firmware\n")); free(data); fclose(f); return;
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

static void load_sd_from_path(const char *p) {
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) { sd_unload(); fat_build_from_dir(p); return; }
    FILE *g = fopen(p, "rb");
    if (!g) { fprintf(stderr, TR("carte introuvable : %s\n", "card not found: %s\n"), p); return; }
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
    printf(TR("carte SD : image %s (%ld Kio)\n", "SD card: image %s (%ld KiB)\n"), p, sz / 1024);
}

static void boot_vectors(void) {
    if (emuTarget == TGT_POKITTO) {
        /* vecteurs déjà chargés par pk_reset_core */
        fprintf(stderr, "Pokitto: SP=%08x PC=%08x\n", regs[13], regs[15]);
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
    tc5Vector = fetchWord(vectorBase + 0x90) & ~1u;
    fprintf(stderr, "SP=%08x PC=%08x systick=%08x dmac=%08x tc4=%08x tc5=%08x\n",
            regs[13], regs[15], sysTickVector, dmacVector, tc4Vector, tc5Vector);
}

static void refresh_title(void) {
    char title[1200];
    if (fwLoaded) snprintf(title, sizeof(title), "%.900s", fwName);
    else snprintf(title, sizeof(title), TR("déposez un firmware .bin", "drop a .bin firmware"));
    SDL_SetWindowTitle(emuWin, title);
}

static void audio_open_standard(void); /* défini avec le bloc SDL */

static void audio_start(void) {
    /* l'audio HLE Pokitto (pk_audio_reopen) peut avoir remplacé le
     * périphérique standard lors d'une partie précédente : un jeu suivant
     * sur l'autre cible doit retrouver le canal S16 (sinon la file aq
     * n'est plus consommée — silence ou gargouillis) */
    if (audioOk && audioDev && !audioDevStandard) audio_open_standard();
    audioPending = 1; /* gate fermé : audio_resume_when_ready l'ouvrira */
}

/* ------------------------------------------------ SDL/HTML5 ------------ */

/* dimensions SDL selon la cible (appelé aussi au changement de cible) */
static void pk_screen_reconfig(void) {
    SCR_W = emuTarget == TGT_POKITTO ? 220 : 160;
    SCR_H = emuTarget == TGT_POKITTO ? 176 : 128;
    if (!emuRen) return;
    SDL_SetWindowSize(emuWin, (int)(SCR_W * 2), (int)(SCR_H * 2));
    scale_apply();
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
    audioDevStandard = 0; /* pk_hle_audio_cb : plus la file aq standard */
    SDL_PauseAudioDevice(dev, 0);
}

/* périphérique audio standard : taux fixe 48 kHz, audio_cb lit la file aq.
 * Une seule définition : sdl_init_all l'ouvre au boot, pk_r2r_reopen y
 * revient quand le HLE se retire, audio_start la rétablit au boot d'un jeu
 * si une partie Pokitto HLE l'avait remplacée. */
static void audio_open_standard(void) {
    /* PAS de garde audioOk : au premier appel (sdl_init_all) le device
     * n'existe pas encore — une garde ici le faisait renvoyer immédiatement
     * et l'émulateur démarrait sans aucun son.  L'échec d'ouverture laisse
     * simplement audioOk à 0 (audio_start retentera au boot d'un jeu). */
    SDL_AudioSpec want, got;
    memset(&want, 0, sizeof(want));
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 1;
    want.samples = 512; want.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!dev) return;
    if (audioDev) { SDL_PauseAudioDevice(audioDev, 1); SDL_CloseAudioDevice(audioDev); }
    audioDev = dev;
    audioOk = 1;
    devRate = got.freq ? got.freq : 48000;
    devCbSamples = got.samples;
    aq_configure(got.samples, (double)devRate, emuDacRate);
    audioDevStandard = 1;
    SDL_PauseAudioDevice(dev, 0);
}

static void pk_r2r_reopen(void) {
    audio_open_standard(); /* retour du HLE au canal standard (S16, file aq) */
}

/* taux DAC réel du jeu (config TC4/TC5) : le périphérique hôte reste
 * ouvert à taux fixe, seule la consommation s'adapte — plus aucune
 * fermeture/réouverture qui renégocierait le son de toute la machine.
 *  Plage raisonnable seulement : hors plage, on garde l'ancien taux. */
static void meta_audio_set_rate(double freq) {
    if (!audioOk || freq <= 0.0) return;
    if (freq < 8000.0 || freq > 96000.0) return; /* config farfelue : taux inchangé */
    if (freq == emuDacRate) return;
    emuDacRate = freq;
    aq_configure(devCbSamples, (double)devRate, freq);
    fprintf(stderr, TR("audio : %d Hz (cadence timer du jeu −250 ppm)\n", "audio: %d Hz (game timer clock −250 ppm)\n"), (int)(freq + 0.5));
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
        fprintf(stderr, TR("fenêtre: %s\n", "window: %s\n"), SDL_GetError());
        return 1;
    }
    /* échelle logique = écran console : redimensionnable, rendu entier
     * (F10 change de mode de mise à l'échelle, F11 le plein écran) */
    scale_apply();

    /* manette déjà branchée : contrôleur sinon joystick brut */
    for (int i = 0; !noPad() && i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }
        else if (!joyFb) joyFb = SDL_JoystickOpen(i);
    }
    if (pad) printf(TR("manette : %s\n", "gamepad: %s\n"), SDL_GameControllerName(pad));
    else if (joyFb) printf(TR("joystick : %s\n", "joystick: %s\n"), SDL_JoystickName(joyFb));

    refresh_title();

    /* 512 fige l'émulateur sur emscripten (SPN audio) : ne pas descendre ;
     * taux fixe 48 kHz : le taux du jeu est rééchantillonné, jamais de
     * renégociation du périphérique système */
    audio_open_standard();
    audioOk = audioDev != 0;
    if (audioOk) {
        fprintf(stderr, TR("audio : périphérique %d Hz (le taux du jeu est rééchantillonné)\n",
                           "audio: device at %d Hz (game rate resampled)\n"), devRate);
        SDL_PauseAudioDevice(audioDev, 0); /* tourne en silence ;
            le pré-buffer est géré par audioPending dans le callback */
    } else {
        fprintf(stderr, TR("audio indisponible : %s\n", "audio unavailable: %s\n"), SDL_GetError());
    }
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
            printf(TR("déposé : %s\n", "dropped: %s\n"), fp);
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
            if (fwLoaded) { fw_restart(); printf(TR("redémarrage\n", "restart\n")); }
        }
        else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_F8) {
            dispFilter = !dispFilter;
            scale_apply();
            printf(TR("filtre : %s (F8)\n", "filter: %s (F8)\n"),
                   dispFilter ? "Game Boy DMG" : TR("aucun", "none"));
        }
        else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_F10) {
            dispScale = (dispScale + 1) % 3;
            scale_apply();
            printf(TR("échelle : %s\n", "scale: %s\n"), dispScale == 0 ? TR("entière (F10/F11)", "integer (F10/F11)")
                                   : dispScale == 1 ? TR("adaptée (F10/F11)", "fitted (F10/F11)")
                                                    : TR("étirée (F10/F11)", "stretched (F10/F11)"));
        }
        else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_F11) {
            dispFull = !dispFull;
            if (SDL_SetWindowFullscreen(emuWin, dispFull ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
                fprintf(stderr, "plein écran : %s\n", SDL_GetError());
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
                if (pad) printf(TR("manette : %s\n", "gamepad: %s\n"), SDL_GameControllerName(pad));
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

/* EMU_AUDIO_STATS : bilan audio par seconde émulée */
static uint32_t diagLastT, diagLastW, diagLastS, diagLastR, diagLastF;
static uint32_t diagLastU, diagLastC, diagLastN;
static unsigned long diagLastL;
static void update_diagnostics(Uint32 frame) {
    static int on = -1;
    if (on < 0) on = getenv("EMU_AUDIO_STATS") ? 1 : 0;
    if (!on) return;
    if (tickCount - diagLastT < (uint32_t)ticks_per_sec()) return;
    double sec = (double)(tickCount - diagLastT) / ticks_per_sec();
    if (emuTarget == TGT_POKITTO) {
        /* mêmes sondes que la META : production R2R (latches) ou HLE,
         * consommation hôte (callbacks), remplissage de la file Pokitto.
         * Les compteurs hôte sont pk_cbCalls/pk_cbSamples — audCb* ne
         * bouge que sur la voie META et donnait cb=0, trompeur. */
        uint32_t cbs = pk_cbCalls - diagLastC;
        fprintf(stderr, "[audio] f=%u latches=%lu (%.0f/s, muets=%lu son=%lu) hle=%s | hote: cb=%u éch/cb=%u sous-débit=%u file=%u\n",
                frame, pk_latchCount - diagLastL, (double)(pk_latchCount - diagLastL) / sec,
                pk_latchMid, pk_latchSound,
                pk_hleState == PK_HLE_ENABLED ? "on" : "off",
                cbs, cbs ? (pk_cbSamples - diagLastN) / cbs : 0, audUnder - diagLastU, pk_aqSize);
        diagLastL = pk_latchCount; diagLastC = pk_cbCalls;
        diagLastN = pk_cbSamples; diagLastU = audUnder;
    } else {
        int ahead = aq_tail - aq_head; if (ahead < 0) ahead += AQ_SIZE;
        fprintf(stderr, "[audio] f=%u dac/s=%.0f famine=%.1f%% relances=%u frames-hote=%u | hote: cb=%u éch/cb=%u sous-débit=%u file=%d\n",
                frame, (tc4Writes - diagLastW) / sec,
                100.0 * (audStarvedTicks - diagLastS) / ((tickCount - diagLastT) / (double)tc4_period_ticks()),
                audRestarts - diagLastR, frame - diagLastF,
                audCbCalls - diagLastC, audCbCalls - diagLastC ? (audCbSamples - diagLastN) / (audCbCalls - diagLastC) : 0,
                audUnder - diagLastU, ahead);
        diagLastW = tc4Writes; diagLastS = audStarvedTicks; diagLastR = audRestarts; diagLastF = frame;
        diagLastC = audCbCalls; diagLastN = audCbSamples; diagLastU = audUnder;
    }
    diagLastT = tickCount;
}

/* % de vitesse dans la barre de titre (échantillonné toutes les 500 ms) */
static void update_title_pct(void) {
    Uint32 nowMs = SDL_GetTicks();
    if (nowMs - titleMs < 500) return;
    if (fwLoaded) {
        /* emuMs en MILLISECONDES : ticks_per_sec est en hertz (48 MHz META,
         * horloge configurée par le guest côté Pokitto) depuis le passage au
         * timing fidèle — la formule donnait des secondes contre des
         * millisecondes côté horloge murale et le % tombait à 0 sur les
         * deux cibles. */
        double emuMs = (double)(tickCount - titleTick) * 1000.0 / ticks_per_sec();
        double wallMs = (double)(nowMs - titleMs);
        int pct = wallMs > 0.0 ? (int)(emuMs / wallMs * 100.0 + 0.5) : 0;
        /* Les vraies perfs, sans la limite temps réel : temps émulé produit
         * par temps mural réellement passé dans l'émulation (accumulé par
         * les boucles natives/wasm autour de run_emulated_frame).  Affiché
         * à côté quand il diffère du % limité. */
        int raw = rawWallMs > 0.0 ? (int)(rawEmuMs / rawWallMs * 100.0 + 0.5) : 0;
        rawEmuMs = rawWallMs = 0;
        if (pct < 0) pct = 0;
        /* PAS d'écrêtage à 100 : un dépassement (EMU_NOPACE, vieux binaire
         * face aux firmwares récents) doit se VOIR dans le titre. */
        if (pct > 999) pct = 999;
        titlePct = pct;
        snprintf(titleBuf, sizeof(titleBuf), "%.900s", fwName);
        if (raw > pct + 5 && raw > 100) {
            if (raw > 9999) raw = 9999;
            snprintf(hudTitle, sizeof(hudTitle), "%.900s — %d %% - %d %%", fwName, pct, raw);
        } else {
            snprintf(hudTitle, sizeof(hudTitle), "%.900s — %d %%", fwName, pct);
        }
    } else {
        snprintf(titleBuf, sizeof(titleBuf), TR("déposez un firmware .bin", "drop a .bin firmware"));
        snprintf(hudTitle, sizeof(hudTitle), "%s", titleBuf);
    }
    SDL_SetWindowTitle(emuWin, hudTitle);
    titleMs = nowMs;
    titleTick = tickCount;
}

/* boucle de frame batchée : petite fonction appelée 60 fois par seconde —
 * V8 la promeut vite vers TurboFan, et step y est appelé DIRECTEMENT (pas
 * d'indirection par instruction).  noinline sur step : s'il était inliné
 * ici, la boucle resterait au compilateur de base (pas d'OSR en wasm).
 *
 * tickCount est un u32 qui wrappe (2^32 à 48 MHz ≈ 89,5 s de jeu) : la
 * cible calculée par la frame précédente peut se retrouver EN DEÇA du tick
 * courant juste après le passage — comparaison signée de la différence,
 * comme sur le compteur (qui wrappe) du Cortex-M0+.  La forme
 * « tick < cible » non signée gelait l'émulateur pour de bon (lapinou,
 * frame ~7000, % du HUD wasm figé à 0). */
__attribute__((noinline))
static void step_batch(uint32_t target) {
    while ((int32_t)(tickCount - target) < 0) step();
}

static void run_emulated_frame(void) {
    uint32_t target = emu_nextFrameTick;
    step_batch(target);
    /* re-base sur tickCount : l'ancien `+= frame_ticks()` laissait
     * emu_nextFrameTick franchir 2^32 UNE frame avant tickCount — le
     * comparateur non signé ne voyait plus rien à exécuter et
     * l'émulateur restait ~12 825 frames sans aucune instruction
     * (214 s figées à la 3,6e minute de jeu, interruptions mortes). */
    emu_nextFrameTick = tickCount + frame_ticks();
}

#if defined(EMU_NODE_HEADLESS)
/* ------------------------------------------- node headless (debug) --- */
/* prog firmware.bin carte_dir|.zip [frames] [capture.ppm] — la capture
 * permet de vérifier l'écran du jeu dans le cœur wasm exact du build
 * navigateur (mêmes sources, sans SDL initialisée). */

int main(int argc, char **argv) {
    memset(sram, 0xff, SRAM_SIZE);
    if (argc < 3) { fprintf(stderr, "usage: prog firmware.bin|.zip carte_dir|.zip [frames] [capture.ppm]\n"); return 1; }
    { /* argv[1] = zip (PK) : carte+firmware, comme le drop du navigateur */
        FILE *z = fopen(argv[1], "rb");
        int iszip = 0;
        if (z) { unsigned char sig[2] = {0,0}; iszip = fread(sig, 1, 2, z) == 2 && sig[0] == 'P' && sig[1] == 'K'; fclose(z); }
        if (iszip) {
            load_sd_from_path(argv[1]);
            if (fwLoaded) boot_vectors();
        } else {
            load_firmware(argv[1], 0);
            if (fwLoaded) boot_vectors();
            if (argc > 2) load_sd_from_path(argv[2]);
        }
    }
    int frames = argc > 3 ? atoi(argv[3]) : 700;
    for (int f = 0; f < frames; f++) run_emulated_frame();
    fprintf(stderr, "FINAL %u %08x\n", tickCount, state_hash());
    uint32_t h = 0x811c9dc5u;
    int nz = 0;
    for (unsigned i = 0; i < SCR_W * SCR_H; i++) {
        h = (h ^ pix[i]) * 0x01000193u;
        if (pix[i]) nz++;
    }
    fprintf(stderr, "ECRAN %08x nz=%d\n", h, nz);
    if (argc > 4) {
        FILE *g = fopen(argv[4], "wb");
        if (g) {
            fprintf(g, "P6\n%d %d\n255\n", SCR_W, SCR_H);
            for (unsigned i = 0; i < SCR_W * SCR_H; i++) {
                uint16_t p = pix[i];
                unsigned char rgb[3] = {
                    (unsigned char)(((p >> 11) & 0x1f) * 255 / 31),
                    (unsigned char)(((p >> 5) & 0x3f) * 255 / 63),
                    (unsigned char)((p & 0x1f) * 255 / 31) };
                fwrite(rgb, 1, 3, g);
            }
            fclose(g);
        }
    }
    return 0;
}

#elif !defined(__EMSCRIPTEN__)
/* ------------------------------------------------------------ natif --- */

int main(int argc, char **argv) {
    if (argc < 2)
        fprintf(stderr, TR("gm0 : lancé sans firmware — déposez un .bin "
                        "dans la fenêtre (carte SD = son répertoire).\n",
                        "gm0: started without a firmware — drop a .bin "
                        "into the window (SD card = its directory).\n"));
    memset(sram, 0xff, SRAM_SIZE); /* comme le TS (constructeur Atsamd21) */
    if (getenv("EMU_TARGET")) {
        if (strcasecmp(getenv("EMU_TARGET"), "pokitto") == 0) emuTarget = TGT_POKITTO;
        if (strcasecmp(getenv("EMU_TARGET"), "meta") == 0) emuTarget = TGT_META;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--build-vcard") == 0 && i + 3 < argc) {
            /* debug : --build-vcard out.img fichier1 fichier2 ... */
            for (int j = i + 2; j < argc; j++) {
                FILE *g = fopen(argv[j], "rb");
                if (!g) { fprintf(stderr, TR("vcard: %s illisible\n", "vcard: %s unreadable\n"), argv[j]); continue; }
                fseek(g, 0, SEEK_END); long sz = ftell(g); fseek(g, 0, SEEK_SET);
                uint8_t *data = malloc((size_t)sz);
                fread(data, 1, (size_t)sz, g); fclose(g);
                vfiles_add(argv[j], data, (size_t)sz);
                free(data);
            }
            fat_build_from_vfiles();
            FILE *out = fopen(argv[i + 1], "wb");
            if (out) { fwrite(fatImage, 1, fatImageSize, out); fclose(out); }
            printf(TR("vcard écrite : %s\n", "vcard written: %s\n"), argv[i + 1]);
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
            else { fprintf(stderr, TR("cible inconnue : %s (meta|pokitto)\n", "unknown target: %s (meta|pokitto)\n"), argv[i]); return 1; }
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
        /* test headless : EMU_PRESS_A=<frame> appuie sur A 6 frames
         * (idem EMU_PRESS_B / EMU_PRESS_MENU — « A+B » des titres, Start) */
        static int pressA = -1;
        static int pressB = -1;
        static int pressM = -1;
        if (pressA < 0) pressA = getenv("EMU_PRESS_A") ? atoi(getenv("EMU_PRESS_A")) : 0;
        if (pressB < 0) pressB = getenv("EMU_PRESS_B") ? atoi(getenv("EMU_PRESS_B")) : 0;
        if (pressM < 0) pressM = getenv("EMU_PRESS_MENU") ? atoi(getenv("EMU_PRESS_MENU")) : 0;
        if (pressA && (frame == pressA)) btn_press(BTN_A);
        if (pressA && (frame == pressA + 6)) btn_release(BTN_A);
        if (pressB && (frame == pressB)) btn_press(BTN_B);
        if (pressB && (frame == pressB + 6)) btn_release(BTN_B);
        if (pressM && (frame == pressM)) btn_press(BTN_MENU);
        if (pressM && (frame == pressM + 6)) btn_release(BTN_MENU);
        { /* EMU_INPUT="frame:touches:durée,..." — touches parmi U D L R A B
           * M(enu) H(ome), ex. "200:H:45,260:A:3,280:B:3" (menu d'options) */
            static const char *in = (const char *)1;
            if (in == (const char *)1) in = getenv("EMU_INPUT");
            for (const char *q = in; q && *q;) {
                unsigned f0 = 0, len = 3; char keys[16] = {0};
                if (sscanf(q, "%u:%15[UDLRABMH]:%u", &f0, keys, &len) >= 2) {
                    uint8_t m = 0;
                    for (char *k = keys; *k; k++)
                        m |= *k == 'U' ? BTN_UP : *k == 'D' ? BTN_DOWN : *k == 'L' ? BTN_LEFT :
                             *k == 'R' ? BTN_RIGHT : *k == 'A' ? BTN_A : *k == 'B' ? BTN_B :
                             *k == 'M' ? BTN_MENU : BTN_HOME;
                    if (frame == f0) btn_press(m);
                    if (frame == f0 + len) btn_release(m);
                }
                q = strchr(q, ','); if (q) q++;
            }
        }
#endif

        if (fwLoaded) {
            Uint64 emuT0 = SDL_GetPerformanceCounter();
            run_emulated_frame();
            /* perf brute (titre « brut N % ») : le temps émulé produit par
             * le temps mural réellement passé à émuler, hors attente */
            double benchEmuMs = (double)frame_ticks() * 1000.0 / ticks_per_sec();
            double benchWallMs = (double)(SDL_GetPerformanceCounter() - emuT0) * 1000.0 / perfFreq;
            rawEmuMs += benchEmuMs; rawWallMs += benchWallMs;
            rawEmuMsTotal += benchEmuMs; rawWallMsTotal += benchWallMs;
        }
        frame++;
        if (emuTarget == TGT_POKITTO && ENVFLAG("EMU_DMA_TRACE") && (frame % 300) == 0) {
            uint32_t msc = pk_rd32le(sram + (0x10005c64u - 0x10000000u));
            uint32_t gframe = pk_rd32le(sram + (0x10000508u - 0x10000000u));
            static Uint32 w0 = 0; if (!w0) w0 = SDL_GetTicks();
            Uint32 w = SDL_GetTicks() - w0;
            fprintf(stderr, "[derive] f=%u tick=%u wall=%.2fs ms=%u (%.1f Hz emu | %.1f Hz wall) gbf=%u (%.2f fps emu | %.2f fps wall)\n",
                    frame, tickCount, w / 1000.0, msc,
                    (double)msc * pk_core_hz() / (double)tickCount,
                    (double)msc * 1000.0 / (double)w,
                    gframe, (double)gframe * pk_core_hz() / (double)tickCount,
                    (double)gframe * 1000.0 / (double)w);
        }

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
        static int noPace = -1; /* EMU_NOPACE=1 : vitesse brute (mesure) */
        if (noPace < 0) noPace = getenv("EMU_NOPACE") ? 1 : 0;
        if (!noPace) {
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

    if (ENVFLAG("EMU_DMA_TRACE") && emuTarget == TGT_POKITTO)
        fprintf(stderr, "[audio] FIN irq_block=%lu irq34=%lu crosses=%lu latches=%lu (muets=%lu son=%lu) tick=%u qtail=%u qhead=%u\n",
                pk_ctIrqCount, pk_irq34, pk_ctCross, pk_latchCount, pk_latchMid, pk_latchSound, tickCount,
                pk_read_half(QADDR), pk_read_half(QADDR + 2));
    if (ENVFLAG("EMU_DMA_TRACE") && emuTarget == TGT_POKITTO) {
        uint32_t msc = pk_rd32le(sram + (0x10005c64u - 0x10000000u));
        uint32_t gframe = pk_rd32le(sram + (0x10000508u - 0x10000000u));
        fprintf(stderr, "[timing] ticks=%u (%.2f s emu @ %.0f MHz) ms_count=%u (%.1f Hz) gbl_frame=%u (%.2f fps GB)\n",
                tickCount, tickCount / pk_core_hz(), pk_core_hz() / 1e6,
                msc, (double)msc * pk_core_hz() / (double)tickCount,
                gframe, (double)gframe * pk_core_hz() / (double)tickCount);
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
            printf(TR("capture : %s\n", "shot: %s\n"), shotPath);
        }
    }
    wav_finish();
    prof_write();
    pk_debug_dump();
    pk_eeprom_save();
    pk_card_export();
    { const char *fd = getenv("FLASH_DUMP"); /* flash après auto-patch du jeu */
      if (fd && emuTarget == TGT_META) {
        FILE *g = fopen(fd, "wb");
        if (g) { fwrite(flash, 1, FLASH_SIZE, g); fclose(g); }
      } }
    { const char *fd = getenv("FAT_DUMP_EXIT"); /* carte SD après la session */
      if (fd && sd_card_data()) {
        FILE *g = fopen(fd, "wb");
        if (g) { fwrite(sd_card_data(), 1, sd_card_size(), g); fclose(g); }
      } }
    if (rawWallMsTotal > 0.0) /* vitesse brute du run, hors attente de pacing */
        fprintf(stderr, TR("[bench] frames=%u wall=%.2fs emu=%.2fs brut=%.0f%% (%.0f MHz effectifs)\n", "[bench] frames=%u wall=%.2fs emu=%.2fs raw=%.0f%% (%.0f MHz effective)\n"),
                frame, rawWallMsTotal / 1000.0, rawEmuMsTotal / 1000.0,
                rawEmuMsTotal / rawWallMsTotal * 100.0,
                (double)ticks_per_sec() / 1e6 * rawEmuMsTotal / rawWallMsTotal);
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
        size_t nl = strlen(vfiles[i].path);
        int iszip = nl > 4 && strcasecmp(vfiles[i].path + nl - 4, ".zip") == 0;
        if (!iszip && !vfile_is_game(&vfiles[i])) return 0;
        reset_core();
        booted = 0;
        if (iszip) {
            /* zip = carte complète + son premier .bin comme firmware
             * (même chemin que le drop navigateur, aplatissement inclus) */
            if (!zip_load_card(vfiles[i].data, vfiles[i].size)) return 0;
            emu_boot();
            return 1;
        }
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
    printf(TR("carte SD : image (%d Kio)\n", "SD card: image (%d KiB)\n"), len / 1024);
}

EMSCRIPTEN_KEEPALIVE
int emu_zip_load(uint8_t *data, int len) {
    /* un zip recharge un firmware : les vecteurs sont à reprendre — sans
     * ça, booted restait à 1 et seul le PREMIER jeu META recevait
     * boot_vectors (les suivants tournaient sans SP/PC, écran noir à
     * 100 %) ; les jeux Pokitto s'en tiraient car pk_reset_core relit
     * elle-même les vecteurs après chargement de la flash */
    booted = 0;
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
     * 4 frames (onglet caché, stall) : pas de rattrapage.  due est ARRONDI
     * (pas tronqué) : un rAF à 60,0 Hz exact donne 16,68 ms < frameMs, la
     * troncature restait à due=0 pour toujours (machine à l'arrêt). */
    double now = emscripten_get_now();
    const double frameMs = 16.743;
    if (wasmLastFrame == 0) wasmLastFrame = now;
    int due = (int)((now - wasmLastFrame) / frameMs + 0.5);
    if (due > 4) { due = 1; wasmLastFrame = now; }
    if (fwLoaded && booted && !wasmPaused) {
        double emuT0 = emscripten_get_now();
        for (int i = 0; i < due; i++) run_emulated_frame();
        /* perf brute (titre « brut N % »), comme la boucle native */
        rawEmuMs += (double)due * frame_ticks() * 1000.0 / ticks_per_sec();
        rawWallMs += emscripten_get_now() - emuT0;
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
    titleMs = (Uint32)emscripten_get_now();
    titleTick = tickCount;
    if (sdl_init_all() != 0) return 1;
    emscripten_set_main_loop(wasm_loop, 0, 1);
    return 0;
}
#endif
