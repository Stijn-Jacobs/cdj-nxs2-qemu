/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Renesas M16C/63 as the CDJ-900's GUI processor: the m16c core with 256 KiB
 * of flash, 27 KiB of RAM and the peripherals the GUI firmware uses (the
 * SFR window at 0x000-0x3FF): interrupt control, three clock-synchronous
 * UARTs, four DMA channels, the timers, the CRC unit and the ports.
 *
 * The chip talks to the rest of the board through m16c63_ops: the byte-wise
 * link to MAIN, the serial stream to the display, and the port pins. Time is
 * counted in f1 cycles of the core (20 MHz on the CDJ-900), so a run is
 * "this many cycles", not "this many instructions".
 */
#ifndef M16C63_H
#define M16C63_H

#include "m16c.h"

typedef struct m16c63 m16c63;

typedef struct m16c63_ops {
    void *opaque;
    /* A byte the display UART (unit 2) or the UART at 0x272 (unit 1) has
     * shifted out. */
    void (*uart_tx)(void *opaque, unsigned unit, uint8_t byte);
    /* Pin levels of port 0-10 for the bits its direction register leaves as
     * inputs, and a port written or its direction changed. */
    uint8_t (*port_in)(void *opaque, unsigned port);
    void (*port_out)(void *opaque, unsigned port, uint8_t latch, uint8_t dir);
    /* First access to an address nothing here models: an SFR, or memory
     * outside the flash and RAM. pc is where the core was. */
    void (*unmodelled)(void *opaque, uint32_t addr, int write, uint32_t val,
                       uint32_t pc);
} m16c63_ops;

m16c63 *m16c63_new(const uint8_t *flash, uint32_t base, uint32_t size,
                   const m16c63_ops *ops);
void    m16c63_free(m16c63 *s);
void    m16c63_reset(m16c63 *s);
m16c_core *m16c63_cpu(m16c63 *s);

/* Runs the chip for at least this many f1 cycles; returns why it stopped
 * early (undefined instruction, break address) or M16C_STOP_BUDGET. */
m16c_stop m16c63_run(m16c63 *s, uint64_t cycles);

/* MAIN clocks one byte over the link UART (unit 0, a clock-synchronous
 * slave): the chip receives in and returns the byte it shifted out at the
 * same time, 0xFF when it had none loaded. */
uint8_t m16c63_link_clock(m16c63 *s, uint8_t in);

#endif
