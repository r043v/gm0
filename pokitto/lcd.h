
#define   __I     volatile const       /*!< Defines 'read only' permissions                 */
#define     __O     volatile             /*!< Defines 'write only' permissions                */
#define     __IO    volatile             /*!< Defines 'read / write' permissions              */

typedef struct {                                    /*!< GPIO_PORT Structure                                                   */
  __IO uint8_t   B[88];                             /*!< Byte pin registers                                                    */
  __I  uint32_t  RESERVED0[42];
  __IO uint32_t  W[88];                             /*!< Word pin registers                                                    */
  __I  uint32_t  RESERVED1[1896];
  __IO uint32_t  DIR[3];                            /*!< Port Direction registers                                              */
  __I  uint32_t  RESERVED2[29];
  __IO uint32_t  MASK[3];                           /*!< Port Mask register                                                    */
  __I  uint32_t  RESERVED3[29];
  __IO uint32_t  PIN[3];                            /*!< Port pin register                                                     */
  __I  uint32_t  RESERVED4[29];
  __IO uint32_t  MPIN[3];                           /*!< Masked port register                                                  */
  __I  uint32_t  RESERVED5[29];
  __IO uint32_t  SET[3];                            /*!< Write: Set port register Read: port output bits                       */
  __I  uint32_t  RESERVED6[29];
  __O  uint32_t  CLR[3];                            /*!< Clear port                                                            */
  __I  uint32_t  RESERVED7[29];
  __O  uint32_t  NOT[3];                            /*!< Toggle port                                                           */
} zLPC_GPIO_PORT_Type;

#undef __I
#undef __O
#undef __IO

#define LPC_GPIO_PORT_BASE              0xA0000000UL
#define LPC_GPIO_PORT                   ((LPC_GPIO_PORT_Type      *) LPC_GPIO_PORT_BASE)


#define CLR_RESET LPC_GPIO_PORT->CLR[LCD_RES_PORT] = 1 << LCD_RES_PIN; //RST = (0); // Clear pin
#define SET_RESET LPC_GPIO_PORT->SET[LCD_RES_PORT] = 1 << LCD_RES_PIN; // RST = (1); // Set pin

#define CLR_CD { LPC_GPIO_PORT->CLR[LCD_CD_PORT] = 1 << LCD_CD_PIN; } // RS = (0); // Clear pin
#define SET_CD { LPC_GPIO_PORT->SET[LCD_CD_PORT] = 1 << LCD_CD_PIN; }// RS = (1); // Set pin

#define CLR_WR { LPC_GPIO_PORT->CLR[LCD_WR_PORT] = 1 << LCD_WR_PIN;__asm("nop");}//__asm("nop");}//WR = (0); // Clear pin
#define CLR_WR_SLOW { LPC_GPIO_PORT->CLR[LCD_WR_PORT] = 1 << LCD_WR_PIN;__asm("nop");__asm("nop");}//WR = (0); // Clear pin
#define SET_WR LPC_GPIO_PORT->SET[LCD_WR_PORT] = 1 << LCD_WR_PIN; //WR = (1); // Set pin

#define CLR_RD LPC_GPIO_PORT->CLR[LCD_RD_PORT] = 1 << LCD_RD_PIN; //RD = (0); // Clear pin
#define SET_RD LPC_GPIO_PORT->SET[LCD_RD_PORT] = 1 << LCD_RD_PIN; //RD = (1); // Set pin

#define SET_CS  //CS tied to ground
#define CLR_CS

#define CLR_CS_CD_SET_RD_WR {CLR_CD; SET_RD; SET_WR;}
#define CLR_CS_SET_CD_RD_WR {SET_CD; SET_RD; SET_WR;}
#define SET_CD_RD_WR {SET_CD; SET_RD; SET_WR;}
#define SET_WR_CS SET_WR;

#define SET_MASK_P2 LPC_GPIO_PORT->MASK[2] = ~(0x7FFF8); //mask P2_3 ...P2_18
#define CLR_MASK_P2 LPC_GPIO_PORT->MASK[2] = 0; // all on

#define POK_BOARDREV 2

#if POK_BOARDREV == 1
/** 2-layer board version 1.3 **/
 #define LCD_CD_PORT           0
 #define LCD_CD_PIN            2
 #define LCD_WR_PORT           1
 #define LCD_WR_PIN            23
 #define LCD_RD_PORT           1
 #define LCD_RD_PIN            24
 #define LCD_RES_PORT          1
 #define LCD_RES_PIN           28
#else
/** 4-layer board version 2.1 **/
 #define LCD_CD_PORT           0
 #define LCD_CD_PIN            2
 #define LCD_WR_PORT           1
 #define LCD_WR_PIN            12
 #define LCD_RD_PORT           1
 #define LCD_RD_PIN            24
 #define LCD_RES_PORT          1
 #define LCD_RES_PIN           0
#endif
