#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "presence.h"
#include "sdkconfig.h"

#include "ui_internal.h"

#define W        BOARD_LCD_WIDTH
#define H        BOARD_LCD_HEIGHT
#define COLS     (W / DISPLAY_FONT_W)
#define ROW_H    9
#define LIST_Y   11
#define LIST_ROWS 6
#define MAX_ROWS 24

static const char *const s_titles[SCREEN_COUNT] = {"", "MENSAJES", "RED"};

// Buffers grandes fuera del stack. Todo este archivo corre solo en el task de
// la UI, asi que no hace falta lock.
static presence_node_t s_nodes[MAX_ROWS + 1];
static message_t s_msgs[MAX_ROWS];

// La fuente es ASCII: pasa las letras con tilde y la enie a su version simple
// (UTF-8 de 2 bytes) y el resto de lo no-ASCII a '?'.
static void to_ascii(const char *in, char *out, size_t len)
{
    static const char *const from[] = {"á", "é", "í", "ó", "ú", "ñ", "Á", "É", "Í", "Ó", "Ú", "Ñ", "ü", "Ü"};
    static const char to[] = "aeiounAEIOUNuU";
    size_t o = 0;
    while (*in && o + 1 < len) {
        if ((unsigned char)*in < 0x80) {
            out[o++] = *in++;
            continue;
        }
        char c = '?';
        for (size_t i = 0; i < sizeof(to) - 1; i++) {
            if (strncmp(in, from[i], 2) == 0) {
                c = to[i];
                break;
            }
        }
        out[o++] = c;
        in++;
        while ((*in & 0xc0) == 0x80) {
            in++;  // bytes de continuacion
        }
    }
    out[o] = '\0';
}

static void text(int x, int y, const char *s, bool invert)
{
    char buf[COLS * 2 + 1];
    to_ascii(s, buf, sizeof(buf));
    if ((int)strlen(buf) > (W - x) / DISPLAY_FONT_W) {
        buf[(W - x) / DISPLAY_FONT_W] = '\0';
    }
    display_text(x, y, buf, invert);
}

static void text_center(int y, const char *s, int scale, bool on)
{
    char buf[COLS + 1];
    to_ascii(s, buf, sizeof(buf));
    const int max = W / (DISPLAY_FONT_W * scale);
    if ((int)strlen(buf) > max) {
        buf[max] = '\0';
    }
    const int x = (W - (int)strlen(buf) * DISPLAY_FONT_W * scale) / 2;
    display_text_scaled(x, y, buf, scale, on);
}

static void ago(uint32_t ms, char *out, size_t len)
{
    const uint32_t s = ms / 1000;
    if (s < 60) {
        snprintf(out, len, "%" PRIu32 "s", s);
    } else if (s < 3600) {
        snprintf(out, len, "%" PRIu32 "m", s / 60);
    } else {
        snprintf(out, len, "%" PRIu32 "h", s / 3600);
    }
}

static void describe(const message_t *m, char *out, size_t len)
{
    switch (m->kind) {
    case MESSAGE_ALERT:
        snprintf(out, len, "%s", messages_alert_label(m->alert));
        break;
    case MESSAGE_TEXT:
        snprintf(out, len, "%s", m->text);
        break;
    case MESSAGE_VOICE:
        snprintf(out, len, "VOZ %us", m->voice_blocks * MESSAGES_VOICE_BLOCK_SAMPLES / MESSAGES_VOICE_SAMPLE_RATE);
        break;
    }
}

static void header(const ui_view_t *view)
{
    char name[PRESENCE_NAME_MAX + 1];
    presence_self_name(name, sizeof(name));
    const size_t n = presence_nodes(s_nodes, MAX_ROWS);
    char right[12];
    snprintf(right, sizeof(right), "%u %d/%d", (unsigned)n + 1, view->screen + 1, SCREEN_COUNT);

    display_fill_rect(0, 0, W, 9, true);
    const int right_x = W - (int)strlen(right) * DISPLAY_FONT_W - 1;
    const char *title = view->screen == SCREEN_HOME ? name : s_titles[view->screen];
    char buf[COLS + 1];
    to_ascii(title, buf, sizeof(buf));
    buf[(right_x - 2) / DISPLAY_FONT_W] = '\0';
    display_text(1, 1, buf, true);
    display_text(right_x, 1, right, true);
}

// --- pantallas ------------------------------------------------------------------

static void draw_home(void)
{
    presence_node_t self;
    presence_self(&self);
    const size_t n = presence_nodes(s_nodes, MAX_ROWS);

    char line[COLS * 2 + 1];  // text() recorta al ancho de la pantalla
    snprintf(line, sizeof(line), "Vecinos %u  Red %u", self.n_links, (unsigned)n + 1);
    text(0, 12, line, false);

    text(0, 23, "Ruido", false);
    display_draw_rect(34, 22, W - 34, 9);
    display_fill_rect(36, 24, self.noise * (W - 38) / 100, 5, true);

    message_t last;
    if (messages_list(&last, 1, 0) == 1) {
        char what[COLS + 1], from[PRESENCE_NAME_MAX + 1], when[8];
        describe(&last, what, sizeof(what));
        presence_name_of(last.origin, from, sizeof(from));
        ago(mesh_now_ms() - last.received_ms, when, sizeof(when));
        text(0, 35, what, last.kind == MESSAGE_ALERT && last.alert != ALERT_SAFE);
        snprintf(line, sizeof(line), "%s %s", when, last.hops == MESSAGE_HOPS_SELF ? "(vos)" : from);
        text(0, 45, line, false);
    } else {
        text(0, 35, "Sin mensajes", false);
    }

    display_fill_rect(0, H - 8, W, 8, false);
    text(0, H - 8, "1VOZ 2SOS 3MED 4PELIG", false);
}

