/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The M16C/63 peripherals, set up the way the CDJ-900 GUI firmware sets them
 * up: a timer in timer mode and in one-shot mode, the display UART fed by a
 * DMA channel, the link UART as a clock-synchronous slave with DMA in both
 * directions, the CRC unit watching the receive register, and the ports.
 * A tiny flash image supplies the main loop and one interrupt handler.
 */
#include "../m16c63.h"
#include <stdio.h>
#include <string.h>

#define FLASH_BASE 0xFC000u
#define FLASH_SIZE 0x4000u
#define COUNTER    0x0500u    /* bumped by the interrupt handler */
#define HANDLER    0xFC100u

static uint8_t flash[FLASH_SIZE];
static m16c63 *chip;
static m16c_core *cpu;
static int failures, checks;
static uint8_t tx_bytes[3][64];
static uint64_t tx_cycle[3][64];
static unsigned tx_count[3];
static uint8_t pins[11], port_latch[11], port_dir[11];

static void uart_tx(void *opaque, unsigned unit, uint8_t byte)
{
    if (tx_count[unit] < 64) {
        tx_bytes[unit][tx_count[unit]] = byte;
        tx_cycle[unit][tx_count[unit]] = m16c_cycles(cpu);
        tx_count[unit]++;
    }
}

static uint8_t port_in(void *opaque, unsigned port)
{
    return pins[port];
}

static void port_out(void *opaque, unsigned port, uint8_t latch, uint8_t dir)
{
    port_latch[port] = latch;
    port_dir[port] = dir;
}

static void check(const char *name, const char *what, uint64_t got, uint64_t want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: %s = 0x%llx, want 0x%llx\n", name, what,
               (unsigned long long)got, (unsigned long long)want);
    }
}

#define W8(a, v)  m16c_write(cpu, a, v, 1)
#define W16(a, v) m16c_write(cpu, a, v, 2)
#define R8(a)     m16c_read(cpu, a, 1)
#define R16(a)    m16c_read(cpu, a, 2)

/* The main loop spins in place; the handler for every vector counts in
 * COUNTER[vector]. */
static void init(void)
{
    static const m16c63_ops ops = { NULL, uart_tx, port_in, port_out, NULL };
    static const uint8_t spin[] = { 0xfe, 0xff };
    static const uint8_t handler[] = {
        0xa7, COUNTER & 0xFF, COUNTER >> 8,     /* INC.B COUNTER */
        0xfb,                                   /* REIT */
    };

    if (chip) {
        m16c63_free(chip);
    }
    memset(flash, 0xff, sizeof(flash));
    memcpy(flash, spin, sizeof(spin));
    memcpy(flash + (HANDLER - FLASH_BASE), handler, sizeof(handler));
    flash[0x3FFC] = 0x00;                       /* reset vector 0xFC000 */
    flash[0x3FFD] = 0xC0;
    flash[0x3FFE] = 0x0F;
    flash[0x3FFF] = 0x00;
    for (unsigned vec = 0; vec < 64; vec++) {
        unsigned at = 0xFFD00 - FLASH_BASE + vec * 4;

        flash[at] = HANDLER & 0xFF;
        flash[at + 1] = HANDLER >> 8 & 0xFF;
        flash[at + 2] = HANDLER >> 16;
        flash[at + 3] = 0;
    }
    chip = m16c63_new(flash, FLASH_BASE, FLASH_SIZE, &ops);
    cpu = m16c63_cpu(chip);
    m16c_set_reg(cpu, M16C_INTB, 0xFFD00);
    m16c_set_reg(cpu, M16C_ISP, 0x6000);
    m16c_set_reg(cpu, M16C_FLG, M16C_I);
    memset(tx_count, 0, sizeof(tx_count));
    memset(pins, 0, sizeof(pins));
    W8(COUNTER, 0);
}

