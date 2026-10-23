# Documentación de SIGSEV

Esta carpeta explica **qué hace el proyecto, cómo está hecho y por qué se
decidió así**. El [README de la raíz](../README.md) es la guía de uso rápida;
acá está el detalle para entender el código, defenderlo y armar la presentación.

Cada documento cita los archivos y funciones donde vive lo que describe
(`components/mesh/mesh.c` → `handle_frame`), así se puede ir del concepto al
código. Los números (tiempos, tamaños, límites) salen de las constantes del
código; si se cambia una constante, conviene actualizar el documento.

## Por dónde empezar

| Si querés… | Leé |
|---|---|
| Entender la idea en 5 minutos | [01 · Visión y conceptos](01-vision.md) |
| Ver cómo se organiza el código | [02 · Arquitectura](02-arquitectura.md) |
| Seguir un caso de punta a punta (qué pasa cuando apretás un botón) | [03 · Recorridos](03-recorridos.md) |
| Entender la red | [04 · Radio, mesh y presencia](04-radio-mesh-presencia.md) |
| Entender cómo viajan y se guardan los mensajes | [05 · Mensajes](05-mensajes.md) |
| Audio, voz y llamadas | [06 · Audio y llamadas](06-audio-llamadas.md) |
| Cómo se mide la distancia | [07 · Distancia](07-distancia.md) |
| El portal y la app del celular | [08 · Web](08-web.md) |
| Botones, pantalla y controles de la placa | [09 · Placa: botones y pantalla](09-placa.md) |
| El formato exacto de cada paquete | [10 · Protocolo (referencia)](10-protocolo.md) |
| Qué no anda, qué falta y qué es riesgoso | [11 · Limitaciones y trabajo futuro](11-limitaciones.md) |
| Armar la presentación | [12 · Guía para la presentación](12-presentacion.md) |
| Las fuentes | [13 · Referencias](13-referencias.md) |

## Glosario

| Término | Qué es en este proyecto |
|---|---|
| **Placa / nodo** | Un ESP32-S3 con el firmware. Forma la red y es el *bastión* de los celulares conectados a ella. |
| **Cliente / celu** | Un celular (o compu) conectado al WiFi de una placa. Tiene ID y nombre propios. |
| **Padre** | La placa de la que cuelga un cliente en este momento. |
| **ID** | 32 bits. El de una placa sale de su MAC; el de un celu lo sortea el celu. Comparten espacio: se le puede escribir a cualquiera de los dos. |
| **Mesh** | La red de placas. Sin coordinador: cada placa reenvía lo que escucha. |
| **Flooding** | Forma de difundir: cada nodo retransmite una vez cada paquete nuevo. |
| **TTL** | Cuántos saltos más puede dar un paquete. Arranca en 6. |
| **Beacon** | Paquete periódico (cada ~3 s) en el que una placa dice "estoy, me llamo así, oigo a estos vecinos". |
| **Privado** | Mensaje con destino (`dest ≠ 0`). Viaja igual que los demás, pero solo lo muestran el destino y el que lo mandó. **No está cifrado.** |
| **Custodia** | Una placa que guarda un privado ajeno para reenviárselo al destino cuando aparezca. |
| **Courier** | El módulo que hace esos reenvíos (`components/messages/courier.c`). |
| **Store-and-forward** | Guardar y reenviar más tarde, cuando hay camino. |
| **ACK** | Confirmación. Hay dos: *entregado* (✓, llegó a la placa del destino) y *leído* (✓✓, lo vio el celu destino). |
| **NACK** | Pedido de los bloques que faltan de una voz o un archivo. |
| **Bloque** | Pedazo de 220 B: 55 ms de voz comprimida o 220 B de un archivo. |
| **ADPCM** | Códec de audio simple: 4 bits por muestra. |
| **FTM** | *Fine Timing Measurement* (802.11mc): medir distancia por tiempo de vuelo del WiFi. |
| **RSSI** | Potencia con la que llega una señal (dBm). Sirve para estimar distancia, mal. |
| **Perfil** | Variante del firmware: `psram` (8 MB de RAM extra, sin pantalla) o `display` (con pantalla, sin PSRAM). |
