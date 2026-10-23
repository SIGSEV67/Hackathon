# 06 · Audio, voz y llamadas

## 1. La cadena de audio

```mermaid
flowchart LR
    mic[Mic PDM<br/>GPIO 7/15] -->|I2S0, 16 kHz estéreo| task[task mic<br/>elige el canal con señal]
    task -->|PCM mono 16 kHz| noise[noise<br/>nivel 0–100]
    task --> rec[grabación<br/>voice.c]
    task --> call[llamada<br/>call.c]
    rec & call --> framer[audio_framer<br/>sin DC, 8 kHz, 55 ms]
    framer --> adpcm[ADPCM<br/>220 B por bloque]
    adpcm --> mesh[(mesh)]
    mesh --> dec[decodificar] --> up[8→16 kHz] --> spk[Parlante I2S1<br/>MAX98357A]
```

### Micrófono PDM

El micrófono de la placa es **PDM** (*pulse density modulation*): entrega un
tren de bits a alta frecuencia cuya densidad de unos representa la amplitud.
El periférico I2S del ESP32-S3 tiene un modo PDM que lo filtra y entrega PCM de
16 bits (`i2s_channel_init_pdm_rx_mode`, `periph/audio.c`).

- Se configura a **16 kHz estéreo**. El mic ocupa un solo canal; el otro trae
  ruido o ceros. Como no se sabe de antemano cuál (depende de cómo está cableado
  el pin de selección), `pick_channel` calcula la energía de cada canal en cada
  bloque y se queda con el que tiene más.
- Un solo task (`mic`) lee y **reparte** el audio a los suscriptores
  (`audio_mic_add_sink`, hasta 4). Los callbacks corren en ese task y tienen
  que ser rápidos.

### El *framer*: de lo que da el mic a bloques de voz

`audio_framer_push` (`periph/audio.c`) hace tres cosas por muestra:

1. **Saca la componente continua.** Un mic PDM tiene un offset DC grande. Un
   pasa-altos de un polo lo estima y lo resta:
   `dc += (x − dc) / 128` → corte en ≈ 16000 / (2π·128) ≈ 20 Hz, por debajo de la voz.
   Se hace en punto fijo (`dc_q8`, 8 bits de fracción) porque no hace falta float.
2. **Baja de 16 a 8 kHz** promediando de a pares. Promediar es un filtro
   anti-alias mínimo (un *box filter* de 2 muestras): deja pasar algo de lo que
   hay arriba de 4 kHz, pero para voz alcanza y cuesta una suma.
3. **Junta 440 muestras (55 ms)** y llama a `on_frame`.

**Por qué 8 kHz:** es la frecuencia de la telefonía. La voz se entiende bien
con lo que hay por debajo de 4 kHz (Nyquist), y la mitad de muestras es la
mitad de bytes para mandar.

**Por qué 55 ms:** 440 muestras en ADPCM son 220 bytes, que con los 18 bytes de
encabezado del paquete de voz llenan exactamente los 238 bytes de payload de
la mesh. Es el bloque más grande que entra en un frame de ESP-NOW.

El framer lo comparten la grabación y las llamadas: se escribió una vez y se
movió a `periph` para que las dos lo usen.

## 2. El códec: IMA ADPCM

`components/adpcm/adpcm.c` (75 líneas, C puro, sin ESP-IDF).

**ADPCM** (*Adaptive Differential PCM*) no guarda cada muestra sino **la
diferencia con una predicción**, cuantizada a **4 bits**, con un **tamaño de paso
que se adapta**: si la señal cambia mucho el paso crece, si cambia poco se achica.

Por muestra:

1. `diff = muestra − predictor`.
2. Se codifica `diff` en 4 bits: 1 de signo y 3 que aproximan `diff / step`
   (bits de valor `step`, `step/2`, `step/4`).
3. Se actualiza el predictor con lo que el **decodificador** va a reconstruir
   (no con la muestra real): así encoder y decoder quedan sincronizados. En el
   código, los dos usan la misma función `apply_code`.
4. El índice del paso se mueve según el código (tabla `s_index_table`: −1 si el
   código fue chico, +2…+8 si fue grande) y el paso sale de una tabla de 89
   valores (`s_step_table`) que crece ~10 % por escalón.

| | PCM 16 bits | IMA ADPCM |
|---|---|---|
| Bits por muestra | 16 | 4 |
| A 8 kHz | 128 kbps | **32 kbps** (4 KB/s) |
| 20 s de voz | 320 KB | **80 KB** |
| Calidad (test de host, dos senos) | — | SNR ≈ 25 dB |

