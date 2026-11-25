/* Monitor IP decode (Moncode.c DISPLAYIPDATAGRAM, v2.11.6): TTL/ID, TCP
 * flags/seq/ack/win/data, UDP, ICMP type/code/id/seq, payload preview, and
 * hard bounds on every read.
 *
 * Extracted verbatim from Moncode.c by tools/unit/extract.sh. Each datagram
 * is copied into a heap block of exactly `avail` bytes, so ASan catches any
 * read past what the frame carries.
 */
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char UCHAR;
typedef unsigned short USHORT;

#pragma pack(1)
struct myin_addr { uint32_t addr; };
typedef struct _IPMSG {
    UCHAR VERLEN; UCHAR TOS; USHORT IPLENGTH; USHORT IPID; USHORT FRAGWORD;
    UCHAR IPTTL; UCHAR IPPROTOCOL; USHORT IPCHECKSUM;
    struct myin_addr IPSOURCE; struct myin_addr IPDEST;
    UCHAR Data;
} IPMSG;
typedef struct _TCPMSG {
    USHORT SOURCEPORT; USHORT DESTPORT; uint32_t SEQNUM; uint32_t ACKNUM;
    UCHAR TCPCONTROL; UCHAR TCPFLAGS; USHORT WINDOW; USHORT CHECKSUM; USHORT URGPTR;
} TCPMSG, *PTCPMSG;
typedef struct _UDPMSG {
    USHORT SOURCEPORT; USHORT DESTPORT; USHORT LENGTH; USHORT CHECKSUM;
} UDPMSG, *PUDPMSG;
typedef struct _ICMPMSG {
    UCHAR ICMPTYPE; UCHAR ICMPCODE; USHORT ICMPCHECKSUM; USHORT ICMPID; USHORT ICMPSEQUENCE;
} ICMPMSG, *PICMPMSG;
#pragma pack()

#include "extracted_ip_decode.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what, const char * got)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n    got: %s\n", what, got); }
}

/* Build an IPv4 header: 44.134.24.10 > 44.134.24.18, IHL 5 unless ihl given. */
static size_t ip_hdr(UCHAR * b, int proto, int totlen, int ihl_words, int frag)
{
    memset(b, 0, 20);
    b[0] = (UCHAR)(0x40 | ihl_words);
    b[2] = (UCHAR)(totlen >> 8); b[3] = (UCHAR)totlen;
    b[4] = 0x12; b[5] = 0x34;                           /* ID 4660 */
    b[6] = (UCHAR)(frag >> 8); b[7] = (UCHAR)frag;
    b[8] = 64; b[9] = (UCHAR)proto;
    b[12] = 44; b[13] = 134; b[14] = 24; b[15] = 10;
    b[16] = 44; b[17] = 134; b[18] = 24; b[19] = 18;
    return 20;
}

/* Decode `avail` bytes of `pkt` from an exact-size heap copy. */
static const char * decode(const UCHAR * pkt, int avail)
{
    static char out[2048];
    UCHAR * copy = malloc(avail > 0 ? (size_t)avail : 1);
    UCHAR * end;
    if (!copy) { perror("malloc"); exit(2); }
    if (avail > 0)
        memcpy(copy, pkt, (size_t)avail);
    memset(out, 0, sizeof(out));
    end = DISPLAYIPDATAGRAM((IPMSG *)copy, (UCHAR *)out, avail);
    *end = 0;
    free(copy);
    return out;
}

static void test_tcp_syn(void)
{
    UCHAR p[60]; const char * r;
    size_t o = ip_hdr(p, 6, 40, 5, 0x4000);
    memset(p + o, 0, 20);
    p[o] = 0xA0; p[o + 1] = 0x92;                     /* src 41106 */
    p[o + 3] = 23;                                    /* dst 23 */
    p[o + 4] = 0; p[o + 5] = 0; p[o + 6] = 0x30; p[o + 7] = 0x39;   /* seq 12345 */
    p[o + 12] = 0x50; p[o + 13] = 0x02;               /* off 5, SYN */
    p[o + 14] = 0x04; p[o + 15] = 0x38;               /* win 1080 */
    r = decode(p, 40);
    ok(strncmp(r, "44.134.24.10>44.134.24.18 LEN:40 TTL:64 ID:4660 DF TCP Src 41106 Dest 23 ",
               strlen("44.134.24.10>44.134.24.18 LEN:40 TTL:64 ID:4660 DF TCP Src 41106 Dest 23 ")) == 0,
       "upstream tokens kept in order, TTL/ID added after LEN", r);
    ok(strstr(r, "Flags:SYN Seq:12345 Win:1080 Data:0") != NULL, "SYN, seq, win, no payload", r);
    ok(strstr(r, "Ack:") == NULL, "no Ack: without the ACK flag", r);
    ok(strstr(r, "Data: ") == NULL, "no preview line for an empty segment", r);
}

static void test_tcp_data_preview(void)
{
    UCHAR p[80]; const char * r;
    const char * text = "login: \r\n";
    size_t n = strlen(text);
    size_t o = ip_hdr(p, 6, (int)(40 + n), 5, 0);
    memset(p + o, 0, 20);
    p[o + 1] = 23; p[o + 2] = 0xA0; p[o + 3] = 0x92;
    p[o + 11] = 7;                                    /* ack 7 */
    p[o + 12] = 0x50; p[o + 13] = 0x18;               /* PSH ACK */
    memcpy(p + o + 20, text, n);
    r = decode(p, (int)(40 + n));
    ok(strstr(r, "Flags:ACK,PSH Seq:0 Ack:7 Win:0 Data:9") != NULL, "ACK,PSH with Ack and data size", r);
    ok(strstr(r, "\r  Data: login: ..") != NULL, "printable preview, CR/LF shown as dots", r);
}

