# 08 · Web: portal cautivo, API y app

`components/web/` hace que cualquier celular, sin instalar nada, use la red:
se conecta al WiFi y se le abre la app.

| Archivo | Qué hace |
|---|---|
| `web.c` | Servidor HTTP, archivos embebidos, redirección al portal, opción DHCP |
| `dns.c` | DNS cautivo: contesta la IP de la placa para cualquier nombre |
| `api.c` | API JSON |
| `json_out.c` | Escritura de JSON en streaming (sin armar el documento entero en RAM) |
| `www/index.html`, `app.js`, `style.css`, `fonts/` | La app. Sin frameworks ni dependencias. |

## 1. El portal cautivo

Cuando un celular se conecta a un WiFi, prueba si hay internet pidiendo una URL
conocida (Android: `connectivitycheck.gstatic.com/generate_204`, iOS:
`captive.apple.com`, Windows: `msftconnecttest.com`). Si la respuesta no es la
esperada, asume que hay un **portal cautivo** (como en un hotel o un aeropuerto)
y muestra la notificación "Iniciar sesión en la red".

SIGSEV usa ese mecanismo para que la app se abra sola, con tres piezas:

1. **DNS cautivo** (`dns.c`). Un servidor UDP en el puerto 53 que contesta
   **cualquier** consulta de tipo A con la IP de la placa (`192.168.4.1`), con TTL
   de 30 s. Las consultas AAAA (IPv6) se contestan vacías, así el celular cae a
   IPv4 enseguida en vez de esperar un timeout. Es un parser mínimo: toma solo la
   primera pregunta y rechaza lo mal formado.
2. **Redirección** (`web.c`). Cualquier URL que no sea de la app ni de la API
   cae en el handler de 404, que contesta `302 Found` con `Location: http://192.168.4.1/`.
   El chequeo de conectividad recibe un 302 en vez de su 204, y el celular abre el portal.
3. **DHCP opción 114** (RFC 8910). El servidor DHCP de la placa anuncia la URL
   del portal al dar la IP. Android 11+ e iOS 14+ la usan para abrir el portal
   directo, sin depender de los chequeos.

**Por qué no hay HTTPS:** sin internet no hay forma de obtener un certificado
válido, y uno autofirmado muestra una advertencia que espanta a cualquiera. El
precio es que el navegador no da el micrófono ni la ubicación (ver [06](06-audio-llamadas.md)).

## 2. El servidor

ESP-IDF trae `esp_http_server`: un servidor HTTP/1.1 con **un solo task** que
atiende todas las conexiones en orden. Configuración (`web_init`):

| | | |
|---|---|---|
| `max_uri_handlers` | 24 | Rutas de la app + API |
| `stack_size` | 6 KB | Los handlers arman respuestas grandes |
| `lru_purge_enable` | sí | Los celulares abren muchas conexiones y no las cierran; si se llena, se cierra la menos usada |

Los archivos de `www/` se **embeben en el firmware** (`EMBED_TXTFILES` y
`EMBED_FILES` en el `CMakeLists.txt`): el linker los pone en flash como arreglos
de bytes y `get_asset` los manda tal cual. No hay sistema de archivos.
`EMBED_TXTFILES` agrega un `\0` al final que no se manda.

Cache: las fuentes se mandan con `immutable` (no cambian nunca); el HTML, el JS
y el CSS con `no-cache` (el celu revalida y ve la versión nueva después de
flashear); las respuestas de la API con `no-store`.

### JSON en streaming

`/api/state` con 48 nodos conocidos y sus enlaces puede tener varios KB. En vez
de armar el JSON entero en un buffer (RAM que no sobra), `json_out.c` escribe
en un buffer chico y lo va mandando como **chunks** de HTTP
(`httpd_resp_send_chunk`): `json_begin`, `json_raw`, `json_fmt`, `json_str`
(con escapado) y `json_end`.

## 3. Identidad del celular

Cada celu es un **cliente** de la placa (ver [01](01-vision.md)):

- La primera vez, `app.js` (`loadMe`) sortea un ID de 32 bits con
  `crypto.getRandomValues` y lo guarda en `localStorage`, junto con un nombre
  (por defecto `Celu XXXX`, editable en *Vos*).
- **Todo pedido** a la API lleva `?c=<id>` (también las URLs de `<img>` y de
  descarga, que no pueden mandar headers) y el header `X-Client-Name` con el
  nombre URL-encoded (`withMe`, `api`).
- Del lado de la placa, `client_of` (`api.c`) valida el ID, lo registra como
  cliente local (`presence_client_seen`) y lo devuelve. Un pedido sin ID válido
  recibe `400`.

Como el ID vive en el celu, **si la persona se pasa a otra placa sigue siendo
la misma**: sus privados la siguen.

## 4. Referencia de la API

Todos los endpoints requieren `c=<id del celu>` (8 hexa).

