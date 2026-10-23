# 05 · Mensajes

`components/messages/` es el corazón funcional: alertas, texto, voz y archivos;
dónde se guardan; quién los ve; cómo se completan si llegan rotos y cómo llegan
a alguien que no estaba.

| Archivo | Responsabilidad |
|---|---|
| `include/messages.h` | API pública (la fachada) |
| `internal.h` | Lo que se comparten los archivos del componente |
| `messages.c` | Init, visibilidad, listado, leídos |
| `store.c` / `store.h` | Almacenamiento de mensajes y bloques |
| `alerts.c` | Alertas y texto |
| `voice.c` | Grabación, importación desde el celu, envío, recepción y decodificación de voz |
| `files.c` | Archivos |
| `sender.c` | Task que manda rangos de bloques |
| `repair.c` | Pedidos de bloques perdidos (NACK) |
| `courier.c` | Entrega de privados (store-and-forward) y confirmaciones |
| `player.c` | Reproducción en el parlante |

## 1. El modelo: `message_t`

```c
typedef struct {
    uint32_t id;             // id LOCAL en el store de esta placa (1, 2, 3…)
    message_kind_t kind;     // ALERT, TEXT, VOICE, FILE
    mesh_node_id_t origin;   // quién lo mandó: una placa o un celu
    uint16_t ref;            // número del mensaje según su origen
    mesh_node_id_t dest;     // 0 = para todos; si no, privado
    bool delivered;          // privado: llegó a la placa del destino (✓)
    bool read;               // privado: el destino lo vio (✓✓)
    uint8_t hops;            // saltos que dio; 0xff = salió de esta placa
    uint32_t received_ms;    // cuándo llegó (reloj local)
    alert_kind_t alert;
    char text[161];          // texto, o nombre del archivo
    uint16_t blocks, blocks_received;   // voz y archivos
    uint32_t file_size;
    char mime[32];
} message_t;
```

Dos identidades, a propósito:

- **`id`** es local y sirve para la API y la pantalla (`/api/voice?id=12`). Cada
  placa numera a su manera.
- **`(origin, ref)`** es global: identifica el mismo mensaje en todas las
  placas. Viaja **dentro del payload** de texto, voz y archivos (no solo en el
  header de la mesh), para que una placa que lo custodia lo pueda reenviar
  tal cual y quien lo reciba lo reconozca como repetido. `ref` arranca en un
  valor al azar en cada arranque para no repetir refs de antes de un reinicio.

Las alertas no tienen `ref`: no se reenvían nunca, así que el dedup de la mesh
alcanza.

## 2. Quién ve qué

En una placa conviven mensajes de varios usuarios: los de la placa misma (sus
botones), los de cada celu conectado y los que custodia para otros.
`messages.c` define los criterios:

| Función | Verdadero si… | Se usa para |
|---|---|---|
| `messages_for_client(m, c)` | `dest == todos`, o `dest == c`, o `origin == c` | Lo que ve el celu `c` en la web |
| `messages_for_board(m)` | lo mismo con `c` = la placa | Lo que muestra el OLED |
| `messages_dest_is_here(m)` | el destino es la placa o un celu conectado a ella | Decidir quién confirma la entrega |
| `messages_is_local(m)` | es para la placa, para un celu de acá, o lo mandó un celu de acá | Avisar eventos, completar bloques perdidos |
| `messages_in_transit(m)` | `!messages_is_local(m)` | Custodia; contador "en custodia" |
| `messages_valid_sender(f)` | `f` es la placa o un celu conectado | Que nadie mande "como" un celu que no está acá |

La placa es **un usuario más**: su pantalla no muestra los privados de los
celus conectados a ella. Es una decisión de privacidad tanto como de diseño:
que alguien tenga la placa en la mano no le da acceso a lo de los demás.

## 3. El store

`store.c` guarda hasta **48 mensajes** en un anillo: cuando se llena, el nuevo
pisa al más viejo. Todo acceso es bajo un mutex, así la mesh, la web, la UI y
los tasks de envío pueden usarlo a la vez.

### Bloques en segmentos

Voces y archivos tienen bloques de 220 bytes de datos (224 con el estado del
códec y la marca de presente). No se guardan en un arreglo contiguo, sino en
**segmentos de 32 bloques (~7 KB)** que se reservan cuando llega el primer
bloque de cada uno:

```
mensaje de 100 bloques → 4 punteros a segmentos
  seg 0: [b0 … b31]   reservado (llegó algo)
  seg 1: NULL         (nada todavía: no ocupa memoria)
  seg 2: [b64 … b95]  reservado
  seg 3: [b96 … b99]  reservado
```

**Por qué:** en el perfil sin PSRAM quedan ~116 KB libres, pero el bloque
contiguo más grande es de ~76 KB (el heap está fragmentado). Una voz de 20 s
contigua (81 KB) no entraría nunca. Con segmentos:

