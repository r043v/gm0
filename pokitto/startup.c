
#include "pokitto.h"

#define REG(x) (*((volatile unsigned int *)(x)))

extern void _vStackTop(void);
extern void SystemInit();

void __wrap_exit( int num ){
  //  using Reg = volatile unsigned int *;

    unsigned int *bootinfo = (unsigned int*)0x3FFF4;
    if (*bootinfo != 0xB007AB1E) bootinfo = (unsigned int*)0x3FF04; //allow couple of alternative locations
    if (*bootinfo != 0xB007AB1E) bootinfo = (unsigned int*)0x3FE04; //allow couple of alternative locations
    if (*bootinfo != 0xB007AB1E) bootinfo = (unsigned int*)0x3F004; //for futureproofing
    if (*bootinfo != 0xB007AB1E)
        REG(0xE000ED0C) = 0x05FA0004; //issue system reset

    __asm volatile ("cpsid i" : : : "memory");
//    __disable_irq();// Start by disabling interrupts, before changing interrupt v

    unsigned long app_link_location = bootinfo[2];

	REG(0x40014000 + 20) = 0; // TIMER32_0_BASE + MCR;
	REG(0x40018000 + 20) = 0; // TIMER32_1_BASE + MCR;
    REG(0xE000E180) = ~0; // NVIC->ICER[0] = (1 << ((uint32_t)(IRQn) & 0x1F));

    REG(0xA0004004) = 0; // LPC_PINT->IENR = 0;
    REG(0xA0004010) = 0; // LPC_PINT->IENF = 0;
    REG(0xE000E010) &= ~1; // SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk & ~(SysTick_CTRL_ENABLE_Msk); //disable systick
    REG(0x40048238) |= 1 << 10; // LPC_SYSCON->PDRUNCFG     |=  (1 << 10);       /* Power-down USB PHY         */
    REG(0x40048238) |= 1 << 8; // LPC_SYSCON->PDRUNCFG     |=  (1 <<  8);       /* Power-down USB PLL         */

// reset clock source to IRC
    REG(0x40048074) = 0x01; // LPC_SYSCON->MAINCLKUEN    = 0x01;             /* Update MCLK Clock Source   */
    REG(0x40048074) = 0x00; // LPC_SYSCON->MAINCLKUEN    = 0x00;             /* Toggle Update Register     */
    while(REG(0x40048074) & 0x01 ); // LPC_SYSCON->MAINCLKUEN & 0x01);     /* Wait Until Updated         */
// switch clock selection to IRC
    REG(0x40048070) = 0; // LPC_SYSCON->MAINCLKSEL    = 0;             /* Select Clock Source        */
    REG(0x40048074) = 0x01; // LPC_SYSCON->MAINCLKUEN    = 0x01;             /* Update MCLK Clock Source   */
    REG(0x40048074) = 0x00; // LPC_SYSCON->MAINCLKUEN    = 0x00;             /* Toggle Update Register     */
    while(REG(0x40048074) & 0x01 ); // LPC_SYSCON->MAINCLKUEN & 0x01);     /* Wait Until Updated         */
//disable PLL clock output
    REG(0x40048044) = 0; // LPC_SYSCON->SYSPLLCLKUEN = 0;
    while(REG(0x40048044) & 0); // LPC_SYSCON->SYSPLLCLKUEN & 0x00); // Eh?!
    REG(0x40048008) = 0; // LPC_SYSCON->SYSPLLCTRL = 0;

//kill peripherals
    REG(0x40048070) = 0; // LPC_SYSCON->MAINCLKSEL = 0;
    REG(0x40048004) = 0; // LPC_SYSCON->PRESETCTRL = 0; //disable all peripherals

//power down PLL
    volatile unsigned int tmp;
    tmp = (REG(0x40048238) & 0x000025FFL); // LPC_SYSCON->PDRUNCFG
    tmp |= ((1<<7) & 0x000025FFL);
    REG(0x40048238) = (tmp | 0x0000C800L); /* Power-down SYSPLL          */

//Chip_Clock_SetMainClockSource(SYSCTL_MAINCLKSRC_IRC); //switch to IRC

// clear all gpio states
    REG(0xA0002100) = 0; // LPC_GPIO_PORT->PIN[0] = 0;
    REG(0xA0002104) = 0; // LPC_GPIO_PORT->PIN[1] = 0;
    REG(0xA0002108) = 0; // LPC_GPIO_PORT->PIN[2] = 0;

// SCB->VTOR = app_link_location;//APPL_ADDRESS; /* Change vector table address
    REG(0xE000ED08) = app_link_location;

    __asm(" mov r0, %[address]"::[address] "r" (app_link_location));
    __asm(" ldr r1, [r0,#0]"); // get the stack pointer value from the program's reset vector
    __asm(" mov sp, r1");      // copy the value to the stack pointer
    __asm(" ldr r0, [r0,#4]"); // get the program counter value from the program's reset vector
    __asm(" blx r0");          // jump to the' start address
}

