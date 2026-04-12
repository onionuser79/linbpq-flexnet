/*
 * flexnet_l3.c — FlexNet L3 connection protocol (shared module)
 *
 * Portable parser, builder, and connection state for FlexNet L3
 * connections (NET/ROM L3/L4 format, PID=0xCF).
 *
 * No dependencies on LinBPQ or flexnetd — pure C.
 * Integration with the host application is done via the app_context
 * pointer in FLEXL3_CONNECTION.
 *
 * Author: IW2OHX, April 2026
 * License: GPL v2
 */

#include <string.h>
#include <time.h>
#include "flexnet_l3.h"

/* ── Connection Table ───────────────────────────────────────────────── */

static struct FLEXL3_CONNECTION connections[FLEXL3_MAX_CONNECTIONS];
static int next_circuit_index = 1;
static int next_circuit_id = 1;

void flexl3_init(void)
{
    memset(connections, 0, sizeof(connections));
    next_circuit_index = 1;
    next_circuit_id = 1;
}

/* ── Frame Parser ───────────────────────────────────────────────────── */

int flexl3_is_connection_frame(unsigned char * data, int len)
{
    /*
     * L3RTT frames start with ASCII text "L3RTT:" (0x4C 0x33 ...).
     * L3 connection frames start with AX.25 callsign bytes (shifted,
     * so first byte is always >= 0x60 for valid calls like 'A'-'Z').
     * Quick heuristic: if first byte >= 0x60, it's a connection frame.
     */
    if (len < FLEXL3_MIN_LEN) return 0;
    if (data[0] < 0x60) return 0;  /* not an AX.25 callsign byte */
    return 1;
}

int flexl3_parse(unsigned char * data, int len, struct FLEXL3_HEADER * hdr)
{
    if (len < FLEXL3_MIN_LEN) return -1;

    memset(hdr, 0, sizeof(*hdr));

    /* NET/ROM L3: offset 0 = source, offset 7 = destination */
    memcpy(hdr->source, &data[0], 7);
    memcpy(hdr->dest, &data[7], 7);

    /* TTL */
    hdr->ttl = data[14];

    /* Circuit index and ID */
    hdr->circuit_index = data[15];
    hdr->circuit_id = data[16];

    /* Sequence numbers */
    hdr->tx_seq = data[17];
    hdr->rx_seq = data[18];

    /* Opcode — lower bits contain the frame type */
    hdr->opcode = data[19] & 0x0F;

    /* Validate opcode */
    if (hdr->opcode < FLEXL3_CREQ || hdr->opcode > FLEXL3_IACK)
        return -1;

    /* Parse type-specific fields */
    switch (hdr->opcode)
    {
    case FLEXL3_CREQ:
        if (len < FLEXL3_CREQ_LEN) return -1;
        hdr->window = data[20];
        memcpy(hdr->origin_user, &data[21], 7);
        memcpy(hdr->origin_node, &data[28], 7);
        break;

    case FLEXL3_CACK:
        if (len >= FLEXL3_CACK_LEN)
            hdr->window = data[20];
        break;

    case FLEXL3_INFO:
        if (len > FLEXL3_HEADER_LEN)
        {
            hdr->payload = &data[FLEXL3_HEADER_LEN];
            hdr->payload_len = len - FLEXL3_HEADER_LEN;
        }
        break;

    case FLEXL3_IACK:
    case FLEXL3_DREQ:
    case FLEXL3_DACK:
        /* No extra fields */
        break;
    }

    return 0;
}

/* ── Frame Builders ─────────────────────────────────────────────────── */

static void build_header(unsigned char * buf,
    unsigned char * dest, unsigned char * source,
    int ttl, int index, int id,
    int tx_seq, int rx_seq, int opcode)
{
    /* NET/ROM L3: offset 0 = source, offset 7 = destination */
    memcpy(&buf[0], source, 7);
    memcpy(&buf[7], dest, 7);
    buf[14] = (unsigned char)ttl;
    buf[15] = (unsigned char)index;
    buf[16] = (unsigned char)id;
    buf[17] = (unsigned char)tx_seq;
    buf[18] = (unsigned char)rx_seq;
    buf[19] = (unsigned char)(opcode & 0x0F);
}

int flexl3_build_creq(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id, int window,
    unsigned char * origin_user, unsigned char * origin_node)
{
    if (buflen < FLEXL3_CREQ_LEN) return -1;

    build_header(buf, dest, source, ttl, index, id, 0, 0, FLEXL3_CREQ);
    buf[20] = (unsigned char)window;
    memcpy(&buf[21], origin_user, 7);
    memcpy(&buf[28], origin_node, 7);

    return FLEXL3_CREQ_LEN;
}

