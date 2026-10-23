#include <string.h>

#include "dedup.h"

// Tiene que cubrir de sobra una rafaga completa (un mensaje de voz son ~400
// paquetes): si una entrada se pisa mientras su paquete todavia rebota por la
// red, se reenviaria de nuevo.
#define DEDUP_SIZE 1024

static struct {
    mesh_node_id_t origin;
    uint16_t seq;
} s_seen[DEDUP_SIZE];
static int s_next;
static int s_count;

void dedup_reset(void)
{
    memset(s_seen, 0, sizeof(s_seen));
    s_next = 0;
    s_count = 0;
}

bool dedup_check_and_add(mesh_node_id_t origin, uint16_t seq)
{
    // Busca de lo mas nuevo a lo mas viejo: los duplicados llegan enseguida.
    for (int i = 0; i < s_count; i++) {
        const int idx = (s_next - 1 - i + DEDUP_SIZE) % DEDUP_SIZE;
        if (s_seen[idx].origin == origin && s_seen[idx].seq == seq) {
            return false;
        }
    }
    s_seen[s_next].origin = origin;
    s_seen[s_next].seq = seq;
    s_next = (s_next + 1) % DEDUP_SIZE;
    if (s_count < DEDUP_SIZE) {
        s_count++;
    }
    return true;
}
