# 02 · Arquitectura

## Plataforma

| | |
|---|---|
| Microcontrolador | ESP32-S3 (dos núcleos Xtensa LX7 a 240 MHz, WiFi 2,4 GHz + BLE) |
| RAM | ~512 KB internos; módulo con 8 MB de PSRAM octal (solo en el perfil `psram`) |
| Flash | 16 MB; partición de app de 4 MB (`partitions.csv`) |
| SDK | ESP-IDF v6.1 (C, sobre FreeRTOS) |
| Periféricos usados | OLED SSD1306 por SPI, mic PDM (I2S0), parlante I2S (I2S1, opcional), joystick y 4 teclas |

ESP-IDF organiza el código en **componentes**: cada carpeta de `components/`
tiene su `CMakeLists.txt`, que declara qué otros componentes usa. Si un archivo
incluye un header de un componente que no declaró, **no compila**. Eso convierte
la arquitectura en una regla que verifica el build, no en una convención.

- `REQUIRES` = la dependencia aparece en la interfaz pública (headers de `include/`).
- `PRIV_REQUIRES` = se usa solo adentro (los `.c`). Quien use el componente no la hereda.

## Capas

```mermaid
flowchart TD
    main["main<br/>main.c (arranque) · controls.c (teclas → acciones)"]
    ui["ui<br/>pantallas del OLED"]
    web["web<br/>portal, API, app"]
    messages["messages<br/>alertas, texto, voz, archivos,<br/>store, courier, repair"]
    call["call<br/>llamadas walkie"]
    presence["presence<br/>directorio de placas y celus"]
    ranging["ranging<br/>distancia FTM / RSSI"]
    noise["noise<br/>nivel de ruido"]
    mesh["mesh<br/>flooding, dedup, vecinos"]
    link["link<br/>transporte de un salto (ESP-NOW)"]
    radio["radio<br/>WiFi AP(+STA)"]
    periph["periph<br/>display, mic, parlante, botones"]
    adpcm["adpcm<br/>códec (C puro)"]
    board["board<br/>pines"]

    main --> ui & web & messages & call & presence & ranging & noise & radio
    ui --> messages & call & presence & periph
    web --> messages & presence & periph
    messages --> presence & mesh & periph & adpcm
    call --> presence & mesh & periph & adpcm
    presence --> mesh
    ranging --> mesh
    noise --> periph
    mesh --> link
    periph --> board
```

Las flechas son las dependencias declaradas en los `CMakeLists.txt`. Van todas
hacia abajo: **ningún componente de abajo conoce a uno de arriba.** Cuando uno
de abajo necesita algo de arriba se resuelve con *inyección* (callbacks que
pasa `main`) o con *eventos* (ver patrones).

| Componente | Responsabilidad | Archivos clave |
|---|---|---|
| `board` | Pines y qué hardware hay en cada perfil (`BOARD_HAS_DISPLAY`). Solo un header. | `include/board.h` |
| `periph` | Drivers: display SSD1306, mic PDM con suscriptores, parlante, botones con eventos. | `display.c`, `audio.c`, `buttons.c` |
| `adpcm` | Códec IMA ADPCM, sin dependencias de ESP-IDF (se testea en la PC). | `adpcm.c`, `test/host/test_adpcm.c` |
| `noise` | Nivel de ruido (0–100) a partir del mic. | `noise.c` |
| `radio` | Levanta el WiFi: AP abierto para los celus, STA ociosa para FTM. | `radio.c` |
| `link` | Interfaz "mandar un frame a los vecinos" + backend ESP-NOW. | `include/link.h`, `link_espnow.c` |
| `mesh` | Red de un solo nivel: header, flooding con TTL, deduplicación, tabla de vecinos, despacho por tipo. | `mesh.c`, `dedup.c`, `neighbors.c` |
| `presence` | Beacons, directorio de placas y celus, historial en NVS, nombres. | `presence.c` |
| `ranging` | Distancia a cada vecino directo. | `ranging.c` |
| `messages` | Todo lo que es un "mensaje": modelo, almacenamiento, envío, recepción, custodia, reparación, reproducción. | ver [05](05-mensajes.md) |
| `call` | Llamadas en vivo entre placas. | `call.c` |
| `web` | DNS cautivo, servidor HTTP, API JSON, app embebida. | `web.c`, `dns.c`, `api.c`, `www/` |
| `ui` | Pantallas del OLED (o un log periódico si no hay pantalla). | `ui.c`, `screens.c` |
| `main` | Arranque en orden y el mapa de teclas. | `main.c`, `controls.c` |