int flexl3_build_cack(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id, int window)
{
    if (buflen < FLEXL3_CACK_LEN) return -1;

    build_header(buf, dest, source, ttl, index, id, 0, 0, FLEXL3_CACK);
    buf[20] = (unsigned char)window;

    return FLEXL3_CACK_LEN;
}

int flexl3_build_info(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id, int tx_seq, int rx_seq,
    unsigned char * payload, int payload_len)
{
    int total = FLEXL3_HEADER_LEN + payload_len;
    if (buflen < total || payload_len > FLEXL3_MAX_DATA) return -1;

    build_header(buf, dest, source, ttl, index, id,
                 tx_seq, rx_seq, FLEXL3_INFO);
    if (payload_len > 0)
        memcpy(&buf[FLEXL3_HEADER_LEN], payload, payload_len);

    return total;
}

int flexl3_build_iack(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id, int tx_seq, int rx_seq)
{
    if (buflen < FLEXL3_HEADER_LEN) return -1;

    build_header(buf, dest, source, ttl, index, id,
                 tx_seq, rx_seq, FLEXL3_IACK);

    return FLEXL3_HEADER_LEN;
}

int flexl3_build_dreq(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id)
{
    if (buflen < FLEXL3_HEADER_LEN) return -1;

    build_header(buf, dest, source, ttl, index, id, 0, 0, FLEXL3_DREQ);

    return FLEXL3_HEADER_LEN;
}

int flexl3_build_dack(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id)
{
    if (buflen < FLEXL3_HEADER_LEN) return -1;

    build_header(buf, dest, source, ttl, index, id, 0, 0, FLEXL3_DACK);

    return FLEXL3_HEADER_LEN;
}

/* ── Connection Table Management ────────────────────────────────────── */

struct FLEXL3_CONNECTION * flexl3_find_by_remote(int index, int id)
{
    for (int i = 0; i < FLEXL3_MAX_CONNECTIONS; i++)
    {
        struct FLEXL3_CONNECTION * c = &connections[i];
        if (c->state != FLEXL3_STATE_IDLE &&
            c->remote_index == index && c->remote_id == id)
            return c;
    }
    return NULL;
}

struct FLEXL3_CONNECTION * flexl3_find_by_local(int index, int id)
{
    for (int i = 0; i < FLEXL3_MAX_CONNECTIONS; i++)
    {
        struct FLEXL3_CONNECTION * c = &connections[i];
        if (c->state != FLEXL3_STATE_IDLE &&
            c->local_index == index && c->local_id == id)
            return c;
    }
    return NULL;
}

struct FLEXL3_CONNECTION * flexl3_alloc(void)
{
    for (int i = 0; i < FLEXL3_MAX_CONNECTIONS; i++)
    {
        if (connections[i].state == FLEXL3_STATE_IDLE)
        {
            memset(&connections[i], 0, sizeof(connections[i]));
            connections[i].created = time(NULL);
            connections[i].last_activity = time(NULL);
            return &connections[i];
        }
    }
    return NULL;
}

void flexl3_free(struct FLEXL3_CONNECTION * conn)
{
    if (conn)
    {
        conn->state = FLEXL3_STATE_IDLE;
        conn->app_context = NULL;
    }
}

int flexl3_next_index(void)
{
    int idx = next_circuit_index;
    next_circuit_index = (next_circuit_index % 255) + 1;
    return idx;
}

int flexl3_next_id(void)
{
    int id = next_circuit_id;
    next_circuit_id = (next_circuit_id % 255) + 1;
    return id;
}

int flexl3_count_active(void)
{
    int count = 0;
    for (int i = 0; i < FLEXL3_MAX_CONNECTIONS; i++)
        if (connections[i].state != FLEXL3_STATE_IDLE)
            count++;
    return count;
}

/* ── Debug Helpers ──────────────────────────────────────────────────── */

struct FLEXL3_CONNECTION * flexl3_get_slot(int i)
{
    if (i < 0 || i >= FLEXL3_MAX_CONNECTIONS) return NULL;
    return &connections[i];
}

const char * flexl3_opcode_name(int opcode)
{
    switch (opcode)
    {
    case FLEXL3_CREQ: return "CREQ";
    case FLEXL3_CACK: return "CACK";
    case FLEXL3_DREQ: return "DREQ";
    case FLEXL3_DACK: return "DACK";
    case FLEXL3_INFO: return "INFO";
    case FLEXL3_IACK: return "IACK";
    default:          return "???";
    }
}