#define WEAK          __attribute__ ((weak))
#define ALIAS(f)      __attribute__ ((weak, alias (#f)))
#define AFTER_VECTORS __attribute__ ((section(".after_vectors")))void ResetISR(void);

extern unsigned int __data_section_table;
extern unsigned int __data_section_table_end;
extern unsigned int __bss_section_table;
extern unsigned int __bss_section_table_end;


extern void __libc_init_array(void);
//extern int main(void);
extern int main(void);
extern void (* const g_pfnVectors[])(void);

     void ResetISR(void);
WEAK void NMI_Handler(void);
WEAK void HardFault_Handler(void);
WEAK void SVC_Handler(void);
WEAK void PendSV_Handler(void);
WEAK void SysTick_Handler(void);
WEAK void IntDefaultHandler(void);

void PIN_INT0_IRQHandler (void) ALIAS(IntDefaultHandler);
void PIN_INT1_IRQHandler (void) ALIAS(IntDefaultHandler);
void PIN_INT2_IRQHandler (void) ALIAS(IntDefaultHandler);
void PIN_INT3_IRQHandler (void) ALIAS(IntDefaultHandler);
void PIN_INT4_IRQHandler (void) ALIAS(IntDefaultHandler);
void PIN_INT5_IRQHandler (void) ALIAS(IntDefaultHandler);
void PIN_INT6_IRQHandler (void) ALIAS(IntDefaultHandler);
void PIN_INT7_IRQHandler (void) ALIAS(IntDefaultHandler);
void GINT0_IRQHandler (void) ALIAS(IntDefaultHandler);
void GINT1_IRQHandler (void) ALIAS(IntDefaultHandler);
void I2C1_IRQHandler (void) ALIAS(IntDefaultHandler);
void USART1_4_IRQHandler (void) ALIAS(IntDefaultHandler);
void USART2_3_IRQHandler (void) ALIAS(IntDefaultHandler);
void SCT0_1_IRQHandler (void) ALIAS(IntDefaultHandler);
void SSP1_IRQHandler (void) ALIAS(IntDefaultHandler);
void I2C0_IRQHandler (void) ALIAS(IntDefaultHandler);
void TIMER16_0_IRQHandler (void) ALIAS(IntDefaultHandler);
void TIMER16_1_IRQHandler (void) ALIAS(IntDefaultHandler);
void TIMER32_0_IRQHandler (void) ALIAS(IntDefaultHandler);
void TIMER32_1_IRQHandler (void) ALIAS(IntDefaultHandler);
void SSP0_IRQHandler (void) ALIAS(IntDefaultHandler);
void USART0_IRQHandler (void) ALIAS(IntDefaultHandler);
void USB_IRQHandler (void) ALIAS(IntDefaultHandler);
void USB_FIQHandler (void) ALIAS(IntDefaultHandler);
void ADCA_IRQHandler (void) ALIAS(IntDefaultHandler);
void RTC_IRQHandler (void) ALIAS(IntDefaultHandler);
void BOD_WDT_IRQHandler (void) ALIAS(IntDefaultHandler);
void FMC_IRQHandler (void) ALIAS(IntDefaultHandler);
void DMA_IRQHandler (void) ALIAS(IntDefaultHandler);
void ADCB_IRQHandler (void) ALIAS(IntDefaultHandler);
void USBWakeup_IRQHandler (void) ALIAS(IntDefaultHandler);

