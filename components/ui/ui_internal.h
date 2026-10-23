#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "call.h"
#include "messages.h"

// Estado de la UI que comparten ui.c (eventos) y screens.c (dibujo).

typedef enum {
    SCREEN_HOME,
    SCREEN_MESSAGES,
    SCREEN_NETWORK,
    SCREEN_COUNT,
} screen_t;

typedef enum {
    TOAST_NONE,
    TOAST_ALERT,     // alerta recibida: pantalla completa, se cierra con el joystick
    TOAST_SENT,      // confirmacion de algo propio
    TOAST_INCOMING,  // llego un mensaje (texto o voz)
    TOAST_DELIVERED, // un privado nuestro llego a destino
    TOAST_CALL_END,  // termino una llamada (toast_msg no se usa)
} toast_kind_t;

typedef struct {
    screen_t screen;
    int scroll;              // cursor: en Mensajes, la fila; en Red, 0 = Todos y 1.. = nodos
    mesh_node_id_t selected; // nodo bajo el cursor en Red (lo escribe el task de dibujo)
    uint32_t selected_msg;   // mensaje bajo el cursor en Mensajes (idem)
    toast_kind_t toast;
    uint32_t toast_until_ms;
    message_t toast_msg;
    call_info_t toast_call;  // TOAST_CALL_END
} ui_view_t;

// Etiquetas de KEY1..KEY4 que paso la aplicacion (fijas despues de ui_init).
extern const char *g_key_labels[4];

// Solo desde el task de la UI (usan buffers estaticos compartidos).

typedef struct {
    mesh_node_id_t node;  // Red: nodo bajo el cursor (MESSAGES_TO_ALL si no hay)
    uint32_t message;     // Mensajes: mensaje bajo el cursor (0 si no hay)
} ui_pick_t;

// Dibuja un frame completo de la vista (sin flush). view->scroll ya viene acotado.
// Devuelve lo que quedo bajo el cursor.
ui_pick_t screens_draw(const ui_view_t *view);

// Cantidad de filas que tiene la pantalla, para acotar el scroll.
int screens_rows(screen_t screen);
