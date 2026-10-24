# 09 · La placa: botones, joystick y pantalla

La placa es un usuario más de la red. Este documento cubre su interfaz física:
`periph/buttons.c`, `main/controls.c`, `periph/display.c` y `components/ui/`.

## 1. Botones

Hay 9 entradas: los 4 contactos del joystick, el centro del joystick (no
conectado en esta placa) y 4 teclas. Todas con **pull-up interno**: en reposo
leen 1 y al apretar cierran a masa (0).

### Interrupción + task, no polling

```
GPIO cambia ─► button_isr (IRAM) ─► cola ─► task buttons ─► debounce 20 ms ─► BUTTONS_EVENT
```

- **`button_isr`** corre en contexto de interrupción: no puede bloquear ni usar
  la mayoría de las funciones. Deshabilita la interrupción de ese pin (para que
  el rebote del contacto no la dispare 50 veces) y encola el ID con
  `xQueueSendFromISR`. Si eso despertó a un task de más prioridad, pide el cambio
  de contexto (`portYIELD_FROM_ISR`) para que no espere al próximo tick.
- **El task `buttons`** (prioridad 10, la más alta del proyecto) espera 20 ms a
  que el contacto se asiente, rehabilita la interrupción, lee el nivel estable
  y publica `PRESSED` o `RELEASED`.

**Por qué no leer los pines en un loop:** con interrupciones el CPU no hace nada
mientras nadie toca un botón, y la respuesta es inmediata.

### El joystick rotado 45°

El joystick está soldado girado: cada contacto quedó en una **diagonal física**.
Empujar el stick hacia arriba cierra los contactos RIGHT y DOWN a la vez.

La corrección (`buttons.c`) es vectorial: cada contacto tiene asignada su
dirección física real,

```c
static const struct { int8_t x, y; } s_joy_dir[] = {
    [BTN_UP]    = {-1, -1},
    [BTN_DOWN]  = {+1, +1},
    [BTN_LEFT]  = {+1, -1},
    [BTN_RIGHT] = {-1, +1},
};
```

y `joystick_read` **suma los vectores de los contactos cerrados**: dos
contactos vecinos dan una dirección cardinal (RIGHT + DOWN = (0, +2) = arriba),
uno solo da una diagonal. Además, al llegar la interrupción de cualquier
contacto del joystick se releen los cuatro juntos después del debounce, así dos
contactos que cierran casi a la vez generan un solo evento.

Esta tabla está **verificada en la placa** en las cuatro direcciones. No hay
que cambiarla sin volver a probar.

## 2. Qué hace cada tecla (`main/controls.c`)

```
            ┌──────┐
            │ KEY4 │  ← apartada, arriba a la derecha: VOZ
            └──────┘
 ┌──────┐ ┌──────┐ ┌──────┐
 │ KEY1 │ │ KEY2 │ │ KEY3 │  ← en fila, abajo
 └──────┘ └──────┘ └──────┘
```

| Tecla | Mantener (1 s) | Toque corto |
|---|---|---|
| KEY1 | Alerta **SOS** | Llamar a la placa marcada en *Red* · atender · colgar |
| KEY2 | Alerta **MÉDICO** | Rechazar una llamada entrante |
| KEY3 | Alerta **PELIGRO** | — |
| KEY4 | Grabar voz mientras se mantiene (a todos, o privada a quien esté marcado en *Red*) · en llamada: hablar | Sobre una voz en *Mensajes*: escucharla o pararla |
| Joystick ← → | Cambiar de pantalla | |
| Joystick ↑ ↓ | Mover el cursor · cerrar una alerta | |

**Toque vs. mantener.** Al apretar KEY1–3 se arranca un timer de 1 s. Si vence
con la tecla apretada, es una alerta. Si la tecla se suelta antes (el timer
sigue activo: `esp_timer_is_active`), es un toque. Una misma tecla tiene dos
funciones sin que una dispare la otra.

**Por qué KEY4 es la voz:** es la tecla apartada de las otras tres, la más
fácil de encontrar a ciegas, y la de grabar es la que se mantiene apretada más tiempo.

**Por qué las alertas no se mandan con un toque:** una falsa alarma de SOS en
una multitud puede ser peor que no tener alarma.

## 3. La pantalla

OLED SSD1306 monocromo de 128×64, por SPI a 10 MHz (`periph/display.c`).

- **Framebuffer** de 1 KB (128 × 64 / 8): se dibuja en RAM y `display_flush` lo
  manda entero. Simple y sin parpadeo.
- **Fuente de 5×7** ASCII. Los acentos y la ñ se transliteran (`to_ascii` en
  `screens.c`: "Médico" → "Medico"); lo demás no-ASCII sale como `?`.
