/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPORT1 RX DMA3: row events, partial bursts and descriptor completion. */
#include "bf531.h"
#include "bfin_priv.h"
#include <stdio.h>
#include <string.h>

#define DMA3        0xFFC00CC0u
#define SIC_ISR     0xFFC00120u
#define SIC_IMASK   0xFFC0010Cu
#define RX_IRQ      (1u << 11)
#define START       0x04
#define CONFIG      0x08
#define XCOUNT      0x10
#define XMOD        0x14
#define YCOUNT      0x18
#define YMOD        0x1C
#define CURR_ADDR   0x24
#define IRQ_STATUS  0x28
#define CURR_X      0x30
#define CURR_Y      0x38
#define DMA2D       0x10
#define DI_EN       0x80
#define DI_SEL      0x100
#define DMA_DONE    1
#define DMA_RUN     8
#define TARGET      0x1000u

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            printf("FAIL line %d: ", __LINE__);                          \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
            failures++;                                                 \
        }                                                               \
    } while (0)

static int failures;
static bf531 *chip;
static uint8_t packet[64];

static uint32_t rd(unsigned r) { return bf531_bus_read(chip, DMA3 + r, 4); }
static void wr(unsigned r, uint32_t v) { bfin_store(bf531_core(chip), DMA3 + r, v, 4); }

static void fresh(unsigned cfg)
{
    static const bf531_host host;

    if (chip) {
        bf531_free(chip);
    }
    chip = bf531_new(0x100000, &host, NULL);
    bfin_reset(bf531_core(chip), 0); /* zero-filled SDRAM executes NOPs */
    wr(START, TARGET);
    wr(XCOUNT, 8);
    wr(XMOD, 2);
    wr(YCOUNT, 4);
    wr(YMOD, 2);
    wr(CONFIG, cfg | DI_EN | 1);
}

static void pump(void) { bf531_stepi(chip); }

static void state(unsigned x, unsigned y, uint32_t addr, unsigned status)
{
    CHECK(rd(CURR_X) == x && rd(CURR_Y) == y && rd(CURR_ADDR) == addr &&
          rd(IRQ_STATUS) == status,
          "state: X=%u Y=%u addr=%x status=%x, want %u %u %x %x",
          rd(CURR_X), rd(CURR_Y), rd(CURR_ADDR), rd(IRQ_STATUS),
          x, y, addr, status);
    CHECK(!!(bf531_bus_read(chip, SIC_ISR, 4) & RX_IRQ) == !!(status & DMA_DONE),
          "SIC follows DMA_DONE");
    if (status & DMA_DONE) {
        CHECK(bf531_bus_read(chip, SIC_IMASK, 4) & RX_IRQ, "RX IRQ unmasked");
    }
}

static void ack(void)
{
    wr(IRQ_STATUS, DMA_DONE);
    CHECK(!(bf531_bus_read(chip, SIC_ISR, 4) & RX_IRQ), "SIC cleared by ack");
}

int main(void)
{
    for (unsigned i = 0; i < sizeof(packet); i++) {
        packet[i] = i + 1;
    }
    fresh(DMA2D | DI_SEL);
    bf531_sport1_rx(chip, packet, 48);
    for (unsigned row = 1; row <= 3; row++) {
        pump();
        state(8, 4 - row, TARGET + row * 16, DMA_RUN | DMA_DONE);
        ack();
    }
    CHECK(!memcmp(bf531_mem(chip, TARGET, 48), packet, 48), "48-byte frame");
    pump();
    state(8, 1, TARGET + 48, DMA_RUN);
    bf531_sport1_rx(chip, packet + 48, 10);
    pump();
    state(3, 1, TARGET + 58, DMA_RUN);
    bf531_sport1_rx(chip, packet + 58, 6);
    pump();
    state(8, 0, TARGET + 64, DMA_DONE);
    CHECK(!memcmp(bf531_mem(chip, TARGET, 64), packet, 64), "completed data");
    ack();
    pump();
    state(8, 0, TARGET + 64, 0);
    printf("sport_dma2d_test: row interrupts, short burst, flow 0 checked\n");

    fresh(DMA2D);
    bf531_sport1_rx(chip, packet, 64);
    for (unsigned row = 1; row <= 4; row++) {
        pump();
        state(8, 4 - row, TARGET + row * 16, row == 4 ? DMA_DONE : DMA_RUN);
    }
    CHECK(!memcmp(bf531_mem(chip, TARGET, 64), packet, 64), "DI_SEL off data");
    printf("sport_dma2d_test: DI_SEL off checked\n");

    fresh(DI_SEL);
    wr(XMOD, 8); /* 1D retains the existing contiguous byte-stream behavior. */
    bf531_sport1_rx(chip, packet, 10);
    pump();
    state(3, 0, TARGET + 10, 0);
    bf531_sport1_rx(chip, packet + 10, 6);
    pump();
    state(0, 0, TARGET + 16, DMA_DONE);
    CHECK(!memcmp(bf531_mem(chip, TARGET, 16), packet, 16), "1D data");
    printf("sport_dma2d_test: 1D regression checked\n");

    /* Unequal signed modifies expose an accidental extra X_MODIFY at a row end. */
    fresh(DMA2D | DI_SEL);
    wr(XMOD, (uint16_t)-2);
    wr(YMOD, 32);
    bf531_sport1_rx(chip, packet, 32);
    for (unsigned row = 1; row <= 2; row++) {
        pump();
        state(8, 4 - row, TARGET + row * 18, DMA_RUN | DMA_DONE);
        for (unsigned x = 0; x < 8; x++) {
            CHECK(!memcmp(bf531_mem(chip, TARGET + (row - 1) * 18 - x * 2, 2),
                          packet + (row - 1) * 16 + x * 2, 2), "strided word");
        }
        ack();
    }
    printf("sport_dma2d_test: signed X_MODIFY and replacement Y_MODIFY checked\n");
    bf531_free(chip);
    printf(failures ? "sport_dma2d_test: %d FAILED\n" :
                     "sport_dma2d_test: all passed\n", failures);
    return failures != 0;
}
