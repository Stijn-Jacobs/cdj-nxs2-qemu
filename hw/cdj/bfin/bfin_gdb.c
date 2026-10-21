/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GDB remote serial protocol session for the Blackfin core. */
#include "bfin_priv.h"
#include "bfin_gdb.h"
#include <stdio.h>
#include <stdlib.h>

#define PKT_MAX   0x4000
#define NUM_REGS  54                    /* gdb's raw registers, R0 to PC */
#define PC_REGNUM 53

struct bfin_gdb {
    bfin_core   *core;
    bfin_gdb_ops ops;
    int          noack;
    int          detached;
    int          intr;
    bfin_gdb_run run;
    /* Receive: 0 between packets, 1 in the body, 2 and 3 in the checksum. */
    int          rx;
    size_t       rxlen;
    unsigned     rxsum;
    unsigned     cksum_hi;
    char         rxbuf[PKT_MAX + 1];
    char         last[PKT_MAX + 8];     /* last reply, resent on '-' */
    size_t       lastlen;
};

/* gdb's register number to the instruction set's group/number. */
static int reg_map(unsigned n, unsigned *grp, unsigned *reg)
{
    static const struct { uint8_t first, end, grp, base; } map[] = {
        {  0,  8, 0, 0 }, {  8, 16, 1, 0 }, { 16, 20, 2, 0 }, { 20, 24, 2, 4 },
        { 24, 28, 3, 0 }, { 28, 32, 3, 4 }, { 32, 36, 4, 0 }, { 36, 38, 4, 6 },
        { 38, 46, 6, 0 }, { 46, 53, 7, 0 },
    };

    for (unsigned k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
        if (n >= map[k].first && n < map[k].end) {
            *grp = map[k].grp;
            *reg = map[k].base + n - map[k].first;
            return 0;
        }
    }
    return -1;
}

static uint32_t reg_get(bfin_gdb *g, unsigned n)
{
    unsigned grp, reg;

    if (n == PC_REGNUM) {
        return bfin_get_pc(g->core);
    }
    reg_map(n, &grp, &reg);
    return bfin_get_reg(g->core, grp, reg);
}

static int reg_set(bfin_gdb *g, unsigned n, uint32_t v)
{
    unsigned grp, reg;

    if (n == PC_REGNUM) {
        bfin_set_pc(g->core, v);
        return 0;
    }
    return reg_map(n, &grp, &reg) ? -1 : bfin_set_reg(g->core, grp, reg, v);
}

/* ---- transport ---------------------------------------------------------- */

static void out(bfin_gdb *g, const void *buf, size_t len)
{
    g->ops.write(g->ops.opaque, buf, len);
}

static void send_packet(bfin_gdb *g, const char *data)
{
    size_t n = strlen(data), at = 0;
    unsigned sum = 0;

    if (n > PKT_MAX) {
        n = PKT_MAX;
    }
    g->last[at++] = '$';
    memcpy(g->last + at, data, n);
    at += n;
    for (size_t i = 0; i < n; i++) {
        sum += (uint8_t)data[i];
    }
    at += snprintf(g->last + at, 4, "#%02x", sum & 0xFF);
    g->lastlen = at;
    out(g, g->last, at);
}

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/* Parses hex at *p into *v; advances *p. Returns 0 on no digits. */
static int parse_hex(const char **p, uint32_t *v)
{
    int d, any = 0;

    *v = 0;
    while ((d = hexval(**p)) >= 0) {
        *v = *v << 4 | d;
        (*p)++;
        any = 1;
    }
    return any;
}

static int expect(const char **p, char ch)
{
    if (**p != ch) {
        return 0;
    }
    (*p)++;
    return 1;
}

/* A register or memory word as 8 hex digits, least significant byte first. */
static void put_le32(char *dst, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        sprintf(dst + 2 * i, "%02x", (v >> (8 * i)) & 0xFF);
    }
}

