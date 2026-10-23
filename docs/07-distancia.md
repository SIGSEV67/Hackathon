# 07 · Distancia entre placas

Sin GPS (y el GPS del celular también pide HTTPS), la red no sabe *dónde* está
cada placa. Pero puede saber **a qué distancia está cada placa de sus
vecinas**, y con eso dibujar la **forma** de la red: quién está cerca de quién.

`components/ranging/ranging.c` mide la distancia a cada vecino directo de dos
formas.

## 1. FTM: tiempo de vuelo por WiFi

**FTM** (*Fine Timing Measurement*, IEEE 802.11mc, incluido en 802.11-2016) mide
cuánto tarda una señal en ir y volver entre dos radios:

```
Iniciador                       Respondedor
    | ---- FTM request ---------> |
    | <--- FTM_1 (sale en t1) --- |
 t2 |                             |
    | ---- ACK (sale en t3) ----> | t4
    | <--- FTM_2 (lleva t1, t4) - |
    RTT = (t4 − t1) − (t3 − t2)
    distancia = RTT · c / 2
```

Cada lado marca con su propio reloj cuándo sale y llega cada frame, así que no
hace falta que los relojes estén sincronizados: se restan los tiempos de
procesamiento. Con una ráfaga de varios intercambios se promedia. Con la luz
a 30 cm por nanosegundo, para tener precisión de metros hacen falta marcas de
tiempo de nanosegundos, y por eso tiene que hacerlo el hardware de la radio.

### Cómo lo usamos

- **Respondedor:** el AP de cada placa tiene `ftm_responder = true` (`radio.c`).
- **Iniciador:** hace falta una interfaz *station*. Por eso la radio corre en
  modo **AP+STA**: la STA **nunca se asocia** a nada, solo existe para iniciar
  sesiones FTM. Como no se asocia, nunca cambia de canal y no rompe ESP-NOW.
- **Una medición por vez, rotando entre vecinos**, cada 2,5 s
  (`ranging_task`). Cada sesión pide 16 frames
  (`esp_wifi_ftm_initiate_session`) a la MAC del vecino, que la tabla de
  vecinos de la mesh conoce porque la anotó al escucharlo.
- **Promedio móvil** de las mediciones de cada vecino (`0,6·anterior + 0,4·nueva`).
- Una medición FTM vale **30 s**; después se vuelve a la estimación por RSSI.
- Un vecino que falla 3 veces seguidas (por ejemplo, una placa sin FTM) se
  reintenta recién al minuto.
- **Calibración:** `CONFIG_SIGSEV_RANGING_FTM_OFFSET_CM` se resta a cada
  medición. Se calibra con dos placas a 1 m: se mira el log y se pone la diferencia.

Precisión esperable: **±1–2 m** en campo abierto. En interiores, los rebotes
(multitrayecto) la empeoran.

### ¿Por qué no Bluetooth (BLE) o UWB?

- **UWB** da precisión de centímetros, pero necesita un chip aparte.
- **BLE** solo da RSSI (el *channel sounding* de Bluetooth 6.0 no lo soporta el ESP32-S3).
- **FTM** viene gratis en la radio WiFi del ESP32-S3.

## 2. RSSI: estimación por potencia

Si FTM no está disponible o no hay una medición reciente, se estima la
distancia por **la potencia con la que llega la señal**, con el modelo de
pérdida log-distancia:

```
RSSI(d) = RSSI(1 m) − 10 · n · log10(d)
  ⇒  d = 10 ^ ((RSSI(1 m) − RSSI) / (10 · n))
```

| Parámetro | Default | Kconfig |
|---|---|---|
| `RSSI(1 m)` | −45 dBm | `SIGSEV_RANGING_RSSI_AT_1M` |
| `n` (exponente de pérdida) | 2,7 | `SIGSEV_RANGING_PATH_LOSS_X10` = 27 |

`n = 2` es espacio libre; entre 2,7 y 3,5 es típico al aire libre con gente
alrededor; más en interiores. El cuerpo humano absorbe mucho a 2,4 GHz, así que en
una multitud el RSSI engaña: **la estimación puede errar al doble o a la
mitad**. Por eso la web la marca distinto (línea punteada y `~`).

## 3. Cómo llega a todos

- `ranging_get(id)` devuelve la distancia a un vecino y el método (FTM o RSSI).
- `presence` la pide a través del callback `link_distance` que le inyecta
  `main.c` (así presence no depende de ranging), y la pone en cada enlace del
  beacon: `{id, rssi, dist_dm, flags}` (decímetros en 16 bits: hasta 6,5 km).
- Como los beacons se inundan, **cada placa conoce todas las distancias de la red**.

## 4. El mapa (web)

`app.js` → `updateMap`, `stepLayout`, `drawMap` dibuja un grafo con un
**layout de fuerzas**:

- Los nodos se **repelen** entre sí (fuerza ∝ 1/d²), para no superponerse.
- Cada enlace es un **resorte** cuyo largo de reposo es la distancia medida (en
  escala: el enlace más largo ocupa ~60 % del mapa). Los de FTM tiran más
  fuerte (0,06) que los de RSSI (0,03), porque son más confiables.
- Una fuerza chica hacia el centro evita que el grafo se escape.
- Se aplica un paso por frame (`requestAnimationFrame`) y las posiciones se
  conservan entre actualizaciones: el mapa se acomoda suave en vez de saltar.
- Los celus son círculos chicos pegados a su placa (no tienen distancia medida).

**Qué muestra y qué no:** la forma (distancias relativas) es real; la
orientación no (el mapa puede salir rotado o espejado), porque las
distancias solas no dicen dónde está el norte. Con 3 placas o más hay
triangulación implícita: el resorte de cada par restringe dónde pueden estar.

## Estado

Probado en placa: el FTM funciona entre las dos placas (13–14 mediciones
válidas por sesión). Con las placas juntas mide ~0 m. **Falta medir a 3–5 m**
para calibrar el offset.

## Referencias de esta parte

- IEEE Std 802.11-2016, cláusula 11.24.6 (*Fine Timing Measurement*).
- [FTM en la guía de WiFi de ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/wifi.html) (sección *Wi-Fi FTM*).
- T. S. Rappaport, *Wireless Communications: Principles and Practice*, 2.ª ed., cap. 4 (modelo log-distancia).
- T. M. J. Fruchterman, E. M. Reingold, *Graph drawing by force-directed placement*, Software: Practice and Experience, 1991.
