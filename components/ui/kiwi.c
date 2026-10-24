#include <math.h>
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "display.h"

#include "ui_internal.h"

// Easter egg: un kiwi (el pajaro) que hace el "6 7" levantando una pata y la
// otra, y despues explota. Todo se dibuja con primitivas y depende solo del
// tiempo desde que arranco cada fase, asi el dibujo no guarda estado.

#define W BOARD_LCD_WIDTH
#define H BOARD_LCD_HEIGHT

#define BEAT_MS       UI_KIWI_BEAT_MS
#define FLASH_MS      120   // destello al explotar
#define WAVE_MS       700   // onda expansiva
#define PARTICLES     48
#define GRAVITY       140.0f  // px/s^2
#define BOOM_TEXT_MS  500   // "BOOM!" aparece despues de esto

// Posicion del cuerpo cuando esta parado.
#define BODY_X  56
#define BODY_Y  31
#define BODY_RX 22
#define BODY_RY 14

static void fill_ellipse(int cx, int cy, int rx, int ry, bool on)
{
    for (int dy = -ry; dy <= ry; dy++) {
        const float f = 1.0f - (float)(dy * dy) / (float)(ry * ry);
        const int half = (int)(rx * sqrtf(f > 0 ? f : 0) + 0.5f);
        display_fill_rect(cx - half, cy + dy, 2 * half + 1, 1, on);
    }
}

// Bresenham con trazo de `thick` pixeles.
static void line(int x0, int y0, int x1, int y1, int thick)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    const int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        display_fill_rect(x0, y0, thick, thick, true);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void circle(int cx, int cy, int r)
{
    // Puntos sueltos cada pocos grados: una onda punteada se lee mejor que una linea.
    for (int deg = 0; deg < 360; deg += 6) {
        const float a = deg * (float)M_PI / 180.0f;
        display_set_pixel(cx + (int)(r * cosf(a)), cy + (int)(r * sinf(a) * 0.6f), true);
    }
}

// Pata desde la cadera: estirada hasta el piso, o levantada con la rodilla doblada.
static void leg(int hip_x, int hip_y, bool up)
{
    const int floor_y = H - 4;
    if (up) {
        const int knee_x = hip_x + 5, knee_y = hip_y + 5;
        const int foot_x = hip_x + 1, foot_y = hip_y + 9;
        line(hip_x, hip_y, knee_x, knee_y, 2);
        line(knee_x, knee_y, foot_x, foot_y, 2);
        line(foot_x - 3, foot_y, foot_x + 5, foot_y, 1);  // dedos
    } else {
        line(hip_x, hip_y, hip_x, floor_y, 2);
        line(hip_x - 3, floor_y + 1, hip_x + 6, floor_y + 1, 1);
        line(hip_x + 1, floor_y + 1, hip_x - 2, floor_y + 3, 1);
    }
}

static void draw_bird(int x, int y, bool left_up, bool right_up)
{
    // Cuerpo con textura de plumas (pixeles apagados sueltos).
    fill_ellipse(x, y, BODY_RX, BODY_RY, true);
    for (int py = y - BODY_RY + 3; py < y + BODY_RY - 2; py++) {
        for (int px = x - BODY_RX + 4; px < x + BODY_RX - 4; px++) {
            if ((px * 3 + py * 7) % 13 == 0) {
                display_set_pixel(px, py, false);
            }
        }
    }
    // Cabeza, ojo y el pico largo y finito del kiwi.
    const int hx = x + BODY_RX - 2, hy = y - 7;
    fill_ellipse(hx, hy, 7, 6, true);
    display_fill_rect(hx + 2, hy - 2, 2, 2, false);
    line(hx + 6, hy + 1, hx + 26, hy + 10, 2);

    leg(x - 7, y + BODY_RY - 1, left_up);
    leg(x + 5, y + BODY_RY - 1, right_up);
}

static void draw_dance(uint32_t ms)
{
    // Una pata por tiempo: la izquierda dice "6", la derecha "7".
    const bool six = (ms / BEAT_MS) % 2 == 0;
    const int bob = (ms % BEAT_MS) < BEAT_MS / 2 ? -1 : 0;  // rebote chiquito en cada tiempo
    display_clear();
    draw_bird(BODY_X, BODY_Y + bob - (six ? 0 : 1), six, !six);
    display_text_scaled(six ? 2 : W - 17, 6, six ? "6" : "7", 3, true);
}

// Pseudoazar reproducible: cada frame recalcula las mismas particulas desde la semilla.
static uint32_t next_rand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void draw_boom(uint32_t ms, uint32_t seed)
{
    display_clear();
    if (ms < FLASH_MS) {
        display_fill_rect(0, 0, W, H, true);
        return;
    }
    const float t = (ms - FLASH_MS) / 1000.0f;
    uint32_t s = seed | 1;
    for (int i = 0; i < PARTICLES; i++) {
        // Sale de algun punto del cuerpo, hacia afuera y un poco para arriba.
        const float a = (next_rand(&s) % 360) * (float)M_PI / 180.0f;
        const float r = (next_rand(&s) % 100) / 100.0f;
        const float speed = 35.0f + (next_rand(&s) % 70);
        const float x0 = BODY_X + cosf(a) * BODY_RX * r;
        const float y0 = BODY_Y + sinf(a) * BODY_RY * r;
        const float x = x0 + cosf(a) * speed * t;
        const float y = y0 + (sinf(a) * speed - 40.0f) * t + 0.5f * GRAVITY * t * t;
        const int size = t < 0.5f ? 3 : (t < 1.0f ? 2 : 1);
        display_fill_rect((int)x, (int)y, size, size, true);
    }
    if (ms - FLASH_MS < WAVE_MS) {
        const int r = 4 + (int)((ms - FLASH_MS) * 0.11f);
        circle(BODY_X, BODY_Y, r);
        circle(BODY_X, BODY_Y, r / 2);
    }
    if (ms > BOOM_TEXT_MS) {
        display_text_scaled((W - 5 * DISPLAY_FONT_W * 2) / 2, 2, "BOOM!", 2, true);
    }
}

static void draw_restart(void)
{
    static const char *const text = "reiniciando...";
    display_clear();
    display_text((W - (int)strlen(text) * DISPLAY_FONT_W) / 2, H / 2 - 4, text, false);
}

void kiwi_draw(ui_kiwi_t phase, uint32_t elapsed_ms, uint32_t seed)
{
    switch (phase) {
    case UI_KIWI_DANCE:
        draw_dance(elapsed_ms);
        break;
    case UI_KIWI_BOOM:
        draw_boom(elapsed_ms, seed);
        break;
    case UI_KIWI_RESTART:
        draw_restart();
        break;
    case UI_KIWI_OFF:
        break;
    }
}
