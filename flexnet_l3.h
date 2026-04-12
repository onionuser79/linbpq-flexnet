/*
 * flexnet_l3.h — FlexNet L3 connection protocol (shared module)
 *
 * Portable L3 frame parser, builder, and connection state machine.
 * No dependencies on LinBPQ or flexnetd — pure C with standard library.
 * Used by both linbpq-flexnet (FlexNetCode.c) and flexnetd (poll_cycle.c).
 *
 * Wire format: standard NET/ROM L3/L4 carried in PID=0xCF frames.
 * Verified from live XNET captures (2026-04-12).
 *
 * Author: IW2OHX, April 2026
 * License: GPL v2
 */

#ifndef FLEXNET_L3_H
#define FLEXNET_L3_H

#include <time.h>

/* ── L3 Frame Types (opcode values) ─────────────────────────────────── */

#define FLEXL3_CREQ   1     /* Connect Request */
#define FLEXL3_CACK   2     /* Connect Acknowledge */
#define FLEXL3_DREQ   3     /* Disconnect Request */
#define FLEXL3_DACK   4     /* Disconnect Acknowledge */
#define FLEXL3_INFO   5     /* Information (user data) */
#define FLEXL3_IACK   6     /* Information Acknowledge */

/* ── L3 Header Layout ───────────────────────────────────────────────── */
/*
 * Offset  Size  Field
 * 0       7     Source callsign (AX.25 shifted format)
 * 7       7     Destination callsign (AX.25 shifted format)
 * 14      1     TTL (hop count, decremented at each node)
 * 15      1     Circuit index (connection multiplexer)
 * 16      1     Circuit ID (connection identifier)
 * 17      1     TX sequence number (S(n) for I/IACK, 0 for control)
 * 18      1     RX sequence number (R(n) for I/IACK, 0 for control)
 * 19      1     Opcode (1-6, see FLEXL3_* constants above)
 *
 * CREQ extra (after opcode):
 * 20      1     Proposed window size
 * 21      7     Originating user callsign (AX.25)
 * 28      7     Originating node callsign (AX.25)
 * Total: 35 bytes
 *
 * CACK extra:
 * 20      1     Accepted window size
 * Total: 21 bytes
 *
 * INFO extra:
 * 20+     var   User data payload (1-235 bytes typical)
 *
 * IACK, DREQ, DACK: no extra data (20 bytes total)
 */

#define FLEXL3_HEADER_LEN     20    /* fixed header size */
#define FLEXL3_CREQ_LEN       35    /* CREQ total size */
#define FLEXL3_CACK_LEN       21    /* CACK total size */
#define FLEXL3_MIN_LEN        20    /* minimum valid frame */
#define FLEXL3_MAX_DATA       236   /* max payload in I-frame */
#define FLEXL3_MAX_FRAME      256   /* max total frame */

#define FLEXL3_DEFAULT_TTL     20   /* default hop count */
#define FLEXL3_DEFAULT_WINDOW   4   /* default window size */
#define FLEXL3_MAX_CONNECTIONS 16   /* max simultaneous L3 connections */

/* ── Parsed L3 Header ───────────────────────────────────────────────── */

struct FLEXL3_HEADER
{
    unsigned char dest[7];          /* AX.25 destination */
    unsigned char source[7];        /* AX.25 source */
    int  ttl;                       /* hop count */
    int  circuit_index;             /* IN — connection multiplexer */
    int  circuit_id;                /* ID — connection identifier */
    int  tx_seq;                    /* S(n) send sequence */
    int  rx_seq;                    /* R(n) receive sequence */
    int  opcode;                    /* frame type (1-6) */
    /* CREQ/CACK only */
    int  window;                    /* proposed/accepted window */
    unsigned char origin_user[7];   /* CREQ: originating user call */
    unsigned char origin_node[7];   /* CREQ: originating node call */
    /* I-frame payload */
    unsigned char * payload;        /* pointer into original buffer */
    int  payload_len;               /* payload length */
};

/* ── L3 Connection State ────────────────────────────────────────────── */

#define FLEXL3_STATE_IDLE           0
#define FLEXL3_STATE_CONNECTING     1   /* CREQ sent, waiting CACK */
#define FLEXL3_STATE_CONNECTED      2   /* data exchange active */
#define FLEXL3_STATE_DISCONNECTING  3   /* DREQ sent, waiting DACK */

struct FLEXL3_CONNECTION
{
    int  state;

    /* Local circuit (our side) */
    int  local_index;
    int  local_id;

    /* Remote circuit (peer's side, from CREQ/CACK) */
    int  remote_index;
    int  remote_id;

    /* Callsigns */
    unsigned char local_call[7];    /* our L3 identity (MYCALL) */
    unsigned char remote_call[7];   /* remote L3 node */
    unsigned char remote_user[7];   /* originating user (from CREQ) */
    unsigned char remote_node[7];   /* originating node (from CREQ) */

    /* Flow control */
    int  window;
    int  tx_seq;                    /* next S(n) to send */
    int  rx_seq;                    /* next S(n) we expect to receive */
    int  acked_seq;                 /* last R(n) we received from peer */

    /* Timing */
    time_t created;
    time_t last_activity;

    /* Application context — opaque pointer for BPQ session or fd */
    void * app_context;
};

/* ── Parser / Builder API ───────────────────────────────────────────── */

/*
 * Parse a raw CF payload into a FLEXL3_HEADER structure.
 * Returns 0 on success, -1 on invalid frame.
 * hdr->payload points into the original data buffer (not copied).
 */
int flexl3_parse(unsigned char * data, int len, struct FLEXL3_HEADER * hdr);

/*
 * Check if a CF payload is an L3 connection frame (not L3RTT).
 * Returns 1 if it's an L3 connection frame, 0 if L3RTT or other.
 * Quick check without full parse — use before flexl3_parse().
 */
int flexl3_is_connection_frame(unsigned char * data, int len);

/* Build L3 frames. Return frame length, or -1 on error. */

int flexl3_build_creq(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id, int window,
    unsigned char * origin_user, unsigned char * origin_node);

int flexl3_build_cack(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id, int window);

int flexl3_build_info(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id, int tx_seq, int rx_seq,
    unsigned char * payload, int payload_len);

int flexl3_build_iack(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id, int tx_seq, int rx_seq);

int flexl3_build_dreq(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id);

int flexl3_build_dack(unsigned char * buf, int buflen,
    unsigned char * dest, unsigned char * source, int ttl,
    int index, int id);

/* ── Connection Table API ───────────────────────────────────────────── */

void flexl3_init(void);

struct FLEXL3_CONNECTION * flexl3_find_by_remote(int index, int id);
struct FLEXL3_CONNECTION * flexl3_find_by_local(int index, int id);
struct FLEXL3_CONNECTION * flexl3_alloc(void);
void flexl3_free(struct FLEXL3_CONNECTION * conn);

int  flexl3_next_index(void);   /* allocate unique circuit index */
int  flexl3_next_id(void);      /* allocate unique circuit ID */
int  flexl3_count_active(void); /* count connections in use */

/* ── Opcode Name (for debug logging) ────────────────────────────────── */

struct FLEXL3_CONNECTION * flexl3_get_slot(int i);
const char * flexl3_opcode_name(int opcode);

#endif /* FLEXNET_L3_H */
