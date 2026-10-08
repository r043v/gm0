/* Test unitaire du NVIC ARMv6-M, commun au META (SAMD21) et à la Pokitto
 * (LPC11U6x, Cortex-M0+ r0p1, quatre niveaux de priorité) : activation
 * (ISER/ICER), PRIMASK, préemption stricte par priorité, HardFault non
 * masquable, retour d'exception, ligne CT encore levée, front de broche
 * latché, SysTick (COUNTFLAG -> PENDSTSET), accès PPB.  Le fichier inclut
 * gm0.c pour atteindre les fonctions statiques.  Lancer tests/unit/run.sh. */
#define main gm0_main
#include "../../gm0.c"
#undef main

static int fails, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "ÉCHEC %s:%d : ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

/* gestionnaire d'exception : adresse Thumb dans la table de vecteurs (flash) */
static void set_vector(int exc, uint32_t handler) {
    uint32_t a = 4u * (uint32_t)exc;
    for (int k = 0; k < 4; k++) flash[a + k] = (uint8_t)(handler >> (8 * k));
}

/* état Pokitto propre à chaque cas, pile dans la SRAM */
static void pokitto_reset(void) {
    emuTarget = TGT_POKITTO;
    sys_VTOR = 0;
    primask = 0;
    nvic_reset();
    memset(pk_ct, 0, sizeof pk_ct);
    memset(pk_pin, 0, sizeof pk_pin);
    memset(pk_syscon, 0, sizeof pk_syscon);
    pk_ist = pk_ienr = pk_ienf = pk_rise = pk_fall = 0;
    pk_systickCSR = 0;
    regs[13] = 0x10001000u;
    regs[15] = 0x00000204u;
    regs[14] = 0;
    fN = fZ = fC = fV = 0;
}

/* IRQ n : vecteur, activation et priorité (2 bits de poids fort) */
static void irq_setup(int n, uint32_t handler, int enable, int prio) {
    set_vector(16 + n, handler);
    if (enable) nvicIser |= 1u << n;
    nvicIpr[n] = (uint8_t)((prio & 3) << 6);
}