static void test_timer_mode(void)
{
    init();
    W8(0x57, 5);                                /* TA2 interrupt, level 5 */
    W8(0x338, 0x40);                            /* TA2: timer mode, f8 */
    W16(0x32A, 0x11B);
    W8(0x320, 0x04);                            /* TA2 count start */
    m16c63_run(chip, 284 * 8 * 10 + 100);
    check("TA2 timer mode", "interrupts in ten periods", R8(COUNTER), 10);
}

static void test_one_shot(void)
{
    init();
    W8(0x55, 1);                                /* TA0 interrupt, level 1 */
    W8(0x336, 0x42);                            /* TA0: one-shot, f8 */
    W16(0x326, 0x4CD);
    W8(0x320, 0x01);
    m16c63_run(chip, 100000);
    check("TA0 one-shot", "no start, no interrupt", R8(COUNTER), 0);
    W8(0x322, 0x01);                            /* TA0 one-shot start */
    m16c63_run(chip, 0x4CD * 8 - 100);
    check("TA0 one-shot", "not yet", R8(COUNTER), 0);
    m16c63_run(chip, 400);
    check("TA0 one-shot", "fires once", R8(COUNTER), 1);
    m16c63_run(chip, 100000);
    check("TA0 one-shot", "and stops", R8(COUNTER), 1);
    check("TA0 one-shot", "start flag reads back 0", R8(0x322), 0);
}

static void test_interrupt_priority(void)
{
    init();
    W8(0x55, 0);                                /* TA0 at level 0: disabled */
    W8(0x336, 0x42);
    W16(0x326, 10);
    W8(0x320, 0x01);
    W8(0x322, 0x01);
    m16c63_run(chip, 2000);
    check("level 0", "IR set, not taken", R8(0x55) & 8, 8);
    check("level 0", "handler not run", R8(COUNTER), 0);
    W8(0x55, 0x0B);                             /* level 3, keeping IR */
    m16c63_run(chip, 200);
    check("level raised", "taken", R8(COUNTER), 1);
    check("level raised", "IR cleared by the acknowledge", R8(0x55) & 8, 0);
}

/* The display UART as the firmware drives it: the first byte by hand, the
 * rest by DMA on every transmit-buffer-empty. */
static void test_display_dma(void)
{
    init();
    for (unsigned i = 0; i < 5; i++) {
        W8(0x1000 + i, 0x10 + i);
    }
    W8(0x2A8, 0x01);                            /* clock-synchronous, own clock */
    W8(0x2A9, 0x01);                            /* BRG */
    W8(0x2AC, 0x10);
    W8(0x2AD, 0x01);                            /* transmit enable */
    W8(0x398, 0x15);                            /* DMA0: display UART transmit */
    W8(0x18C, 0x11);
    W16(0x184, 0x2AA);
    W16(0x186, 0);
    W16(0x180, 0x1001);
    W16(0x182, 0);
    W16(0x188, 3);                              /* four transfers */
    W8(0x18C, 0x19);
    W8(0x2AA, 0x10);                            /* first byte by hand */
    m16c63_run(chip, 5 * 40 + 100);
    check("display DMA", "bytes shifted out", tx_count[2], 5);
    for (unsigned i = 0; i < 5; i++) {
        check("display DMA", "byte", tx_bytes[2][i], 0x10 + i);
    }
    /* Four byte times of 8 * 2 * (BRG + 1) cycles; the clock the bytes are
     * stamped with only advances in whole instructions. */
    check("display DMA", "four byte times", tx_cycle[2][4] - tx_cycle[2][0] >= 128 &&
          tx_cycle[2][4] - tx_cycle[2][0] < 140, 1);
    check("display DMA", "DMAE cleared at the end", R8(0x18C) & 8, 0);
    check("display DMA", "interrupt requested", R8(0x4B) & 8, 8);

    /* Armed again without rewriting the count: the whole burst repeats. */
    tx_count[2] = 0;
    W16(0x180, 0x1001);
    W8(0x18C, 0x11);
    W8(0x18C, 0x19);
    W8(0x2AA, 0x10);
    m16c63_run(chip, 5 * 40 + 100);
    check("display DMA again", "bytes shifted out", tx_count[2], 5);
}