__attribute__ ((section(".isr_vector")))
void (* const g_pfnVectors[])(void) = {
	// Core Level - CM0
    &_vStackTop,                     // The initial stack pointer
    ResetISR,                        // The reset handler
    NMI_Handler,                     // The NMI handler
    HardFault_Handler,               // The hard fault handler
    0,                               // Reserved
    0,                               // Reserved
    0,                               // Reserved
    0,                               // Checksum
    0,                               // Reserved
    0,                               // Reserved
    0,                               // Reserved
    SVC_Handler,                     // SVCall handler
    0,                               // Reserved
    0,                               // Reserved
    PendSV_Handler,                  // The PendSV handler
    SysTick_Handler,                 // The SysTick handler

    // Chip Level - LPC11U68
    PIN_INT0_IRQHandler,             //  0 - GPIO pin interrupt 0
    PIN_INT1_IRQHandler,             //  1 - GPIO pin interrupt 1
    PIN_INT2_IRQHandler,             //  2 - GPIO pin interrupt 2
    PIN_INT3_IRQHandler,             //  3 - GPIO pin interrupt 3
    PIN_INT4_IRQHandler,             //  4 - GPIO pin interrupt 4
    PIN_INT5_IRQHandler,             //  5 - GPIO pin interrupt 5
    PIN_INT6_IRQHandler,             //  6 - GPIO pin interrupt 6
    PIN_INT7_IRQHandler,             //  7 - GPIO pin interrupt 7
    GINT0_IRQHandler,                //  8 - GPIO GROUP0 interrupt
    GINT1_IRQHandler,                //  9 - GPIO GROUP1 interrupt
    I2C1_IRQHandler,                 // 10 - I2C1
    USART1_4_IRQHandler,             // 11 - combined USART1 & 4 interrupt
    USART2_3_IRQHandler,             // 12 - combined USART2 & 3 interrupt
    SCT0_1_IRQHandler,               // 13 - combined SCT0 and 1 interrupt
    SSP1_IRQHandler,                 // 14 - SPI/SSP1 Interrupt
    I2C0_IRQHandler,                 // 15 - I2C0
    TIMER16_0_IRQHandler,            // 16 - CT16B0 (16-bit Timer 0)
    TIMER16_1_IRQHandler,            // 17 - CT16B1 (16-bit Timer 1)
    TIMER32_0_IRQHandler,            // 18 - CT32B0 (32-bit Timer 0)
    TIMER32_1_IRQHandler,            // 19 - CT32B1 (32-bit Timer 1)
    SSP0_IRQHandler,                 // 20 - SPI/SSP0 Interrupt
    USART0_IRQHandler,               // 21 - USART0
    USB_IRQHandler,                  // 22 - USB IRQ
    USB_FIQHandler,                  // 23 - USB FIQ
    ADCA_IRQHandler,                 // 24 - ADC A(A/D Converter)
    RTC_IRQHandler,                  // 25 - Real Time CLock interrpt
    BOD_WDT_IRQHandler,              // 25 - Combined Brownout/Watchdog interrupt
    FMC_IRQHandler,                  // 27 - IP2111 Flash Memory Controller
    DMA_IRQHandler,                  // 28 - DMA interrupt
    ADCB_IRQHandler,                 // 24 - ADC B (A/D Converter)
    USBWakeup_IRQHandler,            // 30 - USB wake-up interrupt
    0,                               // 31 - Reserved
};
/* End Vector */

AFTER_VECTORS void data_init(unsigned int romstart, unsigned int start, unsigned int len) {
    unsigned int *pulDest = (unsigned int*) start;
    unsigned int *pulSrc = (unsigned int*) romstart;
    unsigned int loop;
    for (loop = 0; loop < len; loop = loop + 4) *pulDest++ = *pulSrc++;
}

AFTER_VECTORS void bss_init(unsigned int start, unsigned int len) {
    unsigned int *pulDest = (unsigned int*) start;
    unsigned int loop;
    for (loop = 0; loop < len; loop = loop + 4) *pulDest++ = 0;
}

/* Reset entry point*/
void software_init_hook(void) __attribute__((weak));

AFTER_VECTORS void ResetISR(void) {
    SystemInit();
    if( *cBtn ) __wrap_exit(0);

    unsigned int LoadAddr, ExeAddr, SectionLen;
    unsigned int *SectionTableAddr;

    SectionTableAddr = &__data_section_table;

    while (SectionTableAddr < &__data_section_table_end) {
        LoadAddr = *SectionTableAddr++;
        ExeAddr = *SectionTableAddr++;
        SectionLen = *SectionTableAddr++;
        data_init(LoadAddr, ExeAddr, SectionLen);
    }
    while (SectionTableAddr < &__bss_section_table_end) {
        ExeAddr = *SectionTableAddr++;
        SectionLen = *SectionTableAddr++;
        bss_init(ExeAddr, SectionLen);
    }

    __libc_init_array();
    __asm volatile ("cpsie i" : : : "memory");
    main();

    while( 1 ) ;
}

AFTER_VECTORS void NMI_Handler       (void) {}
AFTER_VECTORS void HardFault_Handler (void) {}
AFTER_VECTORS void MemManage_Handler (void) {}
AFTER_VECTORS void BusFault_Handler  (void) {}
AFTER_VECTORS void UsageFault_Handler(void) {}
AFTER_VECTORS void SVC_Handler       (void) {}
AFTER_VECTORS void DebugMon_Handler  (void) {}
AFTER_VECTORS void PendSV_Handler    (void) {}
AFTER_VECTORS void SysTick_Handler   (void) {}
AFTER_VECTORS void IntDefaultHandler (void) {}

//int __aeabi_atexit(void *object, void (*destructor)(void *), void *dso_handle) {return 0;}
