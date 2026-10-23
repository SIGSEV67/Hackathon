# SIGSEV — red mesh para emergencias y marchas

Proyecto para la hackatón **Ghost in the HAL** (ComCom, FCEyN-UBA). Placas
ESP32-S3 que forman una **red mesh peer-to-peer sin infraestructura**: sin
router, sin datos móviles y sin cuentas. Sirve para una marcha donde la red
celular colapsa, un corte de luz o un lugar sin señal.

- **Alertas rápidas** (SOS, MÉDICO, PELIGRO, ESTOY BIEN) con un botón de la placa o desde el celular.
- **Mensajes de voz** push-to-talk: se graban en la placa y viajan comprimidos por la mesh.
- **Texto libre** desde el celular.
- **Mapa de la red y del ruido**: qué nodos hay, cómo están enlazados y cuánto ruido mide cada uno.

Cada placa reenvía lo que escucha, así que si un nodo se cae los mensajes siguen
por los demás.

## ¿Por qué no LoRa?

LoRa (Meshtastic, por ejemplo) llega a kilómetros con muy poco consumo y eso no lo discutimos.
Nuestra apuesta está en lo que permite una radio con ~500× más ancho de banda.

| | LoRa / Meshtastic | SIGSEV (ESP-NOW + WiFi) |
|---|---|---|
| Alcance por salto | km | ~100–200 m |
| Ancho de banda | ~1–20 kbps | ~1 Mbps |
| Latencia | segundos | milisegundos |
| Qué viaja | texto corto | **voz**, alertas, telemetría (ruido, topología) |
| Celular | app propia + Bluetooth | **cualquier navegador**, sin instalar nada |
| Hardware extra | módulo LoRa | ninguno |

## Uso

### En la placa

| Control | Acción |
|---|---|
| **KEY1** mantener 1 s | Alerta **SOS** |
| **KEY2** mantener 1 s | Alerta **MÉDICO** |
| **KEY3** mantener 1 s | Alerta **PELIGRO** |
| **KEY4** (la apartada) mantener | Grabar voz (hasta 20 s). Al soltar se manda a toda la red |
| Joystick ← → | Cambiar pantalla: Inicio / Mensajes / Red |
| Joystick ↑ ↓ | Scroll; cierra el aviso de alerta recibida |

KEY1–3 son los tres botones en fila de abajo; KEY4 es el que está apartado arriba
a la derecha. La pantalla de inicio los dibuja en esa misma posición y los
rellena mientras están apretados. Las alertas piden mantener apretado para que
un toque sin querer no dispare nada.

### En el celular

1. Conectarse al WiFi abierto `SIGSEV-XXXX` (cada nodo publica el suyo).
2. El portal se abre solo; si no, entrar a `http://192.168.4.1/`.
3. Pestañas:
   - **Alertas**: botones de alerta (mantener apretado), mensajes de texto y feed
     con reproducción de voz.
   - **Red**: mapa de nodos coloreados por ruido, con enlaces más gruesos cuanto mejor es la señal.
   - **Nodo**: renombrar el nodo (ej. "Puesto médico") y estadísticas de tráfico.

La placa no tiene parlante: las voces se escuchan en el celular.

## Arquitectura

```
main/            arma las piezas (main.c) y mapea botones a acciones (controls.c)
components/
  board/         pines y perfil de hardware (BOARD_HAS_DISPLAY)
  periph/        drivers: display, mic (con suscriptores), botones (esp_event)
  adpcm/         códec IMA ADPCM, C puro (tiene test en test/host)
  noise/         nivel de ruido sobre el mic
  radio/         WiFi softAP en canal fijo (lo comparten celulares y ESP-NOW)
  link/          interfaz de transporte de un salto + backend ESP-NOW
  mesh/          flooding con TTL y dedup, vecinos con RSSI, handlers por tipo
  presence/      beacons: directorio de nodos, nombres, ruido, topología
  messages/      alertas, texto y voz + store (PSRAM si hay)
  web/           DNS cautivo, servidor HTTP, API JSON y app web embebida
  ui/            pantallas del OLED (o log en el perfil sin display)
```

Las dependencias van en una sola dirección y cada componente las declara en su
`CMakeLists.txt`, así el build no deja que una capa use otra que no le toca:

```
main ─► ui, web, controls ─► messages, presence ─► mesh ─► link
                                   │                         ▲
                                   └─► periph, adpcm      radio (WiFi)
```

