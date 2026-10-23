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

## Referencias de esta parte

- [GPIO e interrupciones en ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/gpio.html).
- [FreeRTOS: `xQueueSendFromISR`](https://www.freertos.org/Documentation/02-Kernel/04-API-references/06-Queues/04-xQueueSendFromISR).
- [Driver de LCD de ESP-IDF (`esp_lcd`)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/lcd/index.html).
- Datasheet del controlador SSD1306 (Solomon Systech).