static void test_framing_bytes_never_leak(void)
{
    UCHAR p[80]; const char * r; size_t i;
    size_t o = ip_hdr(p, 17, 20 + 8 + 4, 5, 0);
    memset(p + o, 0, 8);
    p[o + 1] = 53; p[o + 3] = 53;
    p[o + 8] = 0xFF; p[o + 9] = 0xFE; p[o + 10] = 0x1B; p[o + 11] = 'A';
    r = decode(p, 32);
    ok(strstr(r, "UDP Src 53 Dest 53 Data:4") != NULL, "UDP ports and size", r);
    for (i = 0; r[i]; i++)
        if ((UCHAR)r[i] >= 0x7f || ((UCHAR)r[i] < 0x20 && r[i] != '\r'))
            break;
    ok(r[i] == 0, "only printable ASCII + CR in output (0xFE/0xFF are telnet monitor framing)", r);
    ok(strstr(r, "Data: ...A") != NULL, "non-printables as dots", r);
}

static void test_icmp(void)
{
    UCHAR p[64]; const char * r;
    size_t o = ip_hdr(p, 1, 20 + 8 + 56, 5, 0);
    memset(p + o, 0, 8);
    p[o] = 8; p[o + 5] = 1; p[o + 7] = 5;             /* echo req id 1 seq 5 */
    r = decode(p, 28);
    ok(strstr(r, "ICMP Echo Request ID:1 Seq:5 Data:56") != NULL, "echo request id/seq/size", r);
    p[o] = 3; p[o + 1] = 1;
    r = decode(p, 28);
    ok(strstr(r, "ICMP Dest Unreachable Code:1") != NULL, "named type + code", r);
    p[o] = 42; p[o + 1] = 0;
    r = decode(p, 28);
    ok(strstr(r, "ICMP Type:42 Code:0") != NULL, "unknown type printed numerically", r);
}

static void test_bounds(void)
{
    UCHAR p[64]; const char * r;
    ip_hdr(p, 6, 40, 5, 0);
    r = decode(p, 10);
    ok(strstr(r, "too short: 10 bytes") != NULL, "under 20 bytes: no header read", r);
    r = decode(p, 0);
    ok(strstr(r, "too short") != NULL, "zero bytes", r);
    r = decode(p, 26);
    ok(strstr(r, "TCP Src 0 Dest 0 (truncated)") != NULL, "TCP header cut by the frame", r);
    ip_hdr(p, 6, 40, 3, 0);
    r = decode(p, 40);
    ok(strstr(r, "(bad header: ver 4 ihl 12)") != NULL, "IHL < 20 rejected", r);
    ip_hdr(p, 6, 40, 15, 0);
    r = decode(p, 40);
    ok(strstr(r, "(bad header: ver 4 ihl 60)") != NULL, "IHL past total length rejected", r);
}

static void test_fragment_and_other(void)
{
    UCHAR p[64]; const char * r;
    ip_hdr(p, 6, 40, 5, 0x2000 | 10);                 /* MF, offset 80 */
    r = decode(p, 40);
    ok(strstr(r, "MF Offset 80 Proto:6") != NULL, "later fragment: no transport decode", r);
    ip_hdr(p, 47, 40, 5, 0);
    r = decode(p, 40);
    ok(strstr(r, "Proto:47 Data:20") != NULL, "other protocol: number + size", r);
}

static void test_preview_cap(void)
{
    UCHAR p[200]; const char * r; const char * d;
    size_t o = ip_hdr(p, 17, 20 + 8 + 100, 5, 0);
    memset(p + o, 0, 8);
    memset(p + o + 8, 'x', 100);
    r = decode(p, 128);
    d = strstr(r, "Data: ");
    ok(d != NULL && strlen(d) == 6 + 64 + 3, "preview capped at 64 chars + '...'", r);
    r = decode(p, 40);                                /* frame carries only 12 of 100 */
    d = strstr(r, "Data: ");
    ok(strstr(r, "Data:100") != NULL && d != NULL && strlen(d) == 6 + 12 + 3,
       "datagram size from header, preview limited to bytes present", r);
}

static void test_worst_case_length(void)
{
    /* Longest header line + full preview: what the "<300 chars" bound in
     * Moncode.c promises the DecodeFrame callers. */
    UCHAR p[200]; const char * r;
    size_t o = ip_hdr(p, 6, 65535, 5, 0x6000);
    p[4] = p[5] = 0xff; p[8] = 255;
    p[12] = p[13] = p[14] = p[15] = 255; p[16] = p[17] = p[18] = p[19] = 255;
    memset(p + o, 0xff, 20);
    p[o + 12] = 0x50; p[o + 13] = 0x3f;
    memset(p + o + 20, 'y', 100);
    r = decode(p, 140);
    ok(strlen(r) < 300, "worst-case decode stays under 300 chars", r);
    ok(strstr(r, "Flags:SYN,ACK,PSH,FIN,RST,URG Seq:4294967295 Ack:4294967295 Win:65535") != NULL,
       "all flags, max seq/ack/win", r);
}

int main(void)
{
    test_worst_case_length();
    test_tcp_syn();
    test_tcp_data_preview();
    test_framing_bytes_never_leak();
    test_icmp();
    test_bounds();
    test_fragment_and_other();
    test_preview_cap();
    printf("ip_decode: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
