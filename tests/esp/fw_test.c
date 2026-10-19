/* Firmware de test de la machine ESPboy de gm0 (sans Arduino ni SDK) : écran
 * par SPI1, sigma-delta, timer1 et son interruption, boutons du MCP23017 par
 * twi_*, sauvegarde en flash par spi_flash_*.  Les fonctions « du SDK » ont ici
 * des corps vides : gm0 les accroche par leur nom. */
#include <stdint.h>
#define REG(a) (*(volatile uint32_t *)(a))
#define HOOK __attribute__((noipa, noinline)) /* le compilateur ne doit rien en déduire : gm0 remplace le corps */
#define RTC_GPIO_OUT 0x60000768u
#define SPI1 0x60000100u
#define GPSD 0x60000368u
#define T1L 0x60000600u
#define T1C 0x60000608u

HOOK uint32_t micros(void) { return 0; }
HOOK void delay(unsigned long ms) { (void)ms; }
HOOK uint8_t twi_writeTo(unsigned char a, unsigned char *b, unsigned l, unsigned char s) { (void)a; (void)b; (void)l; (void)s; return 0; }
HOOK uint8_t twi_readFrom(unsigned char a, unsigned char *b, unsigned l, unsigned char s) { (void)a; (void)b; (void)l; (void)s; return 0; }
HOOK int spi_flash_read(uint32_t a, uint32_t *d, uint32_t n) { (void)a; (void)d; (void)n; return 0; }
HOOK int spi_flash_write(uint32_t a, uint32_t *d, uint32_t n) { (void)a; (void)d; (void)n; return 0; }
HOOK int spi_flash_erase_sector(uint32_t s) { (void)s; return 0; }
HOOK uint32_t system_update_cpu_freq(uint8_t m) { (void)m; return 1; }
typedef void (*isr_attach_t)(int, void *, void *);
typedef void (*isr_unmask_t)(uint32_t);
#define ets_isr_attach ((isr_attach_t)0x40000f88)
#define ets_isr_unmask ((isr_unmask_t)0x40000fa8)

static volatile uint32_t ticks;
static void isr(void *arg) { (void)arg; ticks++; REG(GPSD) = 0x10000u | ((ticks & 1u) ? 200u : 56u); }

static void spi_byte(unsigned dc, uint8_t b) {
    REG(RTC_GPIO_OUT) = dc;
    REG(SPI1 + 0x1c) = 1u << 27;
    REG(SPI1 + 0x20) = 7u << 17;
    REG(SPI1 + 0x40) = b;
    REG(SPI1) = 1u << 18;
}
static void cmd(uint8_t c) { spi_byte(0, c); }
static void data(uint8_t b) { spi_byte(1, b); }
static void window(void) {
    cmd(0x2a); data(0); data(2); data(0); data(129);
    cmd(0x2b); data(0); data(3); data(0); data(130);
    cmd(0x2c);
}
static uint8_t buttons(void) {
    uint8_t reg = 0x12, v = 0xff;
    twi_writeTo(0x20, &reg, 1, 0);
    twi_readFrom(0x20, &v, 1, 1);
    return (uint8_t)~v;
}
static uint32_t saved[4];

void setup(void) {
    system_update_cpu_freq(160);
    REG(0x60000774) = 1;           /* RTC_GPIO_ENABLE : GPIO16 (D/C) en sortie */
    REG(0x3ff00004) |= 2;          /* DPORT : interruption de front de FRC1 autorisée */
    { unsigned char pu[2] = {0x0c, 0xff}; twi_writeTo(0x20, pu, 2, 1); } /* GPPUA : tirages des boutons (sans eux, 0 = tout appuyé) */
#ifdef CS_HIGH
    { unsigned char b[3] = {0x01, 0xfe, 0}, o[2] = {0x15, 0x01}; /* IODIRB : B0 en sortie ; OLATB : B0 haut = écran non sélectionné */
      twi_writeTo(0x20, b, 2, 1); twi_writeTo(0x20, o, 2, 1); }
#endif
    cmd(0x01); cmd(0x11);
#ifndef MADCTL
#define MADCTL 0xc8 /* MX | MY | BGR : ce que pose le firmware m0boy */
#endif
    cmd(0x36); data(MADCTL);
    cmd(0x3a); data(5); /* 16 bits par pixel (au reset : 18) */
    cmd(0x29);
    spi_flash_read(0x3c0000, saved, 16);
    uint32_t had = saved[0] == 0x50415645u; /* « EVAP » écrit par une partie précédente */
    ets_isr_attach(9, (void *)isr, 0);
    ets_isr_unmask(1u << 9);
    REG(T1L) = 5000000u / 11025u; /* timer1 : 11025 Hz à 80 MHz / 16 */
    REG(T1C) = 0x80u | 0x40u | 0x04u; /* actif, recharge, /16 */
    for (unsigned frame = 0;; frame++) {
        uint8_t k = buttons();
        window();
        for (unsigned y = 0; y < 128; y++)
            for (unsigned x = 0; x < 128; x++) {
                uint16_t c = (uint16_t)(((x >> 2) << 11) | ((y >> 1) << 5) | (had ? 31 : 0));
                if (k & 8) c ^= 0xffffu;              /* droite : couleurs inversées */
                if (y < 8 && x < 8) c = had ? 0x07e0 : 0xf800; /* repère : vert si sauvegarde relue, rouge sinon */
                data((uint8_t)(c >> 8)); data((uint8_t)c);
            }
        if (frame == 3 && !had) { /* première partie : écrit la sauvegarde */
            static uint32_t blk[4] = {0x50415645u, 1, 2, 3};
            spi_flash_erase_sector(0x3c0);
            spi_flash_write(0x3c0000, blk, 16);
        }
        delay(16);
    }
}
void loop(void) {}
