#pragma once

#include <stdbool.h>
#include <stdint.h>

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
} toast_kind_t;

typedef struct {
    screen_t screen;
    int scroll;
    toast_kind_t toast;
    uint32_t toast_until_ms;
    message_t toast_msg;
} ui_view_t;

// Etiquetas de KEY1..KEY4 que paso la aplicacion (fijas despues de ui_init).
extern const char *g_key_labels[4];

// Solo desde el task de la UI (usan buffers estaticos compartidos).

// Dibuja un frame completo de la vista (sin flush). view->scroll ya viene acotado.
void screens_draw(const ui_view_t *view);

// Cantidad de filas que tiene la pantalla, para acotar el scroll.
int screens_rows(screen_t screen);
