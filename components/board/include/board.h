#pragma once

#include "driver/gpio.h"
#include "sdkconfig.h"

// Parlante: los pines salen de menuconfig (SIGSEV: hardware), por defecto 4/5/6.
#if CONFIG_HACK_SPEAKER_ENABLE
#define BOARD_SPK_DIN_IO        ((gpio_num_t)CONFIG_HACK_SPEAKER_DIN_GPIO)
#define BOARD_SPK_BCLK_IO       ((gpio_num_t)CONFIG_HACK_SPEAKER_BCLK_GPIO)
#define BOARD_SPK_LRCLK_IO      ((gpio_num_t)CONFIG_HACK_SPEAKER_LRCLK_GPIO)
#endif

#define BOARD_MIC_CLK_IO        GPIO_NUM_7
#define BOARD_MIC_DOUT_IO       GPIO_NUM_15

// La PSRAM octal usa internamente los GPIO 33-37, que pisan SCL/SDA del display.
// Con PSRAM los pines del LCD no se definen: cualquier codigo que los use no compila.
#if CONFIG_SPIRAM
#define BOARD_HAS_DISPLAY       0
#else
#define BOARD_HAS_DISPLAY       1
#define BOARD_LCD_SCL_IO        GPIO_NUM_36
#define BOARD_LCD_SDA_IO        GPIO_NUM_37
#define BOARD_LCD_RST_IO        GPIO_NUM_40
#define BOARD_LCD_DC_IO         GPIO_NUM_39
#define BOARD_LCD_CS_IO         GPIO_NUM_38
#endif
#define BOARD_LCD_WIDTH         128
#define BOARD_LCD_HEIGHT        64

// Contactos crudos del joystick. El joystick esta montado rotado 45 grados, asi
// que cada contacto queda en una diagonal fisica: buttons.c lo compensa.
#define BOARD_KEY_UP_IO         GPIO_NUM_9
#define BOARD_KEY_DOWN_IO       GPIO_NUM_11
#define BOARD_KEY_LEFT_IO       GPIO_NUM_12
#define BOARD_KEY_RIGHT_IO      GPIO_NUM_10
// Center no esta conectado, me di cuenta tarde.
// Podemos conectarlo con un cablecito a algun pin si lo ven necesario
#define BOARD_KEY_CENTER_IO     GPIO_NUM_13
#define BOARD_KEY_1_IO          GPIO_NUM_14
#define BOARD_KEY_2_IO          GPIO_NUM_21
#define BOARD_KEY_3_IO          GPIO_NUM_47
#define BOARD_KEY_4_IO          GPIO_NUM_48

#define BOARD_I2C_SDA_IO        GPIO_NUM_1
#define BOARD_I2C_SCL_IO        GPIO_NUM_2
