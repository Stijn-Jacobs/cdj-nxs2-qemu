/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Standalone RSP server around a bfin_core running gdb_prog.s, for trying a
 * real bfin-elf-gdb against the session without the emulator:
 *   gdb_server 1234   then   (gdb) target remote :1234
 */
#include "bfin_priv.h"
#include "bfin_gdb.h"
#include "gdb_prog.h"
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint8_t ram[0x10000];
static int fd;

static uint32_t bus_read(void *o, uint32_t a, unsigned s) { return 0; }
static void bus_write(void *o, uint32_t a, uint32_t v, unsigned s) { }

static void sink(void *o, const uint8_t *b, size_t n)
{
    if (write(fd, b, n) < 0) {
        perror("write");
    }
}

int main(int argc, char **argv)
{
    static const bfin_bus bus = { NULL, bus_read, bus_write };
    struct sockaddr_in sa = { .sin_family = AF_INET,
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                              .sin_port = htons(argc > 1 ? atoi(argv[1]) : 1234) };
    bfin_core *c = bfin_new(&bus);
    bfin_gdb_ops ops = { .write = sink };
    bfin_gdb *g;
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;

    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memcpy(ram, gdb_prog, sizeof gdb_prog);
    bfin_map_ram(c, 0, sizeof ram, ram);
    bfin_reset(c, START);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 1)) {
        perror("listen");
        return 1;
    }
    fd = accept(ls, NULL, NULL);
    g = bfin_gdb_new(c, &ops);
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        uint8_t buf[4096];
        bfin_gdb_run run = bfin_gdb_run_request(g);

        if (poll(&p, 1, run == BFIN_GDB_CONT ? 0 : -1) > 0) {
            ssize_t n = read(fd, buf, sizeof buf);

            if (n <= 0) {
                break;
            }
            bfin_gdb_feed(g, buf, n);
            if (bfin_gdb_detached(g)) {
                break;
            }
            if (bfin_gdb_interrupted(g)) {
                bfin_gdb_stopped(g, BFIN_STOP_BUDGET);
            }
        }
        run = bfin_gdb_run_request(g);
        if (run == BFIN_GDB_STEP) {
            bfin_gdb_stopped(g, bfin_stepi(c));
        } else if (run == BFIN_GDB_CONT) {
            bfin_stop st = bfin_step(c, 1000, NULL);

            if (st != BFIN_STOP_BUDGET && st != BFIN_STOP_IDLE) {
                bfin_gdb_stopped(g, st);
            }
        }
    }
    return 0;
}
