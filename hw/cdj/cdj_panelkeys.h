/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Front-panel key and touch side channel between the two board processes.
 * The GUI board owns the window (and so the keyboard and mouse); the front
 * panel is a device on MAIN. Presses cross over as UDP datagrams on
 * 127.0.0.1, bound by MAIN and sent by the GUI board or any script:
 *
 *      <off>:<val>:<dur_ms>[:<op>]    e.g. 0x14:0x01:120  = tap BROWSE
 *
 *      op omitted / "or"   report[off] |= val   while the press lasts
 *      "rot"               report[off] += val   permanently (rotary counter)
 *
 * CDJ_PANEL_KEYSOCK is a name, not a filesystem path: it is hashed to a port
 * by cdj_panelsock_port(). A name ending in ":<port>", or a bare number, sets
 * the port directly. UDP rather than AF_UNIX because Windows has no datagram
 * AF_UNIX.
 *
 * Known report bits: BROWSE 0x14/0x01, MENU 0x14/0x08, USB 0x13/0x04.
 * CDJ_PANEL_ROTARY=<off> and CDJ_PANEL_ROTPUSH=<off>:<mask> set the rotary
 * byte and push bit, which are not yet identified.
 */
#ifndef CDJ_PANELKEYS_H
#define CDJ_PANELKEYS_H

#include "qemu/sockets.h"

#define CDJ_PANELKEY_ENV   "CDJ_PANEL_KEYSOCK"

/* Name -> port. Must match port_for() in scripts/run/cdj_panelsock.py. */
static inline uint16_t cdj_panelsock_port(const char *name)
{
    const unsigned char *p;
    const char *base, *colon, *digits;
    unsigned long explicit_port;
    uint32_t h = 2166136261u;
    size_t len;
    char *end;

    /* Hash only the stem (no directory, no ".sock"): MSYS2 rewrites /tmp
     * arguments into Windows paths, and both ends must agree. */
    base = name;
    for (p = (const unsigned char *)name; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = (const char *)p + 1;
        }
    }
    len = strlen(base);
    if (len > 5 && !strcmp(base + len - 5, ".sock")) {
        len -= 5;
    }

    colon = strrchr(base, ':');
    digits = colon ? colon + 1 : base;
    explicit_port = strtoul(digits, &end, 10);
    if (*digits && !*end && explicit_port >= 1024 && explicit_port <= 65535) {
        return (uint16_t)explicit_port;
    }

    for (p = (const unsigned char *)base; len--; p++) {
        h = (h ^ *p) * 16777619u;
    }
    /* FNV-1a folded into 20000..44999: Windows reserves UDP ranges above
     * 50000 for Hyper-V and a bind there fails. */
    return (uint16_t)(20000 + h % 25000);
}

static inline void cdj_panelsock_addr(const char *name, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa->sin_port = htons(cdj_panelsock_port(name));
}

/* Receiver: bind the name, then recv() datagrams. */
static inline int cdj_panelsock_bind(const char *name)
{
    struct sockaddr_in sa;
    int fd;

    if (!name || !*name) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    cdj_panelsock_addr(name, &sa);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    qemu_socket_set_nonblock(fd);
    return fd;
}

/* Sender: a connected UDP socket. Sends fail harmlessly if nobody is bound. */
static inline int cdj_panelsock_connect(const char *name)
{
    struct sockaddr_in sa;
    int fd;

    if (!name || !*name) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    cdj_panelsock_addr(name, &sa);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    qemu_socket_set_nonblock(fd);
    return fd;
}

static inline int cdj_panelkey_bind(const char *name)
{
    return cdj_panelsock_bind(name);
}

/* One datagram per press. */
static inline void cdj_panelkey_send_op(const char *name, unsigned off,
                                        int val, unsigned dur_ms,
                                        const char *op)
{
    char msg[64];
    int fd, n;

    fd = cdj_panelsock_connect(name);
    if (fd < 0) {
        return;
    }
    n = snprintf(msg, sizeof(msg), "0x%02x:%d:%u:%s", off, val, dur_ms,
                 op ? op : "or");
    if (send(fd, msg, n, 0) < 0) {
        /* Nobody bound yet; MAIN is still booting. */
    }
    close(fd);
}

static inline void cdj_panelkey_send(const char *name, unsigned off,
                                     unsigned mask, unsigned dur_ms)
{
    cdj_panelkey_send_op(name, off, (int)mask, dur_ms, "or");
}

/*
 * Touch screen. The LCD's resistive panel is digitised by the front-panel MCU
 * and its raw values ride in the panel report MAIN receives, so clicks in the
 * GUI window are forwarded over the same socket:
 *
 *      <x>:<y>:<down>:touch    finger down (1) / up (0); moves while down
 *      <x>:<y>:<ms>:tap        down now, up after <ms> (MAIN enforces a floor)
 *
 * x is 0..799 and y 0..479 in LCD pixels; MAIN converts to raw panel units.
 */
#define CDJ_TOUCH_W        800
#define CDJ_TOUCH_H        480
#define CDJ_TOUCH_OP       "touch"
#define CDJ_TOUCH_TAP_OP   "tap"
#define CDJ_TOUCH_ENV      "CDJ_TOUCH"

typedef struct CdjTouch {
    uint16_t x, y;          /* last position, panel pixels                   */
    bool down;              /* a finger is on the glass                      */
    uint32_t tap_ms;        /* non-zero: a timed tap, release after this     */
    uint64_t events;        /* messages parsed, for the exit report          */
} CdjTouch;

static inline bool cdj_touch_enabled(void)
{
    const char *v = getenv(CDJ_TOUCH_ENV);

    return v && *v && strcmp(v, "0");
}

/* Parse one datagram; false if it is not a touch message. */
static inline bool cdj_touch_parse(const char *msg, CdjTouch *t)
{
    char op[16] = "";
    int x, y, v;
    bool tap;

    if (sscanf(msg, "%i:%i:%i:%15s", &x, &y, &v, op) != 4) {
        return false;
    }
    tap = !strcmp(op, CDJ_TOUCH_TAP_OP);
    if (!tap && strcmp(op, CDJ_TOUCH_OP)) {
        return false;
    }
    t->x = (uint16_t)MIN(MAX(x, 0), CDJ_TOUCH_W - 1);
    t->y = (uint16_t)MIN(MAX(y, 0), CDJ_TOUCH_H - 1);
    t->down = tap || v != 0;
    t->tap_ms = tap ? (uint32_t)MAX(v, 1) : 0;
    t->events++;
    return true;
}

static inline void cdj_touch_send(const char *name, const CdjTouch *t)
{
    char msg[48];
    int fd, n;

    fd = cdj_panelsock_connect(name);
    if (fd < 0) {
        return;
    }
    n = snprintf(msg, sizeof(msg), "%u:%u:%u:" CDJ_TOUCH_OP,
                 t->x, t->y, t->down ? 1u : 0u);
    if (send(fd, msg, n, 0) < 0) {
        /* Nobody bound yet; MAIN is still booting. */
    }
    close(fd);
}

#endif /* CDJ_PANELKEYS_H */