### ¿Por qué `periph` y no `hal`?

ESP-IDF ya tiene un componente llamado `hal`; uno propio con el mismo nombre
choca en el build. Por eso los drivers propios se llaman `periph`.

## Patrones de diseño

### 1. Interfaz con backend inyectado (puertos y adaptadores)

La mesh no sabe qué es ESP-NOW. Conoce una interfaz mínima (`components/link/include/link.h`):

```c
typedef struct link {
    const char *name;
    size_t mtu;
    esp_err_t (*start)(link_rx_cb_t on_rx, void *ctx);
    esp_err_t (*broadcast)(const uint8_t *data, size_t len);
    void (*self_addr)(uint8_t out[LINK_ADDR_LEN]);
} link_t;
```

`main.c` la conecta con `mesh_init(link_espnow())`. **Por qué:** el transporte
es lo más dependiente del hardware y lo que más probablemente cambie (otra
radio, un cable I²C entre placas, ESP-NOW v2). Con la interfaz, esos cambios
no tocan la lógica de la mesh. Es el patrón *Strategy* / *ports and adapters*
hecho en C con una tabla de punteros a función.

### 2. Registro de handlers por tipo de paquete

Cada módulo registra quién atiende su tipo (`mesh_register_handler(MESH_TYPE_TEXT, on_text, NULL)`).
La mesh no interpreta payloads: despacha. Los tipos viven en **un solo enum**
(`mesh_type_t` en `mesh.h`) para que dos módulos no elijan el mismo número sin
darse cuenta; registrar dos veces el mismo tipo devuelve error.

**Por qué:** agregar una función nueva (las llamadas, la lista de clientes) es
agregar un tipo y un handler, sin tocar la mesh.

### 3. Observer: el micrófono reparte a suscriptores

Hay un solo micrófono y tres interesados: el medidor de ruido, la grabación de
voces y las llamadas. `audio_mic_add_sink(fn, ctx)` registra callbacks; el task
del mic lee del I2S y llama a todos. Cada consumidor decide adentro si le
interesa ese audio (la grabación solo lo usa mientras se graba).

**Por qué:** el mic no conoce a sus consumidores y cada uno se agrega sin tocar el driver.

### 4. Bus de eventos (`esp_event`)

Los productores publican, los consumidores se suscriben, y no se conocen:

| Base | Eventos | Lo publica | Lo escuchan |
|---|---|---|---|
| `BUTTONS_EVENT` | `PRESSED`, `RELEASED` | `periph/buttons.c` | `main/controls.c`, `ui/ui.c` |
| `MESSAGES_EVENT` | `NEW`, `UPDATED`, `DELIVERED`, `REC_STARTED`, `REC_STOPPED`, `PLAY_STOPPED` | `messages` | `ui` |
| `PRESENCE_EVENT` | `NODE_JOINED` | `presence` | `messages/courier.c` |
| `CALL_EVENT` | `CHANGED` | `call` | `ui` |

**Por qué:** así `messages` (abajo) le avisa a `ui` (arriba) sin depender de
ella, y el courier se entera de que apareció alguien sin que `presence` sepa
que existe un courier.

### 5. Inyección de dependencias por configuración

`presence` necesita el nivel de ruido y la distancia a cada vecino para ponerlos
en el beacon, pero no debería depender del mic ni del módulo de distancias.
`main.c` le pasa callbacks:

```c
const presence_config_t presence_cfg = {
    .flags = ...,
    .noise_level = noise_level,
    .noise_take_peak = noise_take_peak,
    .link_distance = link_distance,   // adaptador hacia ranging_get()
};
```