- Primitivas mínimas: píxel, rectángulo lleno o vacío, texto (normal, invertido o ampliado).

### Arquitectura de la UI

```
eventos (botones, mensajes, llamadas) ──► ui.c: modifica s_view (bajo lock)
                                               │
                   task ui (10 fps) ◄──────────┘ copia s_view y dibuja la copia
                        │
                        └─► screens.c: screens_draw(vista) ─► display_flush
```

- **`ui.c`** recibe eventos y actualiza un estado chico, `ui_view_t`: pantalla
  actual, cursor, aviso activo (*toast*). Lo protege con una sección crítica.
- **El task `ui`** copia la vista, la dibuja con `screens_draw` y espera 100 ms.
  Dibujar una copia evita sostener el lock mientras se dibuja.
- **`screens.c`** es una función de la vista: no guarda estado propio entre
  frames. Devuelve qué quedó bajo el cursor (`ui_pick_t`), que es lo que
  `controls.c` consulta con `ui_selected_node()` y `ui_selected_message()`.

**Por qué redibujar todo cada 100 ms** en vez de solo lo que cambió: la
pantalla es chica (1 KB), el SPI la manda en ~1 ms, y se evita toda la lógica
de "qué hay que invalidar". Los datos (edades, ruido, llamada en curso) cambian solos.

### Pantallas

| Pantalla | Título | Contenido |
|---|---|---|
| **Inicio** | Nombre de la placa | Vecinos y nodos en la red, barra de ruido, último mensaje y las teclas dibujadas en su posición física (se rellenan al apretarlas) |
| **Mensajes** | `MENSAJES` (o `MSJ 4:oir` / `4:parar` sobre una voz) | Lista con marca de tipo: `!` alerta, `@` privado, `>` voz (`*` sonando), `#` archivo, `"` texto. Las alertas graves van invertidas |
| **Red** | `RED` + el ID propio (para dictarlo) | `Todos`, después el historial: placas con ruido y saltos, celus como `Nombre@Placa`, los que no están con `--` y hace cuánto |

Superpuestos, con prioridad de arriba hacia abajo:

1. **Grabando**: pantalla invertida con los segundos y una barra hasta el máximo.
2. **Alerta recibida**: pantalla completa (invertida salvo "ESTOY BIEN"), de quién y a cuántos saltos. Se cierra con el joystick o a los 15 s.
3. **Llamada**: LLAMANDO / LLAMADA (con "1: atender 2: no") / en curso con el tiempo y AL AIRE, ESCUCHANDO o EN LÍNEA.
4. **Avisos chicos** (recuadro): enviado, llegó algo, entregado, llamada terminada (con el motivo).

Una alerta no se tapa con un aviso menor (`show_toast`).

### Sin pantalla