- una grabación ocupa lo grabado, no el máximo (se reserva el largo máximo al
  empezar porque no se sabe cuánto va a durar);
- no hace falta memoria contigua grande;
- un mensaje del que llegaron pocos bloques ocupa poco.

### Presupuesto y desalojo

Los segmentos salen de un presupuesto: 96 KB sin PSRAM, 4 MB con PSRAM
(`CONFIG_SIGSEV_VOICE_STORE_KB`). Si un bloque nuevo no entra en el
presupuesto o no hay memoria, se **desaloja la voz o el archivo más viejo**
(nunca el que se está llenando) y se reintenta. Con PSRAM, los segmentos se
piden preferentemente ahí (`heap_caps_calloc_prefer(…, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)`).

### Normalización cacheada

Los mics PDM graban bajo. Al reproducir, cada voz se normaliza para que su pico
quede cerca del máximo (con una ganancia de hasta ×8). Calcular el pico implica
decodificar todo el mensaje, así que la ganancia se guarda en el store y se
invalida si llega un bloque nuevo.

## 4. Alertas y texto (`alerts.c`)

- **Alerta:** `{kind, origin}`, 5 bytes. `origin` va en el payload porque puede
  ser un celu (el header de la mesh siempre dice la placa). Se acepta también
  el formato viejo de 1 byte, tomando el origen del header.
- **Texto:** `{origin, ref, dest, len, text[≤160]}`. 160 bytes de UTF-8 alcanzan
  para un mensaje de emergencia y entran holgados en un paquete.

Al recibir un texto, `store_find(origin, ref)` dice si ya lo teníamos. Si sí, es
un reenvío de algún custodio: se avisa al courier (`courier_heard`) y, si el
destino está acá, se vuelve a confirmar (el ACK anterior se pudo perder).

## 5. Voz (`voice.c`)

Detalles del códec y del audio en [06](06-audio-llamadas.md). Lo propio de
los mensajes:

- Bloques de **55 ms** (440 muestras a 8 kHz → 220 bytes de ADPCM), cada uno con
  el estado del códec: se decodifica solo.
- Largo máximo 20 s (`CONFIG_SIGSEV_VOICE_MAX_S`) = 364 bloques ≈ 80 KB.
- Mínimo 5 bloques (~275 ms): menos es un toque sin querer y se descarta.
- Paquete `VOICE`: `{origin, ref, dest, index, count, predictor, step_index, data[220]}` = 238 bytes, justo el máximo.
- Tres fuentes: grabación en la placa (`messages_voice_start/stop`),
  **importación** desde el celular (`messages_voice_import_begin/block/end`: la
  web manda PCM y la placa lo codifica como si lo hubiera grabado) y recepción
  de la red.

**Por qué el celu manda PCM y no ADPCM:** así el códec existe en un solo lugar
(la placa). La web solo baja la frecuencia a 8 kHz y normaliza.

## 6. Archivos (`files.c`)

- Pedazos de 220 bytes (los mismos bloques del store que la voz).
- Un paquete `FILE_INFO` con nombre (≤40 bytes), tipo MIME (≤31) y tamaño, que
  se repite al principio de cada envío y de cada reenvío.
- Un paquete `FILE` por pedazo.
- Límite: 48 KB sin PSRAM, 1 MB con PSRAM (`CONFIG_SIGSEV_FILE_MAX_KB`).
  Sin PSRAM se mantiene muy por debajo del presupuesto (96 KB) para que un
  archivo no desaloje todas las voces.
- Un solo archivo se sube a la vez (el servidor web tiene un solo task).

Los bloques y los datos del archivo pueden llegar en cualquier orden:
`find_or_add` crea el mensaje con lo primero que llegue, y lo que falte se pide
con NACK (`n_ranges = 0` significa "mandame los datos del archivo").

## 7. Envío en segundo plano (`sender.c`)

Mandar 218 bloques espaciados lleva más de un segundo, y no se puede hacer
dentro de un handler de la mesh ni de un pedido HTTP. `sender_post(id, start,
count, info)` encola el trabajo (cola de 16) y el task `sender` lo hace: manda
los bloques presentes del rango, con 5 ms entre uno y otro.

Lo usan: la subida de archivos, los reenvíos del courier y las respuestas a
NACKs. La grabación de la placa usa su propio task (`voice`), porque además
tiene que cerrar la grabación.

## 8. Reparación de bloques (`repair.c`)

El flooding no reintenta. Con muchos paquetes seguidos, alguno se pierde.

- Cada 500 ms, el task `repair` revisa las voces y archivos **incompletos que le
  interesan a alguien de esta placa** y que no salieron de acá.
- Si un mensaje lleva **1,5 s sin bloques nuevos**, se considera trabado y se
  inunda un `NACK` con hasta 40 rangos de bloques faltantes (`store_missing`).
- **Contesta el origen**, si está en la red (la placa, o la placa del celu que
  lo mandó). Si no está, contesta **cualquiera que lo tenga**: puede salir
  alguno duplicado, que el store descarta.
