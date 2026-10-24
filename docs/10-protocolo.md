# 10 · Protocolo: referencia de paquetes

Todo viaja como **broadcast ESP-NOW** (≤ 250 bytes por frame): 12 bytes de
header de la mesh + hasta 238 de payload. Los structs se mandan crudos
(`__attribute__((packed))`, little endian); los tamaños de esta página son los
de esos structs.

Notación: `u8/u16/u32` sin signo, `i8/i16` con signo, `char[n]` texto UTF-8
sin terminador obligatorio. Un ID (`origin`, `dest`, `to`) es `u32`; `dest = 0`
significa "para todos".

## Header de la mesh (12 B) · `mesh/mesh.c`

| Offset | Campo | Tipo | Valor / significado |
|---|---|---|---|
| 0 | `magic` | u8 | `0x53` ('S') |
| 1 | `version` | u8 | `1` |
| 2 | `type` | u8 | ver tabla de tipos |
| 3 | `ttl` | u8 | saltos que le quedan (sale con 6) |
| 4 | `hops` | u8 | saltos que dio (sale con 0) |
| 5 | `reserved` | u8 | 0 |
| 6 | `seq` | u16 | secuencia del origen (arranca al azar) |
| 8 | `origin` | u32 | **placa** que creó el paquete |

`(origin, seq)` identifica el paquete para el dedup. Ojo: el `origin` del
header es siempre una **placa**; el autor de un mensaje (que puede ser un celu)
va en el payload.

## Tipos (`mesh_type_t`, `mesh/include/mesh.h`)

| # | Tipo | Módulo | Tamaño | Frecuencia |
|---|---|---|---|---|
| 1 | `PRESENCE` | presence | 28 + 8·vecinos (≤ 124) | cada 3 s ± 0,5 s por placa |
| 2 | `ALERT` | messages/alerts | 5 | al mandar una alerta |
| 3 | `TEXT` | messages/alerts | 11 + largo (≤ 171) | al mandar, y en reenvíos de custodia |
| 4 | `VOICE` | messages/voice | 238 | un paquete por bloque de 55 ms |
| 5 | `ACK` | messages/courier | 7 | entregado / leído |
| 6, 7 | — | — | — | libres: eran los archivos, que se sacaron. No se reusan para no correr los demás números |
| 8 | `NACK` | messages/repair | 7 + 4·rangos (≤ 167) | cuando algo se traba |
| 9 | `CALL` | call | 7 (señalización) / 232 (audio) | 1/s al sonar, 1 cada 2 s en llamada, 1 cada 55 ms al hablar |
| 10 | `CLIENTS` | presence | 1 + 24·celus (≤ 193) | después de cada beacon, si hay celus |
| 11 | `KIWI` | easter | 8 | 3 copias cuando explota el kiwi (easter egg) |

## `PRESENCE` · `presence/presence.c`

| Campo | Tipo | |
|---|---|---|
| `name` | char[20] | nombre de la placa |
| `flags` | u8 | bit 0: tiene pantalla · bit 1: tiene PSRAM |
| `noise` | u8 | ruido actual, 0–100 |
| `noise_peak` | u8 | pico desde el beacon anterior |
| `n_links` | u8 | cantidad de vecinos que siguen (≤ 12) |
| `uptime_s` | u32 | segundos desde que arrancó |
| `links[n]` | 8 B c/u | `{id u32, rssi i8, dist_dm u16, flags u8}` · `flags` bit 0: distancia por FTM · `dist_dm = 0`: no se sabe |

## `CLIENTS` · `presence/presence.c`

| Campo | Tipo | |
|---|---|---|
| `n_clients` | u8 | ≤ 8 |
| `clients[n]` | 24 B c/u | `{id u32, name char[20]}` |

Quien recibe anota `parent` = `origin` del header.

## `ALERT` · `messages/alerts.c`

| Campo | Tipo | |
|---|---|---|
| `kind` | u8 | 0 SOS · 1 MÉDICO · 2 PELIGRO · 3 ESTOY BIEN |
| `origin` | u32 | autor (placa o celu). Si el paquete trae solo `kind` (formato viejo), se usa el origen del header |

## `TEXT` · `messages/alerts.c`

| Campo | Tipo | |
|---|---|---|
| `origin` | u32 | autor |
| `ref` | u16 | número del mensaje según su autor |
| `dest` | u32 | 0 = todos; si no, privado |
| `len` | u8 | 1–160 |
| `text` | char[len] | UTF-8 |

## `VOICE` · `messages/voice.c`

| Campo | Tipo | |
|---|---|---|
| `origin`, `ref`, `dest` | u32, u16, u32 | identidad y destino del mensaje |
| `index` | u16 | número de bloque |
| `count` | u16 | bloques totales del mensaje |
| `predictor` | i16 | estado ADPCM al empezar el bloque |
| `step_index` | u8 | estado ADPCM al empezar el bloque (0–88) |
| `reserved` | u8 | 0 |
| `data` | u8[220] | 440 muestras de 4 bits (nibble bajo primero) |

## `ACK` · `messages/courier.c`

| Campo | Tipo | |
|---|---|---|
| `origin`, `ref` | u32, u16 | el mensaje que se confirma |
| `kind` | u8 | 0 = entregado (✓) · 1 = leído (✓✓). Si falta, entregado |

## `NACK` · `messages/repair.c`

| Campo | Tipo | |
|---|---|---|
| `origin`, `ref` | u32, u16 | el mensaje incompleto |
| `n_ranges` | u8 | ≤ 40 |
| `ranges[n]` | 4 B c/u | `{start u16, count u16}` bloques que faltan |

## `CALL` · `call/call.c`

Encabezado común (7 B): `{op u8, call_id u16, to u32}`. Solo procesa el paquete
la placa con ID `to`.

| `op` | Nombre | Extra |
|---|---|---|
| 0 | `INVITE` | — |
| 1 | `ACCEPT` | — |
| 2 | `HANGUP` | — |
| 3 | `BUSY` | — |
| 4 | `PING` | — |
| 5 | `AUDIO` | `seq u16, predictor i16, step_index u8, data u8[220]` (232 B en total) |

## `KIWI` · `easter/easter.c`

| Campo | Tipo | Valor |
|---|---|---|
| `magic` | u32 | `0x6977696b` (`"kiwi"` en little-endian) |
| `round` | u32 | Al azar, lo sortea la placa que empieza; las 3 copias lo comparten |

Lo manda **solo la placa donde se hizo el código**, en el instante de la
explosión, 3 veces con 40 ms entre copias (el broadcast no tiene ACK). Cada
copia es un paquete nuevo (`seq` distinto) y viaja por flooding como cualquier
otro. Quien lo recibe arranca su propia secuencia pero **no lo vuelve a
originar**: no hay cascada. Se ignora si el magic no coincide, si la placa ya
está en la secuencia o si ya hizo esa ronda (la última ronda se guarda en
memoria RTC y sobrevive al reinicio). Con
`CONFIG_SIGSEV_EASTER_EGG` apagado no hay handler: la placa lo reenvía igual
(la mesh reenvía antes de despachar) pero no hace nada.

## Compatibilidad

`version` está en 1 desde el principio. Los cambios de formato que se hicieron
(origen en las alertas, tipo en los ACK) se diseñaron para que un paquete del
formato viejo se siga entendiendo: los campos nuevos van al final y, si
faltan, se asume el valor anterior. Un cambio incompatible debería subir `version`.
