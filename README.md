# SIGSEV — red mesh para emergencias y marchas

Proyecto para la hackatón **Ghost in the HAL** (ComCom, FCEyN-UBA). Placas
ESP32-S3 que forman una **red mesh peer-to-peer sin infraestructura**: sin
router, sin datos móviles y sin cuentas. Sirve para una marcha donde la red
celular colapsa, un corte de luz o un lugar sin señal.

- **Alertas rápidas** (SOS, MÉDICO, PELIGRO, ESTOY BIEN) con un botón de la placa o desde el celular.
- **Mensajes de voz** push-to-talk: se graban en la placa y viajan comprimidos por la mesh.
- **Texto libre** desde el celular.
- **Mensajes privados** (texto o voz) a un nodo puntual, aunque no esté en la red:
  los demás nodos lo guardan y se lo pasan cuando aparece (store-and-forward).
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
| **KEY4** (la apartada) mantener | Grabar voz (hasta 20 s). Al soltar se manda a toda la red, o en privado al nodo marcado en la pantalla Red |
| Joystick ← → | Cambiar pantalla: Inicio / Mensajes / Red |
| Joystick ↑ ↓ | Scroll; en Red mueve el cursor (Todos o un nodo); cierra el aviso de alerta recibida |

La pantalla **Red** muestra el ID propio en el título y lista el historial de
nodos conocidos (los que no están, con `--` y hace cuánto se los vio). Los
privados aparecen con `@` en Mensajes; los propios dicen `a Nombre(ok)` cuando
llegaron.

KEY1–3 son los tres botones en fila de abajo; KEY4 es el que está apartado arriba
a la derecha. La pantalla de inicio los dibuja en esa misma posición y los
rellena mientras están apretados. Las alertas piden mantener apretado para que
un toque sin querer no dispare nada.

### En el celular

1. Conectarse al WiFi abierto `SIGSEV-XXXX` (cada nodo publica el suyo).
2. El portal se abre solo; si no, entrar a `http://192.168.4.1/`.
3. Pestañas:
   - **Alertas**: botones de alerta (mantener apretado), mensajes de texto y feed
     con reproducción de voz. En **Para** se elige *Todos*, un nodo del historial
     o *Otro ID…* (las 8 cifras, o las últimas si alcanzan para identificarlo).
     Los privados propios muestran *en camino…* hasta que vuelve el ✓.
   - **Red**: mapa de nodos coloreados por ruido, con enlaces más gruesos cuanto mejor es la señal.
     La lista incluye los nodos del historial que no están, con ✉ para escribirles.
   - **Nodo**: renombrar el nodo (ej. "Puesto médico") y estadísticas de tráfico.

La placa no tiene parlante: las voces se escuchan en el celular. Las voces
privadas salen de la placa: el celular no puede grabar porque el navegador solo
da el micrófono en páginas HTTPS, y el portal es HTTP plano.

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
  messages/      alertas, texto y voz + store (PSRAM si hay) + courier de privados
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
| `TEXT` | origen, ref, destino y hasta 160 bytes UTF-8 |
| `VOICE` | origen, ref, destino y un bloque de 55 ms de IMA ADPCM a 8 kHz, con su propio estado del códec |
| `ACK` | origen y ref del privado que llegó |

Como cada bloque de voz se puede decodificar solo, un paquete perdido es un
silencio de 55 ms y no corta el resto del mensaje. Un mensaje de 20 s son ~360
paquetes, unos 80 KB.

#### Privados: store-and-forward

Un privado (destino ≠ 0) viaja igual que un mensaje para todos, pero solo lo
muestran el destino y quien lo mandó; el resto de los nodos lo guarda **en
custodia**. Texto y voz llevan su identidad `(origen, ref)` en el payload, así
un custodio puede reenviarlo tal cual y nadie lo muestra dos veces.

- Cuando un nodo con privados pendientes ve aparecer al destino (su beacon de
  presencia), espera entre 0,5 y 3 s al azar y lo reenvía. Si en ese tiempo
  escucha que otro ya lo reenvió, no lo hace: así no lo mandan todos a la vez.
- Mientras el destino siga en la red y no confirme, se reintenta cada ~20 s,
  hasta 5 veces por aparición.
- El destino contesta con un `ACK` que se inunda por la red: el que lo escucha
  marca el mensaje como entregado y deja de insistir. Si el origen no estaba,
  el ACK se le reenvía cuando aparece.
- El historial de nodos (id y último nombre) se guarda en NVS, así se le puede
  escribir a un nodo que no está aunque la placa se haya reiniciado.

Ojo: **privado = dirigido, no cifrado.** Cualquier nodo de la red puede leer
el contenido; solo no lo muestra.

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