`link_distance` es un adaptador de 10 líneas en `main.c`: traduce el tipo de
`ranging` al de `presence` para que ninguno conozca al otro.

### 6. Fachada con internos privados

`messages` tiene un header público (`include/messages.h`) y uno interno
(`internal.h`) con lo que se comparten sus archivos (`courier_ack`,
`sender_post`…). Desde afuera solo se ve la fachada. El `store` tampoco se
expone: nadie fuera de `messages` toca el almacenamiento.

## Concurrencia: los tasks

FreeRTOS corre *tasks* (hilos livianos con stack propio, sin memoria
protegida). Prioridad más alta = corre primero. Estos son los del proyecto:

| Task | Prio | Stack | Qué hace | Archivo |
|---|---|---|---|---|
| `buttons` | 10 | 3 KB | Recibe la interrupción, hace debounce, publica eventos | `periph/buttons.c` |
| `mesh_tx` | 9 | 3 KB | Saca frames de la cola de salida y los manda al aire | `mesh/mesh.c` |
| `mesh_rx` | 8 | 4 KB | Procesa frames recibidos: dedup, reenvío, handler | `mesh/mesh.c` |
| `call_play` | 7 | 3 KB | Audio de la llamada al parlante (jitter buffer) | `call/call.c` |
| `call` | 6 | 3,5 KB | Señalización de la llamada, envío del audio, tonos | `call/call.c` |
| `player` | 6 | 3 KB | Reproduce voces guardadas | `messages/player.c` |
| `mic` | 5 | 4 KB | Lee el mic y reparte a los suscriptores | `periph/audio.c` |
| `voice` | 5 | 4 KB | Cierra grabaciones y manda voces | `messages/voice.c` |
| `sender` | 5 | 3 KB | Manda rangos de bloques (archivos, reenvíos, reparaciones) | `messages/sender.c` |
| `httpd` | 5 | 6 KB | Servidor web (uno solo atiende todos los pedidos) | `web/web.c` |
| `courier` | 4 | 4 KB | Reenvía privados pendientes | `messages/courier.c` |
| `presence` | 4 | 3 KB | Manda el beacon y la lista de celus | `presence/presence.c` |
| `repair` | 3 | 3 KB | Detecta mensajes trabados y pide lo que falta | `messages/repair.c` |
| `ui` | 3 | 4 KB | Dibuja la pantalla a 10 fps | `ui/ui.c` |
| `dns` | 3 | 3 KB | DNS cautivo | `web/dns.c` |
| `ranging` | 2 | 3 KB | Mide distancia a un vecino cada 2,5 s | `ranging/ranging.c` |

Criterios de las prioridades:

- **Lo que no puede esperar arriba.** Botones (la interrupción ya se desarmó y
  hay que rearmarla) y la radio (las colas de ESP-NOW se llenan rápido).
- **El audio en vivo por encima del audio grabado**: una llamada que se atrasa se escucha cortada.
- **Lo que reintenta abajo.** Courier, repair y ranging son trabajo de fondo
  que puede llegar tarde sin consecuencias.

Cómo se comparten datos entre tasks:

- **Colas** (`xQueueSend`/`xQueueReceive`) cuando hay un productor y un consumidor:
  de la ISR de botones al task, de la radio a la mesh, del mic a la llamada.
- **Mutex** (`xSemaphoreTake`) para tablas compartidas: store, presence, vecinos, courier.
- **Secciones críticas** (`portMUX`, `taskENTER_CRITICAL`) para estado chico que
  se lee muy seguido: la vista de la UI, el estado de la llamada.
- **Variables `volatile` / atómicas** para flags de un solo escritor (grabando, reproduciendo).

Regla de oro que sigue el código: **los callbacks de drivers y de la mesh no bloquean.**
`on_link_rx` solo copia el frame a una cola; los handlers de la mesh corren en
`mesh_rx` y, si tienen algo largo para hacer (mandar una voz entera), lo
encolan para su propio task (`sender_post`, `post_job`).

## Arranque

`main/main.c` → `app_main` arma las piezas **en orden de dependencia**:

1. NVS (memoria no volátil: nombres e historial) y el loop de eventos.
2. Hardware: botones, audio (mic y parlante), medidor de ruido.
3. `mesh_init`: identidad del nodo (de la MAC), colas y tasks. **Todavía no toca la radio.**
4. `presence_init`, `messages_init`, `call_init`: registran sus handlers en la mesh.
5. `radio_init(nombre)`: levanta el WiFi con el SSID = nombre de la placa. Por eso la
   mesh se inicializa antes: el nombre por defecto sale de su ID.
6. `mesh_start`: arranca ESP-NOW y la recepción. Recién ahí salen paquetes.
7. `ranging_init`, `web_init`, `ui_init`, `controls_init`.

Los handlers se registran (paso 4) **antes** de que la radio reciba nada
(paso 6), así ningún paquete llega a un tipo sin dueño.

## Perfiles de memoria

La PSRAM octal del módulo usa internamente los GPIO 33–37 y el display usa 36 y
37: **no pueden convivir**. Hay dos perfiles:

| | `psram` | `display` |
|---|---|---|
| RAM | ~512 KB internos + 8 MB PSRAM | ~380 KB libres al arrancar, sin PSRAM |
| Pantalla | no (todo el dibujo es no-op y se loguea un resumen por serie) | sí |
| Memoria para voces y archivos | 4 MB (~15 min de voz) | 96 KB (~20 s de voz) |
| Archivo máximo | 1 MB | 48 KB |
| Config | `sdkconfig.defaults` | `sdkconfig.defaults` + `sdkconfig.defaults.display` |

En `board.h`, con PSRAM los pines del LCD **ni siquiera se definen**, y
`display.c` tiene un `#error` si alguien compila el display con PSRAM: el
conflicto de pines no se puede introducir por accidente.

Los valores por defecto de cada perfil se eligen en Kconfig según
`CONFIG_SPIRAM` (`components/messages/Kconfig.projbuild`):

```
config SIGSEV_VOICE_STORE_KB
    default 4096 if SPIRAM
    default 96
```

## Configuración (menuconfig)

Todo lo ajustable está en Kconfig (`idf.py menuconfig` → menús *SIGSEV: …*), no
en `#define` sueltos: canal WiFi, celus por placa, TTL, jitter de reenvío,
timeout de vecinos, largo de voz, memoria, tamaño de archivo, ganancia,
parlante (pines y volumen), rotación del display, calibración de distancia. Así
se cambia sin tocar código y cada opción tiene su ayuda.

Algunas opciones de ESP-IDF que el proyecto cambia (`sdkconfig.defaults`):

| Opción | Valor | Por qué |
|---|---|---|
| `CONFIG_FREERTOS_HZ` | 1000 | Tick de 1 ms: el jitter de reenvío (0–8 ms) y las esperas cortas lo necesitan; con el default de 100 Hz todo se redondea a 10 ms. |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | 1024 | Los navegadores de celular mandan headers largos. |
| `CONFIG_LWIP_MAX_SOCKETS` | 16 | Conexiones HTTP + DNS cautivo. |
| `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` | 4096 | Los handlers de eventos copian un `message_t` (~250 B) al stack. |
| `CONFIG_ESP_WIFI_FTM_ENABLE` | y | Medición de distancia por WiFi. |
| `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | y | Con PSRAM, los buffers de red van ahí y liberan RAM interna. |

## Estilo del código

- C11, nombres `modulo_accion` (`mesh_send`, `presence_known`), estáticos `s_` y tipos `_t`.
- Errores con `esp_err_t` y las macros `ESP_RETURN_ON_ERROR` / `ESP_RETURN_ON_FALSE`, que loguean y salen.
- Formatos en el aire como `struct __attribute__((packed))` con
  `_Static_assert` de que entran en un paquete: si alguien agranda un campo, no compila.
- Comentarios en castellano explicando **por qué**, no qué.
- ESP-IDF compila con `-Werror=all`: los warnings comunes (un `switch` sin todos los casos, un puntero a un campo *packed*) son errores.
