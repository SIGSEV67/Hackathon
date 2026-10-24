# 12 · Guía para la presentación

Material para armar la presentación: el hilo del relato, la demo, los números
que conviene tener a mano y las preguntas probables. No es un guion cerrado.

## El hilo en una frase

> Cuando la red celular colapsa, SIGSEV arma una red propia con placas baratas:
> cualquier celular se conecta sin instalar nada y puede pedir ayuda, escribir y
> mandar audios, incluso a alguien que ahora no está al alcance.

## Estructura sugerida (~7 min)

1. **El problema** (30 s). Una marcha, antenas saturadas, nadie puede avisar
   que hay un herido.
2. **La idea** (1 min). Placas que se pasan los mensajes entre sí, sin
   infraestructura. Los celulares son clientes: se conectan al WiFi de la placa
   más cercana. La placa también se usa sola, con botones.
3. **Demo** (3 min). Ver abajo.
4. **Cómo funciona** (1,5 min). Tres ideas, una diapositiva cada una:
   - **Flooding:** cada placa reenvía una vez lo que escucha (dedup, TTL, jitter).
   - **Custodia:** si el destino no está, las placas lo guardan y se lo pasan cuando aparece. ✓ y ✓✓.
   - **Radio rápida:** ESP-NOW a 1 Mbps permite voz, cosa que LoRa no.
5. **Lo que lo hace distinto** (30 s). La tabla contra LoRa (documento 01):
   voz, cualquier navegador, distancia medida, sin hardware extra.
6. **Honestidad** (30 s). No está cifrado y el flooding no escala a cientos de
   placas; contamos qué haríamos (documento 11). Muestra que entendemos el
   sistema, no solo que lo hicimos andar.

## Demo

Preparación: las dos placas con el perfil `display` (o tres, si hay), dos celulares.
Antes de presentar, conectar cada celu a una placa distinta, abrir la app y
ponerles nombre (*Vos → Tu nombre*).

| # | Acción | Qué se ve | Qué contar |
|---|---|---|---|
| 1 | Prender las placas | En ~3 s cada una ve a la otra (pantalla de inicio: "Red 2") | Se descubren solas, sin configuración |
| 2 | Conectar un celu al WiFi `SIGSEV-…` | Se abre el portal solo | Sin app, sin internet, sin contraseña |
| 3 | Mantener KEY1 en una placa | Alerta SOS a pantalla completa en la otra; banner y vibración en los celus | Un botón físico, para quien no tiene celular |
| 4 | Pestaña *Red* en el celu | Mapa con las placas, la distancia y el ruido; los celus colgando de su placa | La red conoce su propia forma |
| 5 | Celu A → privado a celu B | En A: *en camino…* → ✓ → ✓✓ | Las dos tildes |
| 6 | Apagar la placa de B, mandarle otro privado desde A, volver a prenderla | El mensaje llega solo cuando B vuelve | Store-and-forward: el destino no tenía que estar |
| 7 | Grabar un audio en un celu y mandarlo | Suena en el otro celu (y en la placa, con *▶ en la placa*) | 15 s de voz salen en ~2 s; con LoRa, minutos |
| 8 | (Con parlante) Llamada entre placas con KEY1 + KEY4 | Walkie en vivo | Voz en tiempo real por la mesh |
| 9 | Cierre: código Konami en una placa (o `tools/konami.py`) | El kiwi baila el "6 7", explota y **todas** las placas lo repiten y se reinician | Un solo paquete llega a toda la red en milisegundos; y se vuelven a encontrar solas en ~3 s |

Plan B si algo falla en vivo: tener grabado un video corto de la demo y las
capturas de la web con la API simulada (`docs/` + un servidor mock).

## Números para tener a mano

| | |
|---|---|
| Tamaño máximo de un paquete | 250 B (12 de header + 238 de datos) |
| Saltos máximos | 6 |
| Beacon | cada 3 s; una placa se da por caída a los 12 s |
| Descubrimiento | ~3 s |
| Voz | 8 kHz, ADPCM 4 bits = 32 kbps; bloques de 55 ms independientes |
| Voz de 20 s | ~80 KB, ~364 paquetes, sale en ~2,5 s |
| Memoria de mensajes | 48 mensajes; voz: ~20 s sin PSRAM, ~15 min con PSRAM |
| Reintentos de un privado | espera al azar 0,5–3 s al aparecer el destino; cada 20 s, hasta 5 veces |
| Distancia | FTM ±1–2 m al aire libre; RSSI ±×2 |
| Celulares por placa | 4 (configurable hasta 10) |
| Código | ~7500 líneas de C (firmware: 14 componentes + main) y ~730 de JS (app) |

## Preguntas probables

**¿Por qué no LoRa?**
LoRa llega más lejos y consume menos, pero transmite ~0,3–20 kbps. Una voz de
20 s tardaría minutos. Con ESP-NOW sale en segundos, y el celular se conecta por
WiFi desde el navegador sin una app. En una marcha la gente está densa: el
alcance de 100–200 m por salto alcanza.

**¿Cuánto alcance tiene?**
~100–200 m por salto en línea de vista, menos entre gente (el cuerpo absorbe
2,4 GHz). Con 6 saltos, del orden de un kilómetro si las placas están bien distribuidas.

**¿Es seguro? ¿La policía puede leer los mensajes?**
Hoy sí: nada está cifrado y lo decimos. Los privados son dirigidos, no
secretos. El plan (documento 11) es cifrado de punta a punta entre celulares
con claves generadas en el navegador.

**¿Qué pasa si se cae una placa?**
Nada especial: no hay nodo central. Los mensajes siguen por las demás y, si
justo era el único camino, los privados esperan en custodia.

**¿Cuántas placas soporta?**
Decenas. El flooding hace que cada paquete lo retransmita cada placa: con
muchas placas y mucha voz se satura el canal. Para alertas y texto escala mucho
más. Hay mejoras conocidas (documento 11).

**¿Cómo saben las distancias?**
Con FTM, una función del WiFi que mide el tiempo de ida y vuelta de la señal
(±1–2 m). Si no hay, se estima por la potencia de la señal. Con las
distancias entre todas las placas se dibuja la forma de la red.

**¿Por qué el celular no habla directo con otros celulares?**
Un celular no puede hacer ESP-NOW ni rutear desde un navegador. La placa es su
puerta a la red; por eso el modelo es "placa = bastión, celu = cliente".

**¿Por qué no hay llamadas en vivo desde el celular?**
El navegador solo da el micrófono en HTTPS, y sin internet no hay certificado
válido. Por eso usamos el grabador del sistema y el *walkie por clips*.

**¿Cuánto cuesta?**
Una placa ESP32-S3 con pantalla y micrófono, más un amplificador I2S opcional.
No hay módulo de radio aparte.

## Qué mostrar del código (si preguntan)

- `components/mesh/mesh.c` → `handle_frame`: el flooding en 40 líneas.
- `components/messages/courier.c` → `on_node_joined` y `tick`: la custodia y la supresión.
- `components/periph/buttons.c` → `s_joy_dir`: el joystick rotado, corregido con vectores (una anécdota simpática).
- El grafo de dependencias de [02 · Arquitectura](02-arquitectura.md): las capas las verifica el build.