**Por qué ADPCM y no Opus, Speex o Codec2:**

- **Opus/Speex** comprimen mucho más (8–16 kbps con mejor calidad), pero piden
  decenas de KB de RAM y bastante CPU, y traer y portar la biblioteca a ESP-IDF
  lleva tiempo de hackatón que no teníamos.
- **Codec2** llega a 0,7–3,2 kbps (pensado justamente para radio), pero suena
  robótico y también es una dependencia grande.
- **ADPCM** son 75 líneas que entendemos enteras, cuestan casi nada de CPU y
  4:1 alcanza: el cuello de botella no es el ancho de banda de ESP-NOW sino la
  memoria para guardar mensajes.

**Bloques independientes.** Normalmente un stream ADPCM arrastra su estado
(predictor e índice) de muestra en muestra: si se pierde un pedazo, lo que
sigue sale mal hasta que el paso se readapta. Acá cada bloque guarda el
estado del códec **al empezar** (3 bytes: `predictor` e `index`), así se
decodifica sin los anteriores. Cuesta 1,4 % más de bytes y hace que perder un
paquete sea un silencio de 55 ms.

Test de host (corre en la PC):

```sh
cc -I components/adpcm/include test/host/test_adpcm.c components/adpcm/adpcm.c -lm -o /tmp/t && /tmp/t
# SNR 24.9 dB
# OK
```

## 3. Medidor de ruido (`components/noise/noise.c`)

Otro suscriptor del mic. Por cada bloque que entrega el mic (256 muestras, 16 ms):

1. **RMS sin la componente continua**: `sqrt(E[x²] − E[x]²)`.
2. **A decibeles**: `20·log10(rms + 1)`. Se le restan 20 dB y se escala para que
   el rango 20–80 dB del mic quede en 0–100. Es un nivel **relativo** (no son
   dB SPL calibrados): sirve para comparar placas entre sí, no para medir con
   precisión.
3. **Suavizado asimétrico**: sube instantáneo, baja de a poco
   (`smoothed = 0,8·smoothed + 0,2·nivel`). Un grito o una explosión se ven
   enseguida y no desaparecen en el próximo bloque.
4. Guarda el **pico** desde el último beacon (`noise_take_peak`), para que un
   ruido corto entre beacons no pase desapercibido.

El nivel va en cada beacon: la red tiene un **mapa de ruido** en vivo (en la
web, el color de cada nodo). En una marcha, dónde hay más ruido suele ser
dónde pasa algo.

## 4. Parlante

Opcional (`CONFIG_HACK_SPEAKER_ENABLE`, prendido por defecto): un amplificador
I2S como el **MAX98357A** en I2S1, pines DIN/BCLK/LRCLK configurables (por
defecto 4/5/6). Sin nada conectado los pines solo cambian de estado, así que es
seguro dejarlo prendido.

- **16 kHz estéreo.** El MAX98357A mezcla L y R por defecto, así que se manda
  la misma muestra en los dos canales.
- **Volumen** en menuconfig (40 % por defecto: el amplificador es fuerte).
- **Un mutex** para el parlante: tonos de llamada, voces y audio de llamada
  pueden querer sonar desde tasks distintos; las escrituras se serializan.
- **De 8 a 16 kHz** (`audio_speaker_write_8k`): interpola linealmente una
  muestra intermedia entre cada par. Guarda la última muestra entre llamadas
  (`prev`) para que no haya un salto en la unión de dos bloques.
- **Tonos** (`audio_play_tone`): un seno con rampas de 5 ms al principio y al
  final para que no haga "clic".

## 5. Audio desde el celular

El navegador solo da acceso al micrófono (`getUserMedia`) en **contextos
seguros** (HTTPS o `localhost`), y el portal de la placa es HTTP plano: no hay
forma razonable de tener un certificado válido para `192.168.4.1` sin internet.

La solución (`app.js` → `sendAudioFile`):

1. Un `<input type="file" accept="audio/*" capture>` abre **el grabador del
   sistema** (en Android, la grabadora; en iOS, según la versión, puede ofrecer
   un archivo). Eso no necesita HTTPS.
2. Lo que devuelve (m4a, webm, ogg, wav…) se decodifica con **Web Audio**
   (`AudioContext.decodeAudioData`), que sí anda en HTTP.
3. `toVoicePcm`: mezcla a mono, baja a 8 kHz promediando cada ventana (filtro
   de caja, como el framer), recorta al largo máximo y **normaliza** el pico a
   0,8 de escala completa (ganancia máxima ×20).
