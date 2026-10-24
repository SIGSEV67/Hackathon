# 11 · Limitaciones, riesgos y trabajo futuro

Un prototipo de hackatón tiene decisiones tomadas por tiempo. Este documento
las hace explícitas: qué no anda, qué anda pero no escala, qué no se probó y
qué haríamos después. Es mejor contarlo en la presentación que esperar a que
lo pregunten.

## 1. Seguridad

Lo más importante. **Hoy SIGSEV no tiene ninguna protección.**

| Problema | Consecuencia | Por qué está así |
|---|---|---|
| **Nada está cifrado.** ESP-NOW va en claro y el WiFi del AP es abierto. | Cualquiera con una placa o una antena en el canal 6 lee todos los mensajes, incluidos los privados. El tráfico celu ↔ placa (HTTP en un WiFi abierto) también se puede capturar. | Cifrar bien requiere manejo de claves, que es la parte difícil. Lo dejamos explícito: "privado = dirigido, no cifrado". |
| **El ID del celu es su única credencial.** | Quien conozca el ID de otra persona (se ve en la web y en la pantalla *Red*) puede mandar `?c=<ese ID>` y **leer sus privados** o mandar mensajes en su nombre. | La identidad la elige el celu y no hay cuentas. Es lo más simple que permite "cada uno ve lo suyo". |
| **No hay autenticación entre placas.** | Una placa con otro firmware puede mandar paquetes con cualquier `origin`: alertas falsas, ACKs falsos, beacons con otro nombre. | Lo mismo: requiere claves. |
| **No hay límite de tráfico.** | Un atacante (o una falla) puede inundar la red de alertas o de voces y saturar el canal, o desalojar los mensajes guardados. | — |

### Qué haríamos

- **Identidad del celu con una clave secreta**, no solo un ID: el celu genera
  un secreto al azar, la placa guarda un hash y cada pedido lo presenta. Así,
  saber el ID de alguien no alcanza para leer sus mensajes.
- **Cifrado de punta a punta entre celulares** con criptografía de clave
  pública (por ejemplo X25519 + XChaCha20-Poly1305). Ojo: la API `crypto.subtle`
  del navegador **solo existe en HTTPS**, así que habría que traer la
  criptografía en JavaScript (por ejemplo TweetNaCl, ~7 KB).
- **Firmas de las placas** (Ed25519) para que nadie pueda falsificar alertas o
  ACKs de otra, y una lista de placas confiables.
- **Límite de paquetes por origen** al reenviar, para contener inundaciones.

## 2. Escala: el costo del flooding

Cada paquete lo retransmite **cada placa** de la red. Un frame de 250 bytes a
1 Mbps ocupa el aire unos 2,3 ms. Para una voz de 20 s (364 paquetes):

| Placas en la red | Transmisiones | Aire ocupado (aprox.) |
|---|---|---|
| 2 | 728 | 1,7 s |
| 10 | 3640 | 8,4 s |
| 20 | 7280 | 16,7 s |
| 50 | 18 200 | 42 s (el canal se satura) |

Es una cota (no todas las placas se escuchan entre sí, así que pueden
transmitir a la vez en zonas distintas), pero muestra el límite: **decenas de
placas, no cientos**, si hay mucha voz. Las alertas y el texto (1 paquete)
escalan mucho mejor.

El dedup guarda 1024 paquetes: alcanza para ~2 voces largas simultáneas. Con
más ráfagas a la vez, una entrada se puede pisar mientras el paquete todavía
rebota, y se reenvía otra vez.

### Qué haríamos

- **Subir la tasa de ESP-NOW** (se puede configurar por *peer*; el default es 1 Mbps).
- **ESP-NOW v2** (ESP-IDF 5.4+) permite frames de hasta ~1470 bytes: 6 veces
  menos paquetes por voz. Requiere que todas las placas lo soporten.
- **Flooding probabilístico** o con supresión (como Meshtastic: quien escucha
  que un vecino ya reenvió, no reenvía), o reenvío solo por algunos nodos (MPR, como OLSR).
- **TTL menor para la voz** que para las alertas: una alerta tiene que llegar
  lejos; una voz a todos, quizás no tanto.
- **Prioridades en la cola de salida**: alertas y ACKs antes que bloques de voz.

## 3. Memoria y almacenamiento

- El store tiene **48 mensajes** en un anillo. En una red muy activa, los
  mensajes nuevos pisan a los viejos, **incluidos privados en custodia**. Una
  ráfaga de alertas puede hacer que una placa pierda un privado que custodiaba.
  Lo correcto sería desalojar primero lo que ya está entregado o es para todos.
