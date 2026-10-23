# SIGSEV — Ghost in the HAL

Proyecto ESP-IDF (v6.1, target `esp32s3`) basado en
[kiwi-board-example](https://github.com/SanFlan/kiwi-board-example). Tenemos dos
placas idénticas y cada una se flashea con uno de dos perfiles.

## Perfiles

| Perfil              | PSRAM            | Display OLED                     | Build dir       | sdkconfig           |
|---------------------|------------------|----------------------------------|-----------------|---------------------|
| `psram` (default)   | octal 8 MB       | no (sí si se recablea SCL/SDA)   | `build`         | `sdkconfig`         |
| `display`           | no (~380 KB RAM) | sí, pines originales             | `build_display` | `sdkconfig.display` |

La PSRAM octal embebida usa internamente los GPIO 33–37, y el display usa 36/37:
con el cableado original son mutuamente excluyentes. Con PSRAM + display la placa
entra en boot loop (`TG1WDT_SYS_RST`). Mientras SCL/SDA estén en 33–37 y la PSRAM
activa, los pines del LCD ni siquiera se definen (`main/board.h`) y las
funciones `display_*` son no-ops.

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
- `HACK_LCD_SCL_IO` / `HACK_LCD_SDA_IO` (default 36 / 37): pines SPI del OLED.
  Si los dos quedan fuera de 33–37, el display funciona también con PSRAM.
- `HACK_DISPLAY_ROTATE_180`: rota el OLED.

## PSRAM + display (recablear SCL/SDA)

1. Cortar las pistas de GPIO 36 y 37 hacia el OLED (lo más cerca del conector
   del OLED), para que el display no quede colgado del bus de la PSRAM.
2. Soldar dos cables desde SCL y SDA del OLED a dos GPIO libres. Candidatos:
   - **4 / 5** (pines del parlante, que no usamos; seguramente tienen pad o
     conector accesible).
   - 8, 16, 17, 18 (libres, pero hay que encontrar dónde soldar).
   - Evitar 0, 3, 45, 46 (strapping), 19/20 (USB), 26–37 (flash/PSRAM).
3. `idf.py menuconfig` → *Hackathon board demo* → cambiar `HACK_LCD_SCL_IO` y
   `HACK_LCD_SDA_IO`. Si se usan 4/5, dejar `HACK_SPEAKER_ENABLE` apagado.
4. `./flash.sh`. El log tiene que mostrar `SSD1306 128x64 ready` y
   `Found 8MB PSRAM device`.

Una vez cortadas las pistas, el perfil `display` también necesita los pines
nuevos (cambiarlos en su menuconfig).

## Pines

| Periférico             | Señal                                   | GPIO                 |
|------------------------|-----------------------------------------|----------------------|
| Display SSD1306 (SPI)  | SCL / SDA / CS / DC / RST               | 36 / 37 / 38 / 39 / 40 (SCL/SDA configurables) |
| Micrófono PDM (I2S0)   | CLK / DOUT                              | 7 / 15               |
| Parlante (I2S1)        | DIN / BCLK / LRCLK                      | 4 / 5 / 6 (no usado) |
| Joystick               | UP / DOWN / LEFT / RIGHT / CENTER       | 9 / 11 / 12 / 10 / 13 (CENTER no conectado) |
| Botones                | KEY1 / KEY2 / KEY3 / KEY4               | 14 / 21 / 47 / 48    |
| Header I²C (placa↔placa) | SDA / SCL                             | 1 / 2                |
| PSRAM octal (interna)  | —                                       | 33–37 (perfil psram) |
