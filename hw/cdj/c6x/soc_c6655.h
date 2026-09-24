/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TMS320C6655 SoC peripheral models -- public interface.
 *
 * Only the blocks the Pioneer DSP program (stage 1 + the 576 KB app) touches,
 * modelled against the TI user guides and checked against the program's own
 * drivers; each soc_*.c header names the program addresses it was checked
 * against.
 *
 * The SoC is time-driven by the integrator on the virtual clock: it never reads
 * a host clock. It raises CPU interrupts through c66x_set_irq() and moves DMA
 * data through the memory bus it is given, so it links unchanged into the
 * offline runner and into the QEMU device.
 */
#ifndef SOC_C6655_H
#define SOC_C6655_H

#include <stdint.h>
#include <stddef.h>
#include "c66x.h"

typedef struct c6655_soc c6655_soc;

typedef struct c6655_soc_config {
    c66x_core *core;          /* interrupt sink; may be NULL in unit tests      */

    /* DSP memory as the DMA masters (uPP, EDMA3) see it. The SoC routes its own
     * register windows itself and folds the CorePac global alias 0x1080_0000
     * onto L2 before calling this. */
    c66x_bus mem;

    /* Clocks. 0 selects the default. */
    uint64_t timer_clk_hz;    /* Timer64 input; default 1 GHz / 6 (inferred)   */
    uint64_t spi_clk_hz;      /* SPI module clock; default 1 GHz / 6 (inferred) */
    uint64_t mcbsp_clks_hz;   /* CLKS pin; default 22.5792 MHz (X1202)         */

    /* External seams. Any may be NULL. */
    void *opaque;
    /* One SPI word, full duplex, DSP as master. Returns the slave's reply. */
    uint16_t (*spi_xfer)(void *opaque, uint16_t tx, unsigned bits,
                         unsigned csnr, int cshold);
    /* One McBSP word leaving DX (port 0 or 1). */
    void (*mcbsp_tx)(void *opaque, unsigned port, uint32_t word, unsigned bits);
    /* A GPIO pin's driven level changed (outputs only). */
    void (*gpio_out)(void *opaque, unsigned pin, int level);
    /* Every SET_DATA (set = 1) / CLR_DATA (set = 0) write, even when no level
     * changes. OUT_DATA resets to 0, so pin 25 already reads "booted" once
     * stage 1 makes it an output (0x00801090); board glue that wants MAIN's
     * PZDR.6 high until the real gpio_clear(25) at 0x00801528 keys on this. */
    void (*gpio_setclr)(void *opaque, uint32_t mask, int set);
    /* Unmodelled register access and other one-line diagnostics. */
    void (*log)(void *opaque, const char *msg);
} c6655_soc_config;

c6655_soc *c6655_soc_new(const c6655_soc_config *cfg);
void       c6655_soc_free(c6655_soc *s);
void       c6655_soc_reset(c6655_soc *s);

/* Register access. owns() tells the integrator which addresses to route here;
 * everything else is RAM or unmapped. */
int      c6655_soc_owns(uint32_t addr);
uint32_t c6655_soc_read(c6655_soc *s, uint32_t addr, unsigned size);
void     c6655_soc_write(c6655_soc *s, uint32_t addr, uint32_t val, unsigned size);
/* The same two calls packaged for c66x_new(). */
c66x_bus c6655_soc_bus(c6655_soc *s);

/* Virtual time. advance() runs every timer, SPI and McBSP event due in the
 * interval, in order. next_event_ns() is the absolute time of the next one
 * (UINT64_MAX if none), so a caller can bound its CPU quantum. */
void     c6655_soc_advance(c6655_soc *s, uint64_t ns);
uint64_t c6655_soc_now_ns(const c6655_soc *s);
uint64_t c6655_soc_next_event_ns(const c6655_soc *s);
/* The earliest event that can raise a CPU interrupt (<= UINT64_MAX). A core
 * idling in a busy loop may advance() straight to it: McBSP word events that
 * only feed EDMA3 in between still run inside advance(), they just no longer
 * bound the skip. */
uint64_t c6655_soc_next_irq_ns(const c6655_soc *s);
/* Diagnostics: advance calls, fast-path returns, event-loop passes,
 * next-event recomputations, McBSP words clocked, catch-up runs. */
void     c6655_soc_debug_stats(const c6655_soc *s, uint64_t out[6]);

/* uPP channel A receive: what the FPGA forwards from MAIN's CS6 ships. Bytes
 * fill the open (and one pending) DMA window; whatever arrives with no window
 * open is discarded and counted, as MAIN's whole-16 KB ships overshoot the
 * DSP's windows. Returns the number of bytes still queued. */
size_t c6655_soc_upp_rx(c6655_soc *s, const uint8_t *buf, size_t len);
size_t c6655_soc_upp_queued(const c6655_soc *s);
size_t c6655_soc_upp_dropped(const c6655_soc *s);

/* GPIO. set_input drives an input pin from outside (MAIN's PVDR.1 -> pin 21).
 * pin_level reads what a pin shows to the board: the output latch when the pin
 * is an output, otherwise the external level (undriven inputs read 1). */
void c6655_soc_gpio_set_input(c6655_soc *s, unsigned pin, int level);
int  c6655_soc_gpio_pin_level(const c6655_soc *s, unsigned pin);

#endif
