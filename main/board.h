#pragma once

#include "driver/gpio.h"
#include "sdkconfig.h"

#define BOARD_SPK_DIN_IO        GPIO_NUM_4
#define BOARD_SPK_BCLK_IO       GPIO_NUM_5
#define BOARD_SPK_LRCLK_IO      GPIO_NUM_6

#define BOARD_MIC_CLK_IO        GPIO_NUM_7
#define BOARD_MIC_DOUT_IO       GPIO_NUM_15

// La PSRAM octal usa internamente los GPIO 33-37. Con los pines originales del
// display (SCL=36, SDA=37) son incompatibles: en ese caso los pines del LCD no se
// definen y cualquier codigo que los use no compila. Si se recablean SCL/SDA fuera
// de 33-37 (HACK_LCD_SCL_IO / HACK_LCD_SDA_IO), display y PSRAM conviven.
#define BOARD_PIN_IS_PSRAM(io)  ((io) >= 33 && (io) <= 37)
#if CONFIG_SPIRAM && (BOARD_PIN_IS_PSRAM(CONFIG_HACK_LCD_SCL_IO) || BOARD_PIN_IS_PSRAM(CONFIG_HACK_LCD_SDA_IO))
#define BOARD_HAS_DISPLAY       0
#else
#define BOARD_HAS_DISPLAY       1
#define BOARD_LCD_SCL_IO        CONFIG_HACK_LCD_SCL_IO
#define BOARD_LCD_SDA_IO        CONFIG_HACK_LCD_SDA_IO
#define BOARD_LCD_RST_IO        GPIO_NUM_40
#define BOARD_LCD_DC_IO         GPIO_NUM_39
#define BOARD_LCD_CS_IO         GPIO_NUM_38
#endif
#define BOARD_LCD_WIDTH         128
#define BOARD_LCD_HEIGHT        64

// Esto esta mal pq habia que rotar 45 grados el joystick asi que bueno...
// Hagan una rotacion ustedes confio en que pueden.
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