static void test_link_slave(void)
{
    static const uint8_t tx[4] = { 0xA0, 0xA1, 0xA2, 0xA3 };
    static const uint8_t rx[4] = { 0xB0, 0xB1, 0xB2, 0xB3 };
    uint8_t out[4];

    init();
    for (unsigned i = 0; i < 4; i++) {
        W8(0x2100 + i, tx[i]);
    }
    W8(0x258, 0x09);                            /* synchronous, external clock */
    W8(0x25D, 0x05);                            /* transmit and receive enable */
    W8(0x390, 0x0F);                            /* DMA2: link UART */
    W8(0x1AC, 0x11);
    W16(0x1A4, 0x25A);
    W16(0x1A6, 0);
    W16(0x1A0, 0x2101);
    W16(0x1A2, 0);
    W16(0x1A8, 2);                              /* three transfers + the first */
    W8(0x392, 0x0F);                            /* DMA3: link UART */
    W8(0x1BC, 0x21);
    W16(0x1B4, 0x2200);
    W16(0x1B6, 0);
    W16(0x1B0, 0x25E);
    W16(0x1B2, 0);
    W16(0x1B8, 3);                              /* four transfers */
    W8(0x1AC, 0x19);
    W8(0x1BC, 0x29);
    W8(0x25A, 0xA0);                            /* first transmit byte */
    W8(0x6A, 4);                                /* DMA3 interrupt, level 4 */

    for (unsigned i = 0; i < 4; i++) {
        out[i] = m16c63_link_clock(chip, rx[i]);
        m16c63_run(chip, 50);
    }
    for (unsigned i = 0; i < 4; i++) {
        check("link slave", "byte shifted out to MAIN", out[i], tx[i]);
        check("link slave", "byte DMAed in", R8(0x2200 + i), rx[i]);
    }
    check("link slave", "receive DMA done", R8(0x1BC) & 8, 0);
    check("link slave", "frame interrupt taken", R8(COUNTER), 1);
}

static uint16_t crc_ccitt(const uint8_t *data, unsigned len)
{
    uint16_t crc = 0;

    while (len--) {
        crc ^= *data++ << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

static void test_crc(void)
{
    static const uint8_t data[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };

    init();
    W8(0x3B6, 0x80);                            /* CRC-CCITT, MSB first */
    W16(0x3B4, 0x425E);                         /* watch reads of the receive register */
    W16(0x3BC, 0);
    W8(0x258, 0x09);
    W8(0x25D, 0x04);
    for (unsigned i = 0; i < 6; i++) {
        m16c63_link_clock(chip, data[i]);
        R8(0x25E);
    }
    check("CRC snoop", "CRCD", R16(0x3BC), crc_ccitt(data, 6));

    W16(0x3B4, 0x025E);                         /* snooping off */
    W16(0x3BC, 0);
    m16c63_link_clock(chip, 0x99);
    R8(0x25E);
    check("CRC snoop off", "CRCD", R16(0x3BC), 0);

    W16(0x3BC, 0);
    for (unsigned i = 0; i < 6; i++) {
        W8(0x3BE, data[i]);
    }
    check("CRCIN", "CRCD", R16(0x3BC), crc_ccitt(data, 6));
    check("CRC-CCITT", "known value", crc_ccitt((const uint8_t *)"123456789", 9), 0x31C3);
}

static void test_ports(void)
{
    init();
    pins[4] = 0x0A;
    W8(0x3EA, 0xF0);                            /* P4: high nibble output */
    W8(0x3E8, 0xA5);
    check("port", "outputs read the latch, inputs the pins", R8(0x3E8), 0xAA);
    check("port", "latch seen by the board", port_latch[4], 0xA5);
    check("port", "direction seen by the board", port_dir[4], 0xF0);
}

int main(void)
{
    test_timer_mode();
    test_one_shot();
    test_interrupt_priority();
    test_display_dma();
    test_link_slave();
    test_crc();
    test_ports();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