En el perfil `psram` no hay display: `ui_init` arranca en su lugar un task que
cada 10 s loguea por serie un resumen ("3 nodes in mesh, 2 direct neighbors,
noise 41"). Las funciones de dibujo son no-op y nunca tocan los GPIO 36/37.

## 4. Easter egg: el kiwi

Con el código Konami el kiwi (el pájaro) baila el "6 7" con las patas y
explota. Esa placa le avisa a la red, **todas las demás hacen lo mismo** y
después se reinician todas. Vive en `components/easter/` y se puede sacar
entero con *menuconfig → SIGSEV: easter egg* (`CONFIG_SIGSEV_EASTER_EGG`,
prendido por defecto).

### Cómo se dispara

- **En la placa:** ↑ ↑ ↓ ↓ ← → ← → B A. La placa no tiene B ni A: **las dos
  son KEY2** (la del medio de la fila). Hay que hacerlo con menos de 3 s entre
  entrada y entrada.
- **Desde la compu**, porque el joystick no es muy preciso: con la placa por
  USB, `tools/konami.py [puerto]` manda la línea `konami` por el USB serie.
  Alcanza con una placa; el resto se entera por la mesh.

Las teclas del código **siguen haciendo lo de siempre** (el joystick mueve
pantallas y cursores; KEY2 con toque corto solo rechaza una llamada entrante):
el detector solo mira, no se come los eventos. Por eso B y A tienen que ser
**toques**: mantener KEY2 1 s manda la alerta MÉDICO.

### Cómo se detecta (`konami.c`)

Escucha `BUTTONS_EVENT_PRESSED` como un suscriptor más del bus de eventos, sin
que `controls.c` ni la UI se enteren. Guarda **las últimas 10 entradas** y las
compara enteras contra el código cada vez que llega una. Un autómata que
vuelve a cero al primer error parecería más simple, pero pierde casos como
"↑ ↑ ↑ ↓ ↓ …" (el tercer ↑ lo resetea y ya no hay dos ↑ al principio); con la
ventana deslizante, cualquier secuencia que *termine* en el código vale.

Dos detalles por el joystick:

- **Rotado 45°** (ver [arriba](#el-joystick-rotado-45)): una dirección mal
  apuntada llega como dos botones casi a la vez. Si una dirección llega a menos
  de 60 ms de la anterior, se ignora: vale la primera.
- **Una pausa de más de 3 s** borra lo acumulado, para que restos de navegar
  la UI no completen un código por accidente.

### Comandos por USB (`serial.c`)

La consola de la placa es el USB-Serial-JTAG del ESP32-S3. Para *leer* de ahí
hay que instalar su driver (`usb_serial_jtag_driver_install`) y pasarle la
consola (`usb_serial_jtag_vfs_use_driver`). Efecto secundario bueno: con el
driver, si no hay una compu leyendo, el log descarta lo que no sale en 50 ms en
vez de trabarse.

El task `serial_cmd` junta bytes hasta un fin de línea y compara (sin
mayúsculas ni espacios) con `konami`. Cualquier otra cosa loguea *unknown
command*. `tools/konami.py` abre el puerto **sin tocar DTR/RTS** (esa
combinación es la que resetea la placa), manda la línea y muestra 2 s de log
para confirmar que llegó.

Se eligió USB y no un botón en la web a propósito: así hace falta tener la
placa en la mano; desde un celu no se puede reiniciar la red.

### La secuencia (`easter.c`)

| Fase | Duración | Pantalla (`ui_kiwi`) | Sonido (si hay parlante) |
|---|---|---|---|
| Baile | 8 tiempos de 300 ms (2,4 s) | El kiwi levanta la pata izquierda con un "6" grande, después la derecha con un "7" | Un tono por tiempo: mi para el 6, sol para el 7 |
| Explosión | 1,8 s | Destello, 48 partículas que salen del cuerpo y caen, onda expansiva y "BOOM!" | Ruido filtrado que se apaga en 0,9 s |
| Reinicio | 0,7 s | "reiniciando..." | — |

Después, `esp_restart()`.

La placa donde se hizo el código (la *líder*) manda el aviso **al explotar**,
no al empezar: así las otras empiezan a bailar justo cuando la líder explota, y
el efecto es una ola que recorre la red. Las seguidoras hacen la secuencia
entera y se reinician ~2,5 s después que la líder.

Corre en su propio task (`easter`), que duerme en una notificación hasta que
hace falta. Ni el handler de la mesh ni el de los botones bloquean: solo
despiertan al task (la regla de [02](02-arquitectura.md#concurrencia-los-tasks)).

### El aviso a la red

Un paquete `KIWI` con 4 bytes de magic ([10 · Protocolo](10-protocolo.md#kiwi--eastereasterc)),
mandado **3 veces** porque el broadcast de ESP-NOW no tiene confirmación. Viaja
por flooding como cualquier otro: llega a toda la red, no solo a los vecinos.

Lo que evita que esto se descontrole:

- **Las seguidoras no reenvían el aviso como propio.** La mesh lo reenvía una
  vez como a cualquier paquete, pero solo la líder lo origina: un aviso no genera más avisos.
- **`s_active`**: una placa que ya está en la secuencia ignora las otras copias.
- **15 s de gracia al arrancar.** Al reiniciarse, la tabla de deduplicación
  arranca vacía: una copia que siga rebotando por una placa más lenta podría
  volver a dispararla, y así para siempre. Recién arrancada, la placa ignora `KIWI`.

### El dibujo (`ui/kiwi.c`)

El kiwi se dibuja con primitivas (elipses rellenas, líneas de Bresenham con
grosor, píxeles sueltos), sin bitmaps: cuerpo con textura de plumas, cabeza,
ojo, el pico largo y fino, y patas con rodilla. Cada frame depende **solo del
tiempo desde que arrancó la fase** (y, en la explosión, de una semilla), así
el dibujo no guarda estado: las partículas se recalculan en cada frame desde la
misma semilla con un generador lineal congruencial, y siempre caen igual.

`ui_kiwi(fase)` es la única entrada: guarda la fase en la vista y el task de la
UI, mientras la fase no es `OFF`, dibuja eso a ~30 fps (33 ms por frame) en vez
de las pantallas normales. Por eso tapa todo, incluso una alerta. Sin display
(perfil `psram`) la secuencia corre igual, se loguea y se reinicia: solo no se ve.

## Referencias de esta parte

- [GPIO e interrupciones en ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/gpio.html).
- [FreeRTOS: `xQueueSendFromISR`](https://www.freertos.org/Documentation/02-Kernel/04-API-references/06-Queues/04-xQueueSendFromISR).
- [Driver de LCD de ESP-IDF (`esp_lcd`)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/lcd/index.html).
- Datasheet del controlador SSD1306 (Solomon Systech).