int main(void) {
    /* 1. une IRQ en attente n'est prise que si elle est activée (ISER) */
    pokitto_reset();
    irq_setup(3, 0x00000801u, 0, 0);
    nvicPend = 1u << 3;
    irq_work(); nvic_service();
    CHECK(excDepth == 0, "IRQ désactivée prise quand même");
    nvicIser |= 1u << 3; irq_work(); nvic_service();
    CHECK(excDepth == 1 && excNum[0] == 19, "IRQ 3 activée non prise (profondeur %d)", excDepth);
    CHECK(regs[15] - 2u == 0x00000800u, "vecteur d'IRQ 3 : PC %08x", regs[15] - 2u);
    CHECK(regs[14] == 0xfffffff9u, "EXC_RETURN : %08x", regs[14]);
    CHECK((nvicPend & (1u << 3)) == 0, "pendance non effacée à l'entrée");

    /* 2. PRIMASK masque les prises configurables, levée -> prise */
    pokitto_reset();
    irq_setup(3, 0x00000801u, 1, 0);
    nvicPend = 1u << 3; primask = 1; irq_work(); nvic_service();
    CHECK(excDepth == 0, "PRIMASK ne masque pas l'IRQ");
    primask = 0; irq_work(); nvic_service();
    CHECK(excDepth == 1, "IRQ non prise après levée de PRIMASK");

    /* 3. préemption : seulement si la priorité est STRICTEMENT plus haute */
    pokitto_reset();
    irq_setup(3, 0x00000801u, 1, 2);  /* IRQ 3 : priorité 2 */
    irq_setup(4, 0x00000901u, 1, 1);  /* IRQ 4 : priorité 1 */
    irq_setup(5, 0x00000A01u, 1, 1);  /* IRQ 5 : priorité 1 */
    nvicPend = 1u << 3; irq_work(); nvic_service();
    CHECK(excDepth == 1 && excNum[0] == 19, "IRQ 3 pas prise");
    nvicPend |= 1u << 4; irq_work(); nvic_service();
    CHECK(excDepth == 2 && excNum[1] == 20, "IRQ 4 (priorité 1) ne préempte pas IRQ 3 (priorité 2)");
    nvicPend |= 1u << 5; irq_work(); nvic_service();
    CHECK(excDepth == 2, "même priorité (IRQ 5 contre IRQ 4) : préemption à tort");
    CHECK(nvicPend & (1u << 5), "IRQ 5 perdue au lieu de rester en attente");

    /* 4. HardFault : non masquable, priorité -1, même sans ISER ni PRIMASK */
    pokitto_reset();
    set_vector(3, 0x00000901u);
    primask = 1;
    pk_reg_write(0x00001000u, 0x12345678u); /* écriture en flash : faute */
    CHECK(excDepth == 1 && excNum[0] == 3, "HardFault non prise sous PRIMASK (profondeur %d)", excDepth);
    CHECK(regs[15] - 2u == 0x00000900u, "vecteur HardFault : PC %08x", regs[15] - 2u);
    CHECK(exc_priority(3) == -1, "priorité HardFault %d", exc_priority(3));

    /* 5. retour : la trame est dépilée, le PC d'origine revient */
    pokitto_reset();
    irq_setup(3, 0x00000801u, 1, 0);
    nvicPend = 1u << 3; irq_work(); nvic_service();
    exc_return();
    CHECK(excDepth == 0, "profondeur non revenue à 0 après retour");
    CHECK(regs[15] == 0x00000204u, "PC après retour : %08x", regs[15]);

    /* 6. CT : la ligne encore levée à la sortie redevient en attente */
    pokitto_reset();
    set_vector(34, 0x00000A01u);
    nvicIser |= 1u << 18;
    pk_ct[0].r[PK_CT_IR] = 1; nvicPend |= 1u << 18; irq_work(); nvic_service();
    CHECK(excDepth == 1 && excNum[0] == 34, "CT0 non prise");
    exc_return();
    CHECK(nvicPend & (1u << 18), "CT0 toujours levée : pas de nouvelle prise au retour");
    pokitto_reset();
    set_vector(34, 0x00000A01u);
    nvicIser |= 1u << 18;
    pk_ct[0].r[PK_CT_IR] = 1; nvicPend |= 1u << 18; irq_work(); nvic_service();
    pk_ct[0].r[PK_CT_IR] = 0; /* le handler efface l'IR */
    exc_return();
    CHECK((nvicPend & (1u << 18)) == 0, "CT0 effacée par le handler mais toujours en attente");

    /* 7. front de broche : latché dans le NVIC, prise à l'activation */
    pokitto_reset();
    irq_setup(0, 0x00000801u, 1, 0);
    pk_syscon[PK_SYSCON_PINTSEL(0)] = 33;  /* broche 1_9 (bouton A) */
    pk_ienr = 1u;
    pk_gpio_input(1, 9, 1);
    CHECK(pk_ist & 1u, "IST non levé sur un front attendu");
    CHECK(nvicPend & 1u, "front de broche non latché au NVIC");
    nvic_service();
    CHECK(excDepth == 1 && excNum[0] == 16, "broche 0 non prise");

    /* 8. SysTick : COUNTFLAG -> PENDSTSET seulement avec TICKINT */
    pokitto_reset();
    pk_systickCSR = 1u | (1u << 16);   /* ENABLE, sans TICKINT */
    pk_systick_pend();
    CHECK(sysTickPend == 0 && (pk_systickCSR & (1u << 16)), "SysTick pris sans TICKINT");
    pk_systickCSR |= 2u;               /* TICKINT posé : pendance */
    pk_systick_pend();
    CHECK(sysTickPend == 1 && !(pk_systickCSR & (1u << 16)), "SysTick non mis en attente");
    nvic_service();
    CHECK(excDepth == 1 && excNum[0] == 15 && sysTickPend == 0, "SysTick en attente non prise");

    /* 9. accès PPB : ISER/ICER, IPR, CPUID et ICSR passent par le NVIC commun */
    pokitto_reset();
    pk_reg_write(0xE000E100u, 1u << 5);
    CHECK(nvicIser == (1u << 5), "ISER : %08x", nvicIser);
    CHECK(pk_reg_read(0xE000E100u) == (1u << 5), "lecture ISER : %08x", pk_reg_read(0xE000E100u));
    pk_reg_write(0xE000E180u, 1u << 5);
    CHECK(nvicIser == 0, "ICER : %08x", nvicIser);
    pk_reg_write(0xE000E400u, 0x40000000u); /* octet 3 : IRQ 3, priorité 1 */
    CHECK(nvicIpr[3] == 0x40, "IPR de l'IRQ 3 : %02x", nvicIpr[3]);
    CHECK(pk_reg_read(0xE000E400u) == 0x40000000u, "lecture IPR : %08x", pk_reg_read(0xE000E400u));
    CHECK(pk_reg_read(0xE000ED00u) == 0x410CC601u, "CPUID : %08x", pk_reg_read(0xE000ED00u));

    printf("%d vérifications, %d échec(s)\n", checks, fails);
    return fails ? 1 : 0;
}