- Más de 8 rangos se piden como uno solo de punta a punta (menos trabajos para
  el sender, a costa de mandar algo de más).
- Hasta 6 rondas seguidas sin progreso; después se rinde (hasta que llegue algo nuevo).

**Por qué NACK y no ACK por bloque:** un ACK por cada bloque, por cada
receptor, en una red por flooding, multiplica el tráfico por la cantidad de
receptores. Con NACK solo habla quien tiene un problema, y solo para decir qué
le falta. Es la idea de los protocolos de multicast confiable como SRM (Floyd
et al., 1997).

## 9. Privados: el courier (`courier.c`)

### La idea: store-and-forward epidémico

Un privado viaja por flooding como cualquier otro mensaje. Si el destino no
está, **todas las placas que lo escucharon lo guardan** y se lo reenvían cuando
el destino aparece. Es *epidemic routing* (Vahdat y Becker, 2000), la técnica
base de las redes tolerantes a demoras (DTN): no hace falta saber dónde va a
estar el destino, alcanza con que alguna placa con el mensaje se cruce con él.

### Cuándo se reenvía

Dos predicados:

- **`needs_courier`**: privado, el destino no está acá y todavía **no llegó** a
  su placa. Se reintenta periódicamente mientras el destino esté vivo.
- **`pending`**: privado, el destino no está acá y todavía **no se leyó**.
  Incluye lo entregado pero no visto. Solo se reenvía cuando el destino
  *aparece* (por ejemplo, un celu que se cambió de placa antes de ver un mensaje).

| Situación | Qué hace el courier |
|---|---|
| Aparece el destino (`NODE_JOINED`) | Agenda el reenvío con una espera al azar de 0,5 a 3 s |
| Vence la espera y nadie se adelantó | Reenvía |
| Vence la espera y **escuchó a otro** reenviarlo | No reenvía (supresión) |
| El destino sigue vivo y no confirmó | Reintenta cada ~20 s, hasta 5 veces por aparición |
| El destino aparece como celu **de esta placa**, el mensaje ya estaba acá y no figuraba entregado | Lo entrega directamente (confirma sin reenviar) |
| Aparece el **origen** de un privado que sabemos entregado | Le reenvía la confirmación (la original se le pudo perder) |
| El mensaje tiene más de 2 h | Deja de insistir |

### Supresión

Si 20 placas custodian el mismo privado y el destino aparece, que las 20 lo
reenvíen a la vez es inútil y satura el canal. Cada una espera un tiempo al
azar; la primera reenvía y las demás, al escuchar el reenvío
(`courier_heard`), cancelan el suyo. Es la misma idea que Trickle (Levis et
al., 2004), que se usa para propagar código en redes de sensores.

### Confirmaciones

El `ACK` lleva `{origin, ref, kind}` y se inunda:

- **`ACK_DELIVERED` (✓):** lo manda la placa donde está el destino, al recibir
  el mensaje completo.
- **`ACK_READ` (✓✓):** lo manda la placa del celu destino cuando el celu lo
  listó (`messages_mark_read`, que llama la API). Si el destino es una placa,
  se considera leído apenas llega: su pantalla lo muestra en el momento.

Quien escucha un ACK marca el mensaje (`store_set_delivered` / `store_set_read`)
y deja de reenviarlo. Las confirmaciones del mismo tipo para el mismo mensaje
se limitan a una cada 3 s, para que una ráfaga de reenvíos repetidos no genere
una ráfaga de ACKs.

### Por qué no un ruteo dirigido al destino

Podría mandarse el privado solo por el camino hacia la placa del destino. Pero
ese camino no se conoce de antemano (la gente se mueve), cambia y, si el
destino no está, no existe. El flooding con custodia funciona sin conocer la
topología, a costa de tráfico. Para mensajes de texto y voces cortas, en redes
de decenas de placas, ese costo es aceptable.

## 10. Reproducción (`player.c`)

Un task decodifica bloque a bloque y escribe al parlante; el DMA del I2S marca
el ritmo. Pedir otra voz mientras suena una corta la actual. Al final se manda
un bloque de silencio para vaciar el buffer sin cortar la última palabra.
Grabar corta la reproducción (si no, el mic grabaría el parlante).

## 11. Eventos

`messages_notify` publica en `MESSAGES_EVENT` con el `id` local:

| Evento | Cuándo |
|---|---|
| `NEW` | Llegó o se mandó un mensaje que le interesa a alguien de esta placa |
| `UPDATED` | Una voz o un archivo completó, o llegaron los datos de un archivo |
| `DELIVERED` | Cambió la confirmación de un privado (✓ o ✓✓) |
| `REC_STARTED` / `REC_STOPPED` | Empezó o terminó una grabación en la placa |
| `PLAY_STOPPED` | Terminó una reproducción |

La UI filtra con `messages_for_board`: lo de los celus no se avisa en la placa.