- Sin PSRAM entran ~20 s de voz en total (96 KB). Cada voz nueva desaloja las
  viejas. Con PSRAM, ~15 min.
- Nada sobrevive a un reinicio salvo nombres e historial (NVS): los mensajes se pierden.

## 4. Tiempo

- No hay reloj de pared: todo es tiempo desde el arranque de cada placa
  (`mesh_now_ms`). Las edades ("hace 3 min") son desde que el mensaje llegó a
  *esa* placa, no desde que se escribió.
- El límite de 2 h para dejar de insistir con un privado se cuenta desde que
  cada placa lo recibió: un mensaje que va pasando de placa en placa puede vivir más.
- `mesh_now_ms` es de 32 bits: da la vuelta a los 49,7 días de encendido. No es
  un problema en una marcha.
- `ref` es de 16 bits y arranca al azar en cada arranque: después de un
  reinicio hay una chance chica de repetir el `ref` de un mensaje viejo que
  siga guardado en alguna placa, que lo tomaría como repetido.

## 5. Comportamientos que conviene conocer

- **✓✓ significa que la app del celu lo recibió**, no que la persona lo leyó: la
  web marca leído lo que lista. Si la app está abierta en segundo plano, cuenta como visto.
- **Un celu con la pantalla apagada** deja de consultar; a los 30 s la placa lo
  da por ido. Sus privados quedan esperando y le llegan en cuanto vuelve a
  abrir la app (la placa lo detecta y los entrega).
- **4 celulares por placa** (configurable hasta 10). Más es más tráfico de
  polling y menos aire para ESP-NOW.
- **El servidor web tiene un solo task:** mientras un celu sube un audio, los
  demás esperan para refrescar.
- **Cambiar el nombre de la placa** cambia el SSID recién al reiniciar.
- **Las llamadas solo son entre placas** y viajan por flooding: cada placa
  reenvía el audio de cada llamada.
- **En iPhone** el `<input capture>` puede ofrecer elegir un archivo en vez de
  abrir el grabador, según la versión de iOS.
- **El modo walkie** depende de que el navegador permita reproducir solo: si la
  página quedó en segundo plano, puede no sonar.
- **La distancia por RSSI** puede errar al doble o a la mitad; la de FTM no está calibrada.
- **El easter egg reinicia toda la red** y viene prendido. Cualquiera con una
  placa (o que arme un paquete `KIWI`) puede tirar la red ~5 s; los mensajes
  guardados en RAM se pierden con el reinicio. Para uso real, apagar
  `CONFIG_SIGSEV_EASTER_EGG`: así la placa ni siquiera tiene el handler.

## 6. Qué está probado y qué no

| Estado | Qué |
|---|---|
| **Probado en placa** (2 placas) | Arranque, descubrimiento mutuo (~3 s), mesh, pantalla en las dos, joystick con la corrección de 45°, FTM entre placas (13–14 mediciones válidas por sesión), inicialización del parlante |
| **Usado en versiones anteriores**; confirmar y reprobar después de los últimos cambios | Alertas, texto y voz entre placas; privados entre placas |
| **Solo compilado** | Easter egg (código, comando por USB, aviso a la red), reproducción por parlante (no había parlante conectado), llamadas, audio desde el celu, reparación de bloques, grabaciones largas con segmentos, FTM a 3–5 m |
| **Compilado y probado contra una API simulada** (en la PC), no en placa | Todo el modelo de clientes: identidad del celu, privados celu a celu, ✓/✓✓, cambio de placa, walkie por clips |

## 7. Deuda técnica

- No hay tests automáticos salvo el del códec (`test/host/test_adpcm.c`). Lo
  siguiente que convendría testear en la PC: el store (desalojo, segmentos) y
  el parser DNS, que no dependen del hardware.
- `flash.sh` usa `psram` por defecto, pero las dos placas de la demo usan `display`.
- El commit `4f38906` no compila solo (el siguiente lo arregla): un `git bisect`
  tiene que saltearlo.
- `app.js` es un solo archivo de ~730 líneas; si crece, conviene partirlo en módulos.

## 8. Trabajo futuro (además de lo de arriba)

- **Ubicación absoluta** con al menos una placa con GPS (un módulo UART barato),
  combinada con las distancias.
- **Mapa de ruido en el tiempo** (historial por placa), para ver hacia dónde se mueve "lo caliente".
- **Puente a internet:** una placa que tenga conexión podría sacar alertas
  afuera (SMS, Telegram) cuando haya.
- **Backend I²C/cable** para `link_t` (los headers están en la placa), para
  extender la red con cable donde la radio no llega.
- **Ahorro de energía:** hoy la radio no duerme nunca; con batería, eso define la duración.