static int get_le32(const char *src, uint32_t *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        int hi = hexval(src[2 * i]), lo = hexval(src[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        *v |= (uint32_t)(hi << 4 | lo) << (8 * i);
    }
    return 0;
}

/* ---- memory ------------------------------------------------------------- */

static int mem_read(bfin_gdb *g, uint32_t addr, uint8_t *buf, size_t len)
{
    if (g->ops.read_mem) {
        return g->ops.read_mem(g->ops.opaque, addr, buf, len);
    }
    for (size_t n = 0; n < len; n++) {
        const bfin_ram *r = bfin_bank(g->core, addr + n, 1);

        if (!r) {
            return -1;
        }
        buf[n] = r->host[addr + n - r->base];
    }
    return 0;
}

static int mem_write(bfin_gdb *g, uint32_t addr, const uint8_t *buf,
                     size_t len)
{
    if (g->ops.write_mem) {
        return g->ops.write_mem(g->ops.opaque, addr, buf, len);
    }
    for (size_t n = 0; n < len; n++) {
        const bfin_ram *r = bfin_bank(g->core, addr + n, 1);

        if (!r || r->rom) {
            return -1;
        }
        r->host[addr + n - r->base] = buf[n];
    }
    g->core->code_gen++;
    return 0;
}

/* ---- packets ------------------------------------------------------------ */

static void cmd_regs(bfin_gdb *g)
{
    static char buf[NUM_REGS * 8 + 1];

    for (unsigned n = 0; n < NUM_REGS; n++) {
        put_le32(buf + 8 * n, reg_get(g, n));
    }
    send_packet(g, buf);
}

static void cmd_set_regs(bfin_gdb *g, const char *p)
{
    uint32_t v[NUM_REGS];

    if (strlen(p) < NUM_REGS * 8) {
        send_packet(g, "E01");
        return;
    }
    for (unsigned n = 0; n < NUM_REGS; n++) {
        if (get_le32(p + 8 * n, &v[n])) {
            send_packet(g, "E01");
            return;
        }
    }
    for (unsigned n = 0; n < NUM_REGS; n++) {
        if (reg_set(g, n, v[n])) {
            send_packet(g, "E01");
            return;
        }
    }
    send_packet(g, "OK");
}

static void cmd_reg(bfin_gdb *g, const char *p, int write)
{
    uint32_t n, v;
    char buf[9];

    if (!parse_hex(&p, &n) || n >= NUM_REGS) {
        send_packet(g, "E01");
        return;
    }
    if (!write) {
        put_le32(buf, reg_get(g, n));
        send_packet(g, buf);
        return;
    }
    if (!expect(&p, '=') || strlen(p) != 8 || get_le32(p, &v) ||
        reg_set(g, n, v)) {
        send_packet(g, "E01");
        return;
    }
    send_packet(g, "OK");
}

static void cmd_mem_read(bfin_gdb *g, const char *p)
{
    static char reply[PKT_MAX + 1];
    uint8_t chunk[PKT_MAX / 2];
    uint32_t addr, len;

    if (!parse_hex(&p, &addr) || !expect(&p, ',') || !parse_hex(&p, &len)) {
        send_packet(g, "E01");
        return;
    }
    if (len > sizeof(chunk)) {
        len = sizeof(chunk);
    }
    if (mem_read(g, addr, chunk, len)) {
        send_packet(g, "E14");
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        sprintf(reply + 2 * i, "%02x", chunk[i]);
    }
    reply[2 * len] = 0;
    send_packet(g, reply);
}

/* M addr,len:hex and X addr,len:binary (} escapes the next byte, xor 0x20). */
static void cmd_mem_write(bfin_gdb *g, const char *p, size_t plen, int binary)
{
    uint8_t data[PKT_MAX];
    uint32_t addr, len, n = 0;
    const char *end = p + plen;

    if (!parse_hex(&p, &addr) || !expect(&p, ',') || !parse_hex(&p, &len) ||
        !expect(&p, ':') || len > sizeof(data)) {
        send_packet(g, "E01");
        return;
    }
    while (n < len && p < end) {
        if (binary) {
            uint8_t b = *p++;

            if (b == '}' && p < end) {
                b = *p++ ^ 0x20;
            }
            data[n++] = b;
        } else {
            int hi = hexval(p[0]), lo = p + 1 < end ? hexval(p[1]) : -1;

            if (hi < 0 || lo < 0) {
                break;
            }
            data[n++] = hi << 4 | lo;
            p += 2;
        }
    }
    if (n != len) {
        send_packet(g, "E01");
    } else if (len && mem_write(g, addr, data, len)) {
        send_packet(g, "E14");
    } else {
        send_packet(g, "OK");
    }
}

/* Z/z type,addr,kind. Types 0 (software breakpoint), 2, 3, 4 (write, read,
 * access watchpoints). Anything else gets the empty "unsupported" reply. */
static void cmd_point(bfin_gdb *g, const char *p, int insert)
{
    uint32_t type, addr, len;
    int rc;

    if (!parse_hex(&p, &type) || !expect(&p, ',') || !parse_hex(&p, &addr) ||
        !expect(&p, ',') || !parse_hex(&p, &len)) {
        send_packet(g, "E01");
        return;
    }
    switch (type) {
    case 0:
        rc = insert ? bfin_break_add(g->core, addr) :
                      bfin_break_del(g->core, addr);
        break;
    case 2: case 3: case 4: {
        int kind = type == 2 ? BFIN_WATCH_WRITE :
                   type == 3 ? BFIN_WATCH_READ : BFIN_WATCH_ACCESS;

        rc = insert ? bfin_watch_add(g->core, addr, len, kind) :
                      bfin_watch_del(g->core, addr, len, kind);
        break;
    }
    default:
        send_packet(g, "");
        return;
    }
    send_packet(g, rc ? "E01" : "OK");
}

static void resume(bfin_gdb *g, bfin_gdb_run mode, const char *addr)
{
    uint32_t pc;

    if (*addr && parse_hex(&addr, &pc)) {
        bfin_set_pc(g->core, pc);
    }
    g->intr = 0;
    g->run = mode;
}

static void detach(bfin_gdb *g, int reply)
{
    bfin_break_clear(g->core);
    bfin_watch_clear(g->core);
    g->detached = 1;
    g->run = BFIN_GDB_CONT;
    if (reply) {
        send_packet(g, "OK");
    }
}

/* vCont;action[:thread][;action...]: only the first action matters with one
 * thread. */
static void cmd_vcont(bfin_gdb *g, const char *p)
{
    switch (*p) {
    case 'c': case 'C':
        resume(g, BFIN_GDB_CONT, "");
        break;
    case 's': case 'S':
        resume(g, BFIN_GDB_STEP, "");
        break;
    default:
        send_packet(g, "E01");
    }
}

static void handle(bfin_gdb *g, const char *p, size_t len)
{
    char reply[256];

    switch (*p) {
    case '?':
        send_packet(g, "T05");
        return;
    case 'g': cmd_regs(g); return;
    case 'G': cmd_set_regs(g, p + 1); return;
    case 'p': cmd_reg(g, p + 1, 0); return;
    case 'P': cmd_reg(g, p + 1, 1); return;
    case 'm': cmd_mem_read(g, p + 1); return;
    case 'M': cmd_mem_write(g, p + 1, len - 1, 0); return;
    case 'X': cmd_mem_write(g, p + 1, len - 1, 1); return;
    case 'Z': cmd_point(g, p + 1, 1); return;
    case 'z': cmd_point(g, p + 1, 0); return;
    case 'c': resume(g, BFIN_GDB_CONT, p + 1); return;
    case 's': resume(g, BFIN_GDB_STEP, p + 1); return;
    case 'D': detach(g, 1); return;
    case 'k': detach(g, 0); return;
    case 'H': case 'T':
        send_packet(g, "OK");
        return;
    case 'v':
        if (!strcmp(p, "vCont?")) {
            send_packet(g, "vCont;c;C;s;S");
        } else if (!strncmp(p, "vCont;", 6)) {
            cmd_vcont(g, p + 6);
        } else {
            send_packet(g, "");
        }
        return;
    case 'q':
        if (!strncmp(p, "qSupported", 10)) {
            snprintf(reply, sizeof(reply),
                     "PacketSize=%x;swbreak+;hwbreak+;QStartNoAckMode+",
                     PKT_MAX);
            send_packet(g, reply);
        } else if (!strcmp(p, "qAttached")) {
            send_packet(g, "1");
        } else if (!strcmp(p, "qC")) {
            send_packet(g, "QC1");
        } else if (!strcmp(p, "qfThreadInfo")) {
            send_packet(g, "m1");
        } else if (!strcmp(p, "qsThreadInfo")) {
            send_packet(g, "l");
        } else {
            send_packet(g, "");
        }
        return;
    case 'Q':
        if (!strcmp(p, "QStartNoAckMode")) {
            send_packet(g, "OK");
            g->noack = 1;
            return;
        }
        break;
    }
    send_packet(g, "");
}

/* ---- public ------------------------------------------------------------- */

bfin_gdb *bfin_gdb_new(bfin_core *core, const bfin_gdb_ops *ops)
{
    bfin_gdb *g = calloc(1, sizeof(*g));

    g->core = core;
    g->ops = *ops;
    return g;
}

void bfin_gdb_free(bfin_gdb *g)
{
    free(g);
}

void bfin_gdb_feed(bfin_gdb *g, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = buf[i];

        switch (g->rx) {
        case 0:
            if (ch == 0x03) {
                g->intr = 1;
            } else if (ch == '-' && g->lastlen) {
                out(g, g->last, g->lastlen);
            } else if (ch == '$' && !g->run) {
                g->rx = 1;
                g->rxlen = 0;
                g->rxsum = 0;
            }
            break;
        case 1:
            if (ch == '#') {
                g->rx = 2;
            } else if (g->rxlen < PKT_MAX) {
                g->rxbuf[g->rxlen++] = ch;
                g->rxsum += ch;
            }
            break;
        case 2:
            g->cksum_hi = hexval(ch);
            g->rx = 3;
            break;
        case 3: {
            int lo = hexval(ch);

            g->rx = 0;
            if ((int)g->cksum_hi < 0 || lo < 0 ||
                ((g->cksum_hi << 4 | lo) != (g->rxsum & 0xFF))) {
                if (!g->noack) {
                    out(g, "-", 1);
                }
                break;
            }
            if (!g->noack) {
                out(g, "+", 1);
            }
            g->rxbuf[g->rxlen] = 0;
            handle(g, g->rxbuf, g->rxlen);
            break;
        }
        }
    }
}

