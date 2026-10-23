# SIGSEV — Ghost in the HAL

Proyecto ESP-IDF (v6.1, target `esp32s3`) basado en
[kiwi-board-example](https://github.com/SanFlan/kiwi-board-example). Tenemos dos
placas idénticas y cada una se flashea con uno de dos perfiles.

## Perfiles

| Perfil              | PSRAM            | Display OLED                     | Build dir       | sdkconfig           |
|---------------------|------------------|----------------------------------|-----------------|---------------------|
| `psram` (default)   | octal 8 MB       | no                               | `build`         | `sdkconfig`         |
| `display`           | no (~380 KB RAM) | sí                               | `build_display` | `sdkconfig.display` |

La PSRAM octal embebida usa internamente los GPIO 33–37, y el display usa 36/37:
son mutuamente excluyentes. Con PSRAM + display la placa entra en boot loop
(`TG1WDT_SYS_RST`). En el perfil `psram` los pines del LCD ni siquiera están
definidos (`main/board.h`) y las funciones `display_*` son no-ops.

`sdkconfig.defaults` es el perfil por defecto (psram), así que `idf.py build`
pelado también funciona. `sdkconfig.defaults.display` se aplica encima para el
perfil display.

## Build y flash

```sh
./flash.sh [psram|display] [puerto] [build|flash|monitor]

./flash.sh                                 # psram, /dev/ttyACM0, compilar + flashear
./flash.sh x build                         # psram, solo compilar (el puerto se ignora)
./flash.sh display /dev/ttyACM0            # perfil display
./flash.sh psram /dev/ttyACM1 monitor      # compilar + flashear + monitor serie
```

Si `idf.py` no está en el PATH, el script hace `. ~/Hackathon/esp-idf/export.sh`.

**Dos placas a la vez:** `/dev/ttyACM*` depende del orden de conexión. Usá el
path estable por MAC (`./flash.sh` sin argumentos lista las placas conectadas):

```sh
ls /dev/serial/by-id/
./flash.sh psram /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:45:6B:36:01:50-if00
```

Si no flashea: mantener RESET+BOOT, soltar RESET, soltar BOOT. Para salir del
monitor: `Ctrl+T` y después `Ctrl+X`.

Para cambiar opciones de un perfil: `idf.py menuconfig` (psram) o
`idf.py -B build_display -D SDKCONFIG=sdkconfig.display menuconfig`
(menú *Hackathon board demo*).

## Opciones del proyecto (Kconfig)

- `HACK_SPEAKER_ENABLE` (default **off**): inicializa el parlante I2S1. Apagado,
  `audio_play_tone()` devuelve `ESP_ERR_NOT_SUPPORTED`. El micrófono anda igual.
- `HACK_SPEAKER_VOLUME`: volumen del parlante (solo si está habilitado).
- `HACK_DISPLAY_ROTATE_180`: rota el OLED.

## Pines

| Periférico             | Señal                                   | GPIO                 |
|------------------------|-----------------------------------------|----------------------|
| Display SSD1306 (SPI)  | SCL / SDA / CS / DC / RST               | 36 / 37 / 38 / 39 / 40 (solo perfil display) |
| Micrófono PDM (I2S0)   | CLK / DOUT                              | 7 / 15               |
| Parlante (I2S1)        | DIN / BCLK / LRCLK                      | 4 / 5 / 6 (no usado) |
| Joystick               | UP / DOWN / LEFT / RIGHT / CENTER       | 9 / 11 / 12 / 10 / 13 (CENTER no conectado) |
| Botones                | KEY1 / KEY2 / KEY3 / KEY4               | 14 / 21 / 47 / 48    |
| Header I²C (placa↔placa) | SDA / SCL                             | 1 / 2                |
| PSRAM octal (interna)  | —                                       | 33–37 (perfil psram) |
