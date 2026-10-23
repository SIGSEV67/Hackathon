#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "buttons.h"
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
static presence_known_t s_known[PRESENCE_MAX_KNOWN];
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
    // Privado propio: "a Nombre: ..." para saber a quien fue.
    if (m->dest != MESSAGES_TO_ALL && m->hops == MESSAGE_HOPS_SELF) {
        char to[PRESENCE_NAME_MAX + 1];
        presence_name_of(m->dest, to, sizeof(to));
        const int n = snprintf(out, len, "a %s%s: ", to, m->delivered ? "(ok)" : "");
        if (n < 0 || (size_t)n >= len) {
            return;
        }
        out += n;
        len -= n;
    }
    switch (m->kind) {
    case MESSAGE_ALERT:
        snprintf(out, len, "%s", messages_alert_label(m->alert));
        break;
    case MESSAGE_TEXT:
        snprintf(out, len, "%s", m->text);
        break;
    case MESSAGE_VOICE:
        snprintf(out, len, "VOZ %us", m->blocks * MESSAGES_VOICE_BLOCK_SAMPLES / MESSAGES_VOICE_SAMPLE_RATE);
        break;
    case MESSAGE_FILE:
        snprintf(out, len, "ARCH %s", m->text[0] ? m->text : "...");
        break;
    }
}

// "MSJ 4:oir" si el cursor esta sobre una voz completa y hay parlante.
static const char *messages_title(const ui_view_t *view)
{
    message_t m;
    if (!audio_has_speaker() || !view->selected_msg || !messages_get(view->selected_msg, &m) ||
        m.kind != MESSAGE_VOICE || m.blocks_received < m.blocks) {
        return s_titles[SCREEN_MESSAGES];
    }
    uint32_t playing;
    return messages_voice_playing(&playing, NULL) && playing == m.id ? "MSJ 4:parar" : "MSJ 4:oir";
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
    char net_title[16];
    snprintf(net_title, sizeof(net_title), "RED %08" PRIX32, mesh_self_id());  // el id, para dictarlo
    const char *title = view->screen == SCREEN_HOME      ? name
                        : view->screen == SCREEN_NETWORK ? net_title
                        : view->screen == SCREEN_MESSAGES ? messages_title(view)
                                                         : s_titles[view->screen];
    char buf[COLS + 1];
    to_ascii(title, buf, sizeof(buf));
    buf[(right_x - 2) / DISPLAY_FONT_W] = '\0';
    display_text(1, 1, buf, true);
    display_text(right_x, 1, right, true);
}

// --- pantallas ------------------------------------------------------------------

// Dibujo de las teclas como estan en la placa: KEY1-3 en fila abajo y KEY4
// apartada arriba a la derecha. Se rellenan mientras estan apretadas.
#define KEY_W   26
#define KEY_H   11
#define KEY_GAP 3
#define KEY4_W  36

static void draw_key(int x, int y, int w, const char *label, bool pressed)
{
    display_fill_rect(x, y, w, KEY_H, pressed);
    display_draw_rect(x, y, w, KEY_H);
    const int tx = x + (w - (int)strlen(label) * DISPLAY_FONT_W) / 2 + 1;
    display_text(tx, y + 2, label, pressed);
}

static void draw_keys(void)
{
    const uint32_t pressed = buttons_read();
    const int row_y = H - KEY_H;
    for (int i = 0; i < 3; i++) {
        draw_key(i * (KEY_W + KEY_GAP), row_y, KEY_W, g_key_labels[i], pressed & (1u << (BTN_KEY1 + i)));
    }
    draw_key(W - KEY4_W, row_y - KEY_H - 2, KEY4_W, g_key_labels[3], pressed & (1u << BTN_KEY4));
    text(0, row_y - KEY_H, "mantener:", false);
}

static void draw_home(void)
{
    presence_node_t self;
    presence_self(&self);
    const size_t n = presence_nodes(s_nodes, MAX_ROWS);

    char line[COLS * 2 + 1];  // text() recorta al ancho de la pantalla
    snprintf(line, sizeof(line), "Vecinos %u  Red %u", self.n_links, (unsigned)n + 1);
    text(0, 11, line, false);

    text(0, 21, "Ruido", false);
    display_draw_rect(34, 20, W - 34, 9);
    display_fill_rect(36, 22, self.noise * (W - 38) / 100, 5, true);

    // Ultimo mensaje en una linea: "SOS 12s Puesto med"
    message_t last;
    if (messages_list(&last, 1, 0) == 1) {
        char what[COLS + 1], from[PRESENCE_NAME_MAX + 1], when[8];
        describe(&last, what, sizeof(what));
        presence_name_of(last.origin, from, sizeof(from));
        ago(mesh_now_ms() - last.received_ms, when, sizeof(when));
        char summary[COLS + PRESENCE_NAME_MAX + 16];
        snprintf(summary, sizeof(summary), "%s %s %s", what, when, last.hops == MESSAGE_HOPS_SELF ? "(vos)" : from);
        text(0, 31, summary, last.kind == MESSAGE_ALERT && last.alert != ALERT_SAFE);
    } else {
        text(0, 31, "Sin mensajes", false);
    }

    draw_keys();
}