static void draw_messages(int scroll)
{
    const size_t total = messages_list(s_msgs, MAX_ROWS, 0);
    if (!total) {
        text(0, LIST_Y + 10, "Sin mensajes todavia", false);
        return;
    }
    const uint32_t now = mesh_now_ms();
    for (size_t i = scroll, row = 0; i < total && row < LIST_ROWS; i++, row++) {
        const message_t *m = &s_msgs[i];
        char what[COLS + 1], when[8], line[COLS * 2 + 1];
        describe(m, what, sizeof(what));
        ago(now - m->received_ms, when, sizeof(when));
        const char mark = m->kind == MESSAGE_ALERT ? '!' : (m->kind == MESSAGE_VOICE ? '>' : '"');
        snprintf(line, sizeof(line), "%c%-3s %s", mark, when, what);
        text(0, LIST_Y + row * ROW_H, line, m->kind == MESSAGE_ALERT && m->alert != ALERT_SAFE);
    }
}

static void draw_network(int scroll)
{
    presence_self(&s_nodes[0]);
    const size_t total = 1 + presence_nodes(&s_nodes[1], MAX_ROWS);
    for (size_t i = scroll, row = 0; i < total && row < LIST_ROWS; i++, row++) {
        char line[COLS * 2 + 1], name[11];
        to_ascii(s_nodes[i].name, name, sizeof(name));
        if (i == 0) {
            snprintf(line, sizeof(line), "%-10s r%-3u *", name, s_nodes[i].noise);
        } else {
            snprintf(line, sizeof(line), "%-10s r%-3u %us", name, s_nodes[i].noise, s_nodes[i].hops + 1);
        }
        text(0, LIST_Y + row * ROW_H, line, false);
    }
}

// --- avisos encima de la pantalla ----------------------------------------------------

static void draw_recording(uint32_t elapsed_ms)
{
    display_clear();
    display_fill_rect(0, 0, W, H, true);
    text_center(6, "GRABANDO", 2, false);
    char secs[12];
    snprintf(secs, sizeof(secs), "%" PRIu32 "s", elapsed_ms / 1000);
    text_center(28, secs, 2, false);
    const uint32_t max_ms = CONFIG_SIGSEV_VOICE_MAX_S * 1000;
    const uint32_t shown = elapsed_ms < max_ms ? elapsed_ms : max_ms;
    display_fill_rect(8, 50, W - 16, 8, false);
    display_draw_rect(8, 50, W - 16, 8);
    display_fill_rect(10, 52, (int)((W - 20) * shown / max_ms), 4, true);
}

static void draw_alert(const message_t *m)
{
    char from[PRESENCE_NAME_MAX + 1], line[COLS + 1];
    presence_name_of(m->origin, from, sizeof(from));
    const bool calm = m->alert == ALERT_SAFE;
    display_clear();
    if (!calm) {
        display_fill_rect(0, 0, W, H, true);
    }
    text_center(2, "ALERTA", 1, calm);
    text_center(14, messages_alert_label(m->alert), 2, calm);
    text_center(36, from, 1, calm);
    snprintf(line, sizeof(line), m->hops == 0 ? "vecino directo" : "a %u saltos", m->hops + 1);
    text_center(46, line, 1, calm);
    text_center(56, "joystick: cerrar", 1, calm);
}

static void draw_box(const char *title, const char *body)
{
    display_fill_rect(6, 14, W - 12, 36, false);
    display_draw_rect(6, 14, W - 12, 36);
    display_draw_rect(8, 16, W - 16, 32);
    text_center(20, title, 1, true);
    text_center(34, body, 1, true);
}

void screens_draw(const ui_view_t *view)
{
    uint32_t rec_ms;
    if (messages_voice_recording(&rec_ms)) {
        draw_recording(rec_ms);
        return;
    }
    if (view->toast == TOAST_ALERT) {
        draw_alert(&view->toast_msg);
        return;
    }

    display_clear();
    header(view);
    switch (view->screen) {
    case SCREEN_HOME:
        draw_home();
        break;
    case SCREEN_MESSAGES:
        draw_messages(view->scroll);
        break;
    case SCREEN_NETWORK:
        draw_network(view->scroll);
        break;
    case SCREEN_COUNT:
        break;
    }

    if (view->toast == TOAST_SENT || view->toast == TOAST_INCOMING) {
        char what[COLS + 1], from[PRESENCE_NAME_MAX + 1];
        describe(&view->toast_msg, what, sizeof(what));
        if (view->toast == TOAST_SENT) {
            draw_box("ENVIADO", what);
        } else {
            presence_name_of(view->toast_msg.origin, from, sizeof(from));
            char title[COLS * 2 + 1];
            snprintf(title, sizeof(title), "de %s", from);
            draw_box(title, view->toast_msg.kind == MESSAGE_VOICE ? "VOZ: oir en el celu" : what);
        }
    }
}

int screens_rows(screen_t screen)
{
    switch (screen) {
    case SCREEN_MESSAGES:
        return messages_list(s_msgs, MAX_ROWS, 0);
    case SCREEN_NETWORK:
        return 1 + presence_nodes(s_nodes, MAX_ROWS);
    default:
        return 1;
    }
}