| Método y ruta | Parámetros | Hace |
|---|---|---|
| `GET /api/state` | — | Placa (`self`), cliente (`me`), placas vivas con sus enlaces, historial (`known`, con `client` y `parent` para celus), estadísticas, si se está grabando, límites (`voice_max_s`, `file_max`) y si hay parlante |
| `GET /api/messages` | `after` (opcional) | Mensajes del cliente, del más nuevo al más viejo. Después de responder, marca leídos sus privados completos |
| `GET /api/voice` | `id` | La voz como WAV (16 bits, 8 kHz), decodificada al vuelo |
| `GET /api/file` | `id` | El archivo, con su `Content-Type`. `409` si todavía está llegando |
| `POST /api/alert` | `kind` (0–3) | Manda una alerta |
| `POST /api/text` | `to` (opcional) | Manda el body como texto (≤160 bytes) |
| `POST /api/voice` | `to` (opcional) | Body: PCM 16 bits mono 8 kHz. La placa lo codifica y lo manda |
| `POST /api/file` | `to` (opcional) | Body: el archivo. `Content-Type` es su tipo; `X-File-Name`, su nombre |
| `POST /api/play` | `id` | Reproduce la voz en el parlante de la placa. `501` si no hay parlante |
| `POST /api/name` | — | Renombra **la placa** con el body (≤20 bytes) |

Un celu solo puede pedir voces y archivos que son para él (`messages_for_client`):
si no, recibe `404`, como si no existieran.

Ejemplo de un mensaje en `/api/messages`:

```json
{"id":8,"kind":"text","origin":"a1b2c3d4","from":"Ana","hops":0,"age":12,
 "to":"0badf00d","to_name":"Lucho","delivered":true,"read":false,
 "text":"¿Dónde estás?"}
```

## 5. La app

Un solo archivo de JS (`app.js`, ~840 líneas) sin dependencias: se sirve desde
la placa y el celular no tiene internet para bajar una biblioteca.

### Polling, no WebSockets

La app pide `/api/state` cada 2 s y `/api/messages` cada 1,5 s.

**Por qué:** el servidor tiene un solo task; un WebSocket por celular ocuparía
una conexión permanente cada uno y complicaría el servidor. Los celulares además
cortan conexiones cuando se apaga la pantalla. Con polling cada pedido es
independiente, y si uno falla, el siguiente lo compensa. Con 4 celulares por
placa son ~5 pedidos por segundo: el servidor los atiende de sobra.

`/api/messages` devuelve todo cada vez (no solo lo nuevo): son pocos mensajes y
así se actualizan las edades, el progreso de las voces que están llegando y las
confirmaciones.

### Pestañas

- **Alertas:** botones de alerta (mantener 0,9 s; la animación muestra el
  progreso), selector *Para* (Todos, el historial o un ID a mano), texto, *●
  Grabar audio*, *＋ Foto / archivo*, el modo *Walkie* y el feed.
- **Red:** el mapa (ver [07](07-distancia.md)) y la lista de placas y celus con ✉
  para escribirles.
- **Vos:** tu nombre e ID, la placa a la que estás conectado, renombrarla y el tráfico.

### Detalles que importan

- **Feed:** los privados llevan la etiqueta PRIVADO; los propios muestran *en
  camino…*, *✓ entregado* o *✓✓ visto*. Lo que llega nuevo vibra
  (`navigator.vibrate`) y muestra un banner.
- **Fotos:** se achican y recomprimen en el celu (`shrinkImage`: canvas a
  1280→240 px, JPEG 0,8→0,35) hasta que entran en el límite. Las miniaturas se
  cachean (`app.thumbs`) para no volver a pedirlas en cada refresco.
- **Selector de destino:** se rehace solo si cambió la lista, para no cerrarle
  el menú a quien lo tiene abierto. Un ID a mano se acepta completo o por sus
  últimas cifras (las que muestra la pantalla de la placa), si no hay ambigüedad.
- **Nombres de archivo:** se cortan a 40 bytes de UTF-8 sin partir un carácter (`shortName`).

### Estilo

Inspirado en la web de la hackatón (comcom.dc.uba.ar/hackaton): fondo marrón casi
negro, dorados, líneas de 1 px, nada redondeado, títulos en **Silkscreen**
(fuente pixel, licencia OFL, embebida en `www/fonts/`) y texto monoespaciado.
Los colores son variables CSS (`--bg`, `--gold`, …). Los colores de las alertas
siguen la semántica habitual: rojo para SOS, verde para "estoy bien".

## Referencias de esta parte

- [`esp_http_server`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_http_server.html).
- RFC 8910, *Captive-Portal Identification in DHCP and Router Advertisements* (2020).
- RFC 8908, *Captive Portal API* (2020); RFC 8952, *Captive Portal Architecture* (2020).
- RFC 1035, *Domain Names — Implementation and Specification* (formato de los mensajes DNS).