bfin_gdb_run bfin_gdb_run_request(const bfin_gdb *g)
{
    return g->run;
}

int bfin_gdb_interrupted(const bfin_gdb *g)
{
    return g->intr;
}

int bfin_gdb_detached(const bfin_gdb *g)
{
    return g->detached;
}

void bfin_gdb_stopped(bfin_gdb *g, bfin_stop reason)
{
    char reply[64];
    uint32_t addr;
    int kind;

    g->run = BFIN_GDB_HALT;
    g->intr = 0;
    switch (reason) {
    case BFIN_STOP_BREAK:
        send_packet(g, "T05swbreak:;");
        break;
    case BFIN_STOP_WATCH:
        if (bfin_watch_hit(g->core, &addr, &kind)) {
            snprintf(reply, sizeof(reply), "T05%s:%x;",
                     kind == BFIN_WATCH_WRITE ? "watch" :
                     kind == BFIN_WATCH_READ ? "rwatch" : "awatch", addr);
            send_packet(g, reply);
        } else {
            send_packet(g, "T05");
        }
        break;
    case BFIN_STOP_UNDEF:
        send_packet(g, "T04");
        break;
    case BFIN_STOP_BUDGET:
        send_packet(g, "T02");
        break;
    default:
        send_packet(g, "T05");
    }
}