// El cursor es un recuadro (la inversion ya marca las alertas). Devuelve el id
// del mensaje bajo el cursor.
static uint32_t draw_messages(int cursor)
{
    const size_t total = messages_list(s_msgs, MAX_ROWS, 0);
    if (!total) {
        text(0, LIST_Y + 10, "Sin mensajes todavia", false);
        return 0;
    }
    uint32_t playing;
    messages_voice_playing(&playing, NULL);
    const uint32_t now = mesh_now_ms();
    const int first = cursor >= LIST_ROWS ? cursor - LIST_ROWS + 1 : 0;
    for (size_t i = first, row = 0; i < total && row < LIST_ROWS; i++, row++) {
        const message_t *m = &s_msgs[i];
        char what[COLS + 1], when[8], line[COLS * 2 + 1];
        describe(m, what, sizeof(what));
        ago(now - m->received_ms, when, sizeof(when));
        const char mark = m->kind == MESSAGE_ALERT          ? '!'
                          : m->dest != MESSAGES_TO_ALL       ? '@'  // privado
                          : m->kind == MESSAGE_VOICE         ? (m->id == playing ? '*' : '>')
                          : m->kind == MESSAGE_FILE          ? '#'
                                                             : '"';
        snprintf(line, sizeof(line), "%c%-3s %s", mark, when, what);
        text(1, LIST_Y + row * ROW_H, line, m->kind == MESSAGE_ALERT && m->alert != ALERT_SAFE);
        if ((int)i == cursor) {
            display_draw_rect(0, LIST_Y + row * ROW_H - 1, W, ROW_H + 1);
        }
    }
    return (size_t)cursor < total ? s_msgs[cursor].id : 0;
}

// Historial de nodos con los vivos primero. Devuelve cuantos.
static size_t known_sorted(void)
{
    const size_t n = presence_known(s_known, PRESENCE_MAX_KNOWN);
    size_t alive = 0;
    for (size_t i = 0; i < n; i++) {
        if (s_known[i].alive) {
            const presence_known_t k = s_known[i];
            memmove(&s_known[alive + 1], &s_known[alive], (i - alive) * sizeof(k));
            s_known[alive++] = k;
        }
    }
    return n;
}

static const presence_node_t *alive_node(mesh_node_id_t id, size_t n_alive)
{
    for (size_t i = 0; i < n_alive; i++) {
        if (s_nodes[i].id == id) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

// Fila 0 = Todos, despues el historial. El cursor (scroll) marca el destino de
// la voz de KEY4; la ventana se corre para que el cursor siempre se vea.
static mesh_node_id_t draw_network(int cursor)
{
    const size_t n_known = known_sorted();
    const size_t n_alive = presence_nodes(s_nodes, MAX_ROWS);
    const size_t total = 1 + n_known;
    const int first = cursor >= LIST_ROWS ? cursor - LIST_ROWS + 1 : 0;

    for (size_t i = first, row = 0; i < total && row < LIST_ROWS; i++, row++) {
        char line[COLS * 2 + 1], name[11];
        if (i == 0) {
            snprintf(line, sizeof(line), "Todos     (4: voz)");
        } else {
            const presence_known_t *k = &s_known[i - 1];
            to_ascii(k->name, name, sizeof(name));
            const presence_node_t *node = k->alive ? alive_node(k->id, n_alive) : NULL;
            if (node) {
                snprintf(line, sizeof(line), "%-10s r%-3u %us", name, node->noise, node->hops + 1);
            } else if (k->seen) {
                char when[8];
                ago(k->age_ms, when, sizeof(when));
                snprintf(line, sizeof(line), "%-10s -- %s", name, when);
            } else {
                snprintf(line, sizeof(line), "%-10s --", name);
            }
        }
        text(0, LIST_Y + row * ROW_H, line, (int)i == cursor);
    }
    return cursor > 0 && (size_t)cursor <= n_known ? s_known[cursor - 1].id : MESSAGES_TO_ALL;
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

ui_pick_t screens_draw(const ui_view_t *view)
{
    const ui_pick_t keep = {.node = view->selected, .message = view->selected_msg};
    uint32_t rec_ms;
    if (messages_voice_recording(&rec_ms)) {
        draw_recording(rec_ms);
        return keep;
    }
    if (view->toast == TOAST_ALERT) {
        draw_alert(&view->toast_msg);
        return keep;
    }
    ui_pick_t pick = {0};

    display_clear();
    header(view);
    switch (view->screen) {
    case SCREEN_HOME:
        draw_home();
        break;
    case SCREEN_MESSAGES:
        pick.message = draw_messages(view->scroll);
        break;
    case SCREEN_NETWORK:
        pick.node = draw_network(view->scroll);
        break;
    case SCREEN_COUNT:
        break;
    }

    const message_t *tm = &view->toast_msg;
    char what[COLS * 2 + 1], who[PRESENCE_NAME_MAX + 1], title[COLS * 2 + 1];
    switch (view->toast) {
    case TOAST_SENT:
        describe(tm, what, sizeof(what));
        draw_box(tm->dest ? "ENVIADO (privado)" : "ENVIADO", what);
        break;
    case TOAST_INCOMING:
        describe(tm, what, sizeof(what));
        presence_name_of(tm->origin, who, sizeof(who));
        snprintf(title, sizeof(title), "%s %s", tm->dest ? "PRIVADO de" : "de", who);
        draw_box(title, tm->kind == MESSAGE_VOICE  ? (audio_has_speaker() ? "VOZ: 4 en Mensajes" : "VOZ: oir en el celu")
                        : tm->kind == MESSAGE_FILE ? "ARCHIVO: ver en el celu"
                                                   : what);
        break;
    case TOAST_DELIVERED:
        presence_name_of(tm->dest, who, sizeof(who));
        snprintf(title, sizeof(title), "a %s", who);
        draw_box("ENTREGADO", title);
        break;
    default:
        break;
    }
    return pick;
}

int screens_rows(screen_t screen)
{
    switch (screen) {
    case SCREEN_MESSAGES:
        return messages_list(s_msgs, MAX_ROWS, 0);
    case SCREEN_NETWORK:
        return 1 + presence_known(s_known, PRESENCE_MAX_KNOWN);
    default:
        return 1;
    }
}