4. Lo manda como PCM de 16 bits crudo a `POST /api/voice`. La placa lo codifica
   en ADPCM con el mismo código que usa para su mic (`messages_voice_import_*`).

### Walkie por clips (celu a celu)

Para "hablar" entre celulares sin HTTPS no se puede hacer audio en vivo, pero sí
lo más parecido: con un destino privado elegido, el modo **Walkie** hace que:

- cada audio que se graba se mande solo, sin confirmar (eso ya pasa siempre);
- los audios que llegan **de esa persona** suenen solos apenas terminan de
  llegar, en orden, uno atrás del otro (`walkieIncoming`, `playQueued`).

Al prender el modo, lo que ya estaba en el feed se marca como escuchado: solo
suena lo nuevo. Los navegadores bloquean el audio automático hasta que el
usuario tocó la página; tocar el checkbox del walkie cuenta como interacción.

## 6. Llamadas en vivo entre placas (`components/call/call.c`)

### Protocolo

Todo va en paquetes `CALL` por la mesh, dirigidos a una placa (`to`) e
identificados por un `call_id` al azar:

| Op | Cuándo | Contenido extra |
|---|---|---|
| `INVITE` | El que llama, cada 1 s mientras suena | — |
| `ACCEPT` | El que atiende | — |
| `BUSY` | El llamado, si ya está en otra llamada | — |
| `HANGUP` | Cualquiera, al cortar | — |
| `PING` | Cada 2 s durante la llamada | — |
| `AUDIO` | Mientras se habla, cada 55 ms | `seq`, estado del códec, 220 B de ADPCM |

Los tiempos (`call.c`):

| Constante | Valor | Qué es |
|---|---|---|
| `RING_TIMEOUT_MS` | 30 s | Sin respuesta: se corta |
| `LOST_AFTER_MS` | 10 s | Sin noticias del otro durante una llamada: perdida |
| `SILENT_AFTER_MS` | 300 ms | Sin audio: el otro soltó la tecla |
| `PREBUFFER` | 2 bloques | Se juntan ~110 ms antes de empezar a sonar |
| `JITTER_BLOCKS` | 8 | Capacidad del buffer de recepción (~440 ms) |

### Jitter buffer

Los paquetes no llegan con el ritmo exacto con el que salieron: cada salto de la
mesh agrega un retardo variable (cola, jitter de reenvío, colisiones). Si se
reprodujera cada bloque apenas llega, el audio se cortaría cada vez que uno se
atrasa.

`play_task` espera a tener 2 bloques en la cola antes de arrancar y después
reproduce a ritmo constante (el del I2S). Un bloque que llega con una
secuencia vieja o repetida se descarta. Si la cola se vacía 150 ms, se
considera que el otro dejó de hablar y la próxima ráfaga vuelve a juntar 2 bloques.

**El trade-off:** más prebuffer = menos cortes pero más demora. 110 ms es
imperceptible en un walkie y cubre el jitter de unos pocos saltos.

### Half-duplex

Uno habla por vez (con KEY4 apretada, como un walkie-talkie). Mientras se
habla no se reproduce lo que llega. **Por qué:** el mic y el parlante están a
centímetros; en full-duplex el audio del otro saldría por el parlante, entraría
al mic y volvería: acople. Evitarlo pide cancelación de eco (AEC), que en un
microcontrolador sin DSP dedicado es un proyecto aparte.

### Limitaciones

- Las llamadas viajan por **flooding**: cada placa de la red reenvía el audio
  de cada llamada, aunque no sea para ella. Dos o tres llamadas a la vez en una
  red grande ocupan buena parte del canal.
- Solo entre placas. A un celu no se lo llama: se le manda voz (KEY4) o se usa
  el walkie por clips desde otro celu.

## Referencias de esta parte

- [I2S en ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html) (modos estándar y PDM).
- IMA Digital Audio Focus and Technical Working Groups, *Recommended Practices
  for Enhancing Digital Audio Compatibility in Multimedia Systems*, rev. 3.00, 1992 (define IMA ADPCM).
- [Datasheet del MAX98357A](https://www.analog.com/en/products/max98357a.html).
- MDN: [contextos seguros](https://developer.mozilla.org/es/docs/Web/Security/Secure_Contexts),
  [`decodeAudioData`](https://developer.mozilla.org/en-US/docs/Web/API/BaseAudioContext/decodeAudioData),
  [atributo `capture`](https://developer.mozilla.org/en-US/docs/Web/HTML/Attributes/capture),
  [política de autoplay](https://developer.mozilla.org/en-US/docs/Web/Media/Autoplay_guide).