Patrones de diseño:

- **Interfaz con backend inyectado.** `mesh_init(link_espnow())`: la mesh solo
  conoce `link_t`, así que se puede sumar un backend I²C para placas cableadas sin tocarla.
- **Observer.** El mic reparte PCM a sus suscriptores (`audio_mic_add_sink`), que
  hoy son el medidor de ruido y la grabación. La mesh despacha cada paquete al handler registrado para su tipo.
- **Bus de eventos (`esp_event`).** Botones, mensajes y presencia publican eventos,
  y la UI y los controles los consumen sin conocer a quién los produce.
- **Inyección de dependencias.** `presence` recibe el nivel de ruido por callbacks
  (`presence_config_t`) y no depende del mic.
- **Separación vista/controlador.** La UI solo muestra y navega; qué hace cada
  botón lo decide `main/controls.c`.

### Protocolo

Todo es broadcast ESP-NOW (≤ 250 B por frame) con un header de 12 B: magic,
versión, tipo, TTL, saltos, seq y origen. Cada nodo reenvía una sola vez cada
`(origen, seq)` nuevo, con un jitter aleatorio para que los reenvíos no choquen.

| Tipo | Payload |
|---|---|
| `PRESENCE` | nombre, capacidades, ruido, uptime, vecinos con RSSI (cada ~3 s) |
| `ALERT` | tipo de alerta |
| `TEXT` | hasta 160 bytes UTF-8 |
| `VOICE` | un bloque de 55 ms de IMA ADPCM a 8 kHz, con su propio estado del códec |

Como cada bloque de voz se puede decodificar solo, un paquete perdido es un
silencio de 55 ms y no corta el resto del mensaje. Un mensaje de 20 s son ~360
paquetes, unos 80 KB.

Todos los nodos tienen que usar el mismo canal WiFi (`menuconfig` → *SIGSEV: radio*).

## Perfiles de build

| Perfil              | PSRAM            | Display OLED | Build dir       | sdkconfig           |
|---------------------|------------------|--------------|-----------------|---------------------|
| `psram` (default)   | octal 8 MB       | no           | `build`         | `sdkconfig`         |
| `display`           | no (~380 KB RAM) | sí           | `build_display` | `sdkconfig.display` |

La PSRAM octal embebida usa internamente los GPIO 33–37, y el display usa 36/37,
así que son excluyentes. En el perfil `psram` los pines del LCD ni siquiera están
definidos (`components/board/include/board.h`) y el display es un no-op. Con
PSRAM entran unos 15 minutos de voz guardada, contra unos 20 s sin ella.

```sh
./flash.sh [psram|display] [puerto] [build|flash|monitor]

./flash.sh                                 # psram, /dev/ttyACM0, compilar + flashear
./flash.sh x build                         # solo compilar (el puerto se ignora)
./flash.sh display /dev/ttyACM0 monitor    # perfil display + monitor serie
```

Si `idf.py` no está en el PATH, el script hace `. ~/Hackathon/esp-idf/export.sh`.
Con varias placas conectadas conviene usar `/dev/serial/by-id/...`
(`./flash.sh -h` las lista). Si no flashea: mantener RESET+BOOT, soltar RESET,
soltar BOOT. Para salir del monitor: `Ctrl+T` y después `Ctrl+X`.

Test del códec en la PC:

```sh
cc -I components/adpcm/include test/host/test_adpcm.c components/adpcm/adpcm.c -lm -o /tmp/t && /tmp/t
```

## Opciones (menuconfig)

- *SIGSEV: radio*: canal WiFi y máximo de celulares por nodo.
- *SIGSEV: mesh*: TTL, jitter de reenvío y timeout de vecinos.
- *SIGSEV: messages*: duración máxima de voz, memoria para voces y ganancia máxima.
- *SIGSEV: hardware (periph)*: parlante (apagado: no tenemos) y rotación del display.

## Joystick

El joystick está montado rotado: cada contacto queda en una diagonal física
(empujar arriba cierra los contactos RIGHT y DOWN). `components/periph/buttons.c`
lo compensa con la tabla `s_joy_dir`, así `BTN_UP/DOWN/LEFT/RIGHT` reportan la
dirección física. Una diagonal se reporta como dos botones. Está verificado en
placa. Los GPIO de `board.h` son los contactos crudos, no las direcciones.

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
