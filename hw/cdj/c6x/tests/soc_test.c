/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Offline tests for the C6655 SoC models. Each test replays the register
 * sequence the DSP program itself writes (addresses in the comments) and checks
 * what the program then depends on. No core: c66x_set_irq is mocked.
 *
 *   make -C hw/cdj/c6x -f soc.mk O=~/build/c6x-soc test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../soc_c6655.h"

#define NS_PER_S_TEST 1000000000ull

/* ---- harness ---------------------------------------------------------------- */
static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned irq_pulses[16];
static int irq_level[16];
void c66x_set_irq(c66x_core *c, int line, int level)
{
    (void)c;
    if (level && !irq_level[line])
        irq_pulses[line]++;
    irq_level[line] = level;
}

#define L2_BASE 0x00800000u
#define L2_SIZE 0x00100000u
#define DDR_BASE 0x80000000u
#define DDR_SIZE 0x01000000u
static uint8_t l2[L2_SIZE], ddr[DDR_SIZE];

static uint8_t *mem_at(uint32_t addr, unsigned size)
{
    if (addr - L2_BASE <= L2_SIZE - size) return l2 + (addr - L2_BASE);
    if (addr - DDR_BASE <= DDR_SIZE - size) return ddr + (addr - DDR_BASE);
    return NULL;
}

static uint32_t mem_read(void *o, uint32_t addr, unsigned size)
{
    uint8_t *m = mem_at(addr, size);
    uint32_t v = 0;
    if (!m) { printf("  mem read outside RAM 0x%08x\n", addr); return 0; }
    for (unsigned i = 0; i < size; i++) v |= (uint32_t)m[i] << (8 * i);
    return v;
}

static void mem_write(void *o, uint32_t addr, uint32_t val, unsigned size)
{
    uint8_t *m = mem_at(addr, size);
    if (!m) { printf("  mem write outside RAM 0x%08x\n", addr); return; }
    for (unsigned i = 0; i < size; i++) m[i] = val >> (8 * i);
}

static uint16_t (*spi_peer)(uint16_t tx, unsigned bits, unsigned csnr);
static unsigned spi_words, spi_last_csnr;
static uint16_t spi_xfer(void *o, uint16_t tx, unsigned bits, unsigned csnr, int cshold)
{
    spi_words++;
    spi_last_csnr = csnr;
    return spi_peer ? spi_peer(tx, bits, csnr) : 0;
}

static uint32_t mcbsp_log[64];
static unsigned mcbsp_n[2], mcbsp_bits;
static uint64_t mcbsp_sum[2];
static void mcbsp_tx(void *o, unsigned port, uint32_t word, unsigned bits)
{
    mcbsp_sum[port] = mcbsp_sum[port] * 1000003u + word + 1;
    if (port == 0 && mcbsp_n[0] < 64)
        mcbsp_log[mcbsp_n[0]] = word;
    mcbsp_n[port]++;
    mcbsp_bits = bits;
}

static int gpio_seen[32];
static void gpio_out(void *o, unsigned pin, int level)
{
    gpio_seen[pin] = level ? 1 : -1;
}

static uint32_t setclr_mask[2];
static void gpio_setclr(void *o, uint32_t mask, int set)
{
    setclr_mask[set] |= mask;
}

static void log_cb(void *o, const char *msg)
{
    if (getenv("SOC_TEST_VERBOSE"))
        printf("    %s\n", msg);
}

static c6655_soc *S;
static uint32_t r32(uint32_t a) { return c6655_soc_read(S, a, 4); }
static void w32(uint32_t a, uint32_t v) { c6655_soc_write(S, a, v, 4); }
static void orw(uint32_t a, uint32_t v) { w32(a, r32(a) | v); }
static void andw(uint32_t a, uint32_t v) { w32(a, r32(a) & v); }

static void fresh(void)
{
    if (S) c6655_soc_free(S);
    memset(irq_pulses, 0, sizeof irq_pulses);
    memset(irq_level, 0, sizeof irq_level);
    memset(l2, 0, sizeof l2);
    memset(ddr, 0, sizeof ddr);
    memset(gpio_seen, 0, sizeof gpio_seen);
    memset(mcbsp_n, 0, sizeof mcbsp_n);
    memset(mcbsp_sum, 0, sizeof mcbsp_sum);
    memset(setclr_mask, 0, sizeof setclr_mask);
    spi_words = 0;
    spi_peer = NULL;
    c6655_soc_config cfg = {
        .core = (c66x_core *)1,
        .mem = { NULL, mem_read, mem_write },
        .spi_xfer = spi_xfer, .mcbsp_tx = mcbsp_tx, .gpio_out = gpio_out,
        .gpio_setclr = gpio_setclr, .log = log_cb,
    };
    S = c6655_soc_new(&cfg);
}

/* ---- the program's own interrupt setup -------------------------------------- */

/* app 0x8007E260: INTC */
static void app_intc_init(void)
{
    for (int i = 0; i < 4; i++) w32(0x01800040 + 4 * i, 0xFFFFFFFF);
    w32(0x01800080, 15);
    for (int i = 1; i < 4; i++) w32(0x01800080 + 4 * i, 0);
    w32(0x01800104, 0x40184419);
    w32(0x01800108, 0x0B0A1B16);
    w32(0x0180010C, 0x0F0E0D0C);
}

/* 0x0080C178: ENA_STATUS read-modify-write (clears whatever else is pending in
 * that word too, as the program does). */
static void cic_clear_status(unsigned evt)
{
    uint32_t a = 0x02600280 + ((evt >> 3) & ~3u);
    w32(a, r32(a) | 1u << (evt & 31));
}

/* app 0x8007E4A0 + 0x0080C140: route one CIC0 system event to a channel */
static void app_cic_route(unsigned evt, unsigned ch)
{
    w32(0x02600010, 0);
    w32(0x02600038, ch);
    w32(0x02600004, 0);
    w32(0x02600024, evt);
    uint32_t a = 0x02600400 + (evt & ~3u), sh = 8 * (evt & 3);
    w32(a, r32(a) & ~(ch << sh));
    w32(a, r32(a) | ch << sh);
    w32(0x02600028, evt);
    w32(0x02600034, ch);
    orw(0x02600010, 1);
    cic_clear_status(evt);
}

static void app_irq_init(void)
{
    app_intc_init();
    app_cic_route(145, 0);   /* TINT4L       -> ch0 -> event 22 -> INT8 */
    app_cic_route(25, 2);    /* EDMA3_CC_INT1 -> ch2 -> event 24 -> INT6 */
    app_cic_route(156, 3);   /* UPPINT       -> ch3 -> event 25 -> INT4 */
    app_cic_route(151, 4);   /* TINT5L       -> ch4 -> event 26 (not muxed) */
    app_cic_route(26, 5);    /* EDMA3_CC_INT2 -> ch5 -> event 27 -> INT9 */
}

/* ISR prologue/epilogue around a CIC-sourced interrupt (e.g. 0x00803F68). */
static void isr_enter(unsigned hint, unsigned evt)
{
    andw(0x02601500, ~(1u << hint));
    cic_clear_status(evt);
}
static void isr_leave(unsigned hint) { orw(0x02601500, 1u << hint); }

/* ---- tests ------------------------------------------------------------------- */

static void test_ram_blocks(void)
{
    fresh();
    w32(0x0231013C, 0xFFFFFFFF);
    CHECK((r32(0x0231013C) & 1) == 0, "PLL GO bit must read done");
    CHECK(r32(0x0231013C) == 0xFFFFFFFE, "PLL register otherwise RAM-like: 0x%08x", r32(0x0231013C));
    w32(0x02620580, 0x0FFF0000);                 /* stage 1 0x00801920 */
    CHECK(r32(0x02620580) == 0x0FFF0000, "bootcfg RAM-like");
    CHECK(r32(0x02620020) & 1, "DEVSTAT little-endian");
    w32(0x01848200, 1);                          /* app 0x8007E920: MAR */
    CHECK(r32(0x01848200) == 1, "cache MAR RAM-like");
    w32(0x08000000, 0x1234); w32(0x21000010, 0x55);
    CHECK(r32(0x08000000) == 0x1234 && r32(0x21000010) == 0x55, "XMC/DDR3 ctl RAM-like");
    CHECK(c6655_soc_owns(0x02580024) && !c6655_soc_owns(0x00800000) && !c6655_soc_owns(0x80000000),
          "owns() covers registers, not RAM");
}

static void test_gpio_handshake(void)
{
    fresh();
    /* stage 1 0x008018E0: DIR |= 0x00720040, &= 0xF0733FCE */
    orw(0x02320010, 0x00720040);
    andw(0x02320010, 0xF0733FCE);
    CHECK(((r32(0x02320010) >> 21) & 1) == 1, "pin 21 stays an input");
    CHECK(((r32(0x02320010) >> 24) & 3) == 0, "pins 24, 25 are outputs");
    CHECK(c6655_soc_gpio_pin_level(S, 24) == 0 && gpio_seen[24] == -1, "pin 24 driven low after DIR");

    w32(0x02320018, 1u << 24);                   /* gpio_set(24) 0x008017F4 */
    CHECK(c6655_soc_gpio_pin_level(S, 24) == 1 && gpio_seen[24] == 1, "READY high");
    c6655_soc_gpio_set_input(S, 21, 1);          /* MAIN's PVDR.1 ack */
    CHECK((r32(0x02320020) >> 21) & 1, "ACK visible in IN_DATA");
    c6655_soc_gpio_set_input(S, 21, 0);
    CHECK(!((r32(0x02320020) >> 21) & 1), "ACK low visible");
    w32(0x0232001C, 1u << 24);                   /* gpio_clear(24) 0x00801834 */
    CHECK(c6655_soc_gpio_pin_level(S, 24) == 0, "READY low");
    CHECK(c6655_soc_gpio_pin_level(S, 25) == 0, "pin 25 low as soon as it is an output");
    w32(0x0232001C, 1u << 25);                   /* gpio_clear(25) 0x00801528 */
    CHECK(setclr_mask[0] & (1u << 25), "the booted write is visible even without a level change");

    fresh();
    CHECK(c6655_soc_gpio_pin_level(S, 25) == 1, "undriven pin 25 reads high before boot");
}

static void test_timers_and_intc(void)
{
    fresh();
    app_irq_init();
    /* app 0x8007E820: Timer0, Timer3, Timer4 as 32-bit unchained + reload */
    uint32_t base[3] = { 0x02200000, 0x02230000, 0x02240000 };
    uint32_t prd[3] = { 0, 166666, 500000 };
    for (int i = 0; i < 3; i++) {
        orw(base[i] + 0x24, 4);
        orw(base[i] + 0x24, 1u << 4);
        orw(base[i] + 0x24, 3);
        w32(base[i] + 0x10, 0);
        if (prd[i]) w32(base[i] + 0x18, prd[i]);
        orw(base[i] + 0x44, 1);
    }
    /* 0x8007E3A8 / 0x8007E380: start Timer3 and Timer4 continuous */
    w32(0x02230034, 166666);
    orw(0x02230044, 2); orw(0x02230020, 1u << 7);
    w32(0x02240034, 500000);
    orw(0x02240044, 2); orw(0x02240020, 1u << 7);
    /* 0x8007E3F0: Timer0 as a free-running timestamp, period 0x100000 */
    w32(0x02200018, 0x100000); w32(0x02200034, 0x100000);
    orw(0x02200044, 2); orw(0x02200020, 3u << 6);

    c6655_soc_advance(S, 30 * 1000000ull);       /* 30 ms */
    /* first match after 166,666 ticks, then every 166,667: 29 in 30 ms */
    CHECK(irq_pulses[5] == 29, "Timer3 TINT3L -> event 68 -> INT5 every 1 ms: %u", irq_pulses[5]);
    /* Timer4 goes through CIC0 ch0; without the ISR clearing it the output stays
     * asserted and only the first edge reaches INT8. */
    CHECK(irq_pulses[8] == 1, "Timer4 first edge -> INT8: %u", irq_pulses[8]);
    for (int k = 0; k < 9; k++) {                /* ISR 0x00803F68 each 3 ms */
        isr_enter(22, 145);
        isr_leave(22);
        c6655_soc_advance(S, 3 * 1000000ull);
    }
    CHECK(irq_pulses[8] >= 9, "with the ISR clearing CIC status, INT8 repeats: %u", irq_pulses[8]);
    /* 0x100001 ticks = 6.29 ms per period over 57 ms */
    CHECK(irq_pulses[7] == 9, "Timer0 TINT0L -> event 64 -> INT7: %u", irq_pulses[7]);

    /* The timestamp reader 0x8007E458 */
    uint32_t cnt = r32(0x02200010);
    CHECK(cnt > 0, "Timer0 counts: %u", cnt);
    uint64_t t0 = c6655_soc_now_ns(S);
    c6655_soc_advance(S, 600);                   /* 100 ticks at 1 GHz/6 */
    CHECK(r32(0x02200010) - cnt == 100, "Timer0 ticks at 166.67 MHz: %u", r32(0x02200010) - cnt);
    (void)t0;
    CHECK(c6655_soc_next_event_ns(S) > c6655_soc_now_ns(S), "next event in the future");
}

/* stage 1 0x008015E0 + 0x00801738 + 0x008016C0 */
static void stage1_upp_config(void)
{
    orw(0x02580004, 0x10);
    andw(0x02580004, ~0x10u); andw(0x02580004, ~4u); andw(0x02580004, ~2u); orw(0x02580004, 1);
    w32(0x02580010, 0x02020000);
    w32(0x02580014, 0x18);
    andw(0x0258001C, ~3u);
    w32(0x02580008, 0);
    orw(0x0258002C, 0x10);
    orw(0x02580028, 0x08);
    orw(0x02580004, 0x08);
}

static void stage1_arm(void)
{
    w32(0x02580040, 0x108C8800);
    w32(0x02580044, 0x00014000);
    w32(0x02580048, 0);
}

static void test_upp_stage1_windows(void)
{
    fresh();                                     /* stage 1: no interrupt routing */
    stage1_upp_config();
    uint8_t buf[0x4100];
    for (int i = 0; i < 0x4100; i++) buf[i] = (uint8_t)(i * 7 + 3);

    /* bytes that arrive before any window is armed wait (MAIN ships as soon as
     * READY drops); a ship's overshoot past the window it completes is dropped */
    c6655_soc_upp_rx(S, buf, 100);
    CHECK(c6655_soc_upp_queued(S) == 100, "queued before arm");
    stage1_arm();
    CHECK(c6655_soc_upp_queued(S) == 0, "drained on arm");
    CHECK((r32(0x02580058) & 1) == 1, "UPIS2.ACT while filling");
    c6655_soc_upp_rx(S, buf + 100, 0x4000 - 100 + 50);   /* overshoot by 50 */
    CHECK(memcmp(l2 + 0xC8800, buf, 0x4000) == 0, "16 KB window lands at L2 0x008C8800");
    CHECK(c6655_soc_upp_queued(S) == 0 && c6655_soc_upp_dropped(S) == 50,
          "overshoot discarded: queued %zu dropped %zu", c6655_soc_upp_queued(S), c6655_soc_upp_dropped(S));
    CHECK((r32(0x02580024) & 8) == 8, "UPIER.EOWI set");
    CHECK((r32(0x02580058) & 1) == 0, "window done, not active");
    CHECK(irq_pulses[4] == 0, "stage 1 polls: nothing routes UPPINT yet: %u", irq_pulses[4]);
    w32(0x02580024, 8); w32(0x02580024, 0x10);
    CHECK((r32(0x02580024) & 8) == 0, "EOWI cleared by write 1");
    stage1_arm();
    CHECK(l2[0xC8800] == buf[0], "no stale bytes flow into the next window");
    CHECK((r32(0x02580058) & 1) == 1, "next window open and empty");
}

/* The audio shape seen in the machine: MAIN ships whole 16 KB units into the
 * DSP's 0xFC00 window, then 0x40 bytes into its 0x18-byte record window
 * (app 0x0080B560 via request type 6). The record must be MAIN's, not the MP3
 * overshoot. */
static void test_upp_audio_overshoot(void)
{
    fresh();
    stage1_upp_config();
    static uint8_t mp3[0x10000];
    for (int i = 0; i < 0x10000; i++) mp3[i] = 0xFF;
    w32(0x02580040, 0x108C9390); w32(0x02580044, 0x1FC00); andw(0x02580048, ~0xFFF8u);
    for (int k = 0; k < 4; k++)
        c6655_soc_upp_rx(S, mp3 + k * 0x4000, 0x4000);
    CHECK(l2[0xC9390 + 0xFBFF] == 0xFF, "data window full");
    w32(0x02580024, 8);
    uint8_t rec[0x40] = { 0xB4, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0x28 };
    w32(0x02580040, 0x108D8F90); w32(0x02580044, 0x10018); andw(0x02580048, ~0xFFF8u);
    c6655_soc_upp_rx(S, rec, sizeof rec);
    CHECK(l2[0xD8F90] == 0xB4 && l2[0xD8F94] == 1 && l2[0xD8F9C] == 0x28,
          "record window holds MAIN's record (0x%02x 0x%02x)", l2[0xD8F90], l2[0xD8F94]);
    CHECK(c6655_soc_upp_dropped(S) == 0x400 + 0x28, "overshoot dropped: %zu", c6655_soc_upp_dropped(S));
}

static void test_upp_app_isr(void)
{
    fresh();
    app_irq_init();
    stage1_upp_config();
    /* app 0x0080B560: one line of len bytes, UPID2 &= ~0xFFF8 */
    w32(0x02580040, 0x10880000);
    w32(0x02580044, 0x10000 + 0x800);
    andw(0x02580048, ~0xFFF8u);
    uint8_t b[0x800];
    memset(b, 0xA5, sizeof b);
    c6655_soc_upp_rx(S, b, sizeof b);
    CHECK(l2[0x80000] == 0xA5 && l2[0x807FF] == 0xA5, "app window filled");
    CHECK(irq_pulses[4] == 1, "UPPINT -> CIC0 156 -> ch3 -> event 25 -> INT4: %u", irq_pulses[4]);
    /* ISR 0x008048F4 -> 0x0080B5E4 */
    isr_enter(25, 156);
    CHECK((r32(0x02580024) & 8) == 8, "ISR sees EOWI");
    w32(0x02580024, 8);
    CHECK(r32(0x02580024) == 0, "ISR loop ends");
    cic_clear_status(156);
    isr_leave(25);
    CHECK(irq_pulses[4] == 1, "no spurious re-trigger");
}

static uint16_t main_peer(uint16_t tx, unsigned bits, unsigned csnr)
{
    return tx ^ 0xFFFF;
}

/* app 0x8007D288 / 0x8007D2D0: SPI TX and RX PaRAM sets */
static void app_spi_params(uint32_t txbuf, uint32_t rxbuf, uint32_t n)
{
    uint32_t tx = 0x02744000 + 30 * 32, rx = 0x02744000 + 31 * 32;
    uint32_t txw[8] = { 0x0001E200, txbuf | 0x10000000, 0x00010004, 0x20BF003C, 4, 0x0001FFFF, 4, n };
    uint32_t rxw[8] = { 0x0011F200, 0x20BF0040, 0x00010004, rxbuf | 0x10000000, 0x40000, 0x0001FFFF, 0x40000, n };
    for (int i = 0; i < 8; i++) { w32(tx + 4 * i, txw[i]); w32(rx + 4 * i, rxw[i]); }
}

static void spi_link_setup(void)
{
    spi_peer = main_peer;
    /* EDMA3 global setup 0x8007D944 / 0x8007D1E4 / 0x8007D658 */
    w32(0x02741028, 0xFFFFFFFF); w32(0x0274102C, 0xFFFFFFFF);
    w32(0x02740350, 0xC0000000);                 /* DRAE2: channels 30, 31 */
    w32(0x02740178, 960); w32(0x0274017C, 992);  /* DCHMAP30/31 */
    /* SPI init 0x8007E580 then 0x008021A8 */
    w32(0x20BF0000, 0); w32(0x20BF0000, 1);
    w32(0x20BF0004, 3);
    w32(0x20BF0014, 0xE00); w32(0x20BF004C, 3); w32(0x20BF0048, 0x06070000);
    andw(0x20BF0004, ~(1u << 24));
    orw(0x20BF0014, 1);
    w32(0x20BF0048, 0x06070000);
    w32(0x20BF0050, 0x0E020F10);
    w32(0x20BF000C, 0x100);
    orw(0x20BF0004, 1u << 24);

    uint32_t txbuf = 0x00887000, rxbuf = 0x00888000, n = 64;
    for (uint32_t i = 0; i < n; i++)             /* SPIDAT1 words: CSHOLD | CS0 | data */
        mem_write(0, txbuf + 4 * i, 0x10FE0000 | (0x5500 + i), 4);
    app_spi_params(txbuf, rxbuf, n);
    w32(0x02742460, 1u << 31);                   /* region 2 IESR: TCC 31 */
    w32(0x02742430, 0xC0000000);                 /* region 2 EESR: events 30, 31 */
    w32(0x20BF0008, 0x10300);                    /* DMAREQEN | TXINTENA | RXINTENA */
}

static void test_spi_edma_link(void)
{
    fresh();
    app_irq_init();
    spi_link_setup();
    uint32_t rxbuf = 0x00888000, n = 64;

    c6655_soc_advance(S, 10 * 1000000ull);
    CHECK(spi_words == n, "64 words exchanged: %u", spi_words);
    CHECK((spi_last_csnr & 0xFF) == 0xFE, "CS0 selected by CSNR: 0x%x", spi_last_csnr);
    int ok = 1;
    for (uint32_t i = 0; i < n; i++)
        if ((mem_read(0, rxbuf + 4 * i, 4) & 0xFFFF) != (uint16_t)((0x5500 + i) ^ 0xFFFF))
            ok = 0;
    CHECK(ok, "RX buffer holds MAIN's replies in order (first 0x%08x)", mem_read(0, rxbuf, 4));
    CHECK((r32(0x02742468) >> 31) & 1, "region 2 IPR bit 31 = RX complete");
    CHECK(irq_pulses[9] == 1, "EDMA3_CC_INT2 -> CIC0 26 -> ch5 -> event 27 -> INT9: %u", irq_pulses[9]);
    /* ISR 0x00803DA0 -> 0x00803914: region 2 IPR/ICR */
    isr_enter(27, 26);
    w32(0x02742470, 1u << 31);
    cic_clear_status(26);
    isr_leave(27);
    CHECK(r32(0x02742468) == 0, "ICR clears IPR");
    uint32_t spibuf = r32(0x20BF0044);
    CHECK(spibuf & (1u << 31), "SPIBUF empty after DMA read (SPIEMU 0x%08x)", spibuf);
}

/* The program's real ISR order, as traced in c6xrun: the CIC dispatcher
 * (0x0080C1A8) clears status while EDMA3's IPR is still set, and the body
 * (0x00803BA4) clears IPR afterwards with no second status clear. Every later
 * exchange must still raise INT9. */
static void test_spi_edma_repeated_exchanges(void)
{
    test_spi_edma_link();                       /* one exchange, INT9 x1 */
    uint32_t txbuf = 0x00887000, rxbuf = 0x00888000, n = 64;
    for (unsigned round = 2; round <= 3; round++) {
        isr_enter(27, 26);                      /* status clear with IPR still set */
        w32(0x20BF0008, 0);                     /* 0x00803974: SPIINT0 off */
        w32(0x02742470, 1u << 31);              /* 0x00803BA4: ICR */
        isr_leave(27);
        app_spi_params(txbuf, rxbuf, n);        /* 0x8007DBD0 / 0x8007DC24 re-arm */
        w32(0x02742430, 0xC0000000);
        w32(0x02742460, 1u << 31);
        w32(0x20BF0008, 0x10300);
        c6655_soc_advance(S, 10 * 1000000ull);
        CHECK(irq_pulses[9] == round, "exchange %u raises INT9 again: %u", round, irq_pulses[9]);
    }
}

static void mcbsp_pingpong_setup(void)
{
    fresh();
    app_irq_init();
    /* EDMA3: DRAEH1 = ch 37, 39; DCHMAP37 */
    w32(0x0274034C, 0xA0);
    w32(0x02740194, 1184);
    /* buffers A and B, planar L then R, 16 frames each */
    uint32_t bufA = 0x00885DE0, bufB = 0x00886000;
    for (int i = 0; i < 16; i++) {
        mem_write(0, bufA + 4 * i, 0x1000 + i, 4); mem_write(0, bufA + 64 + 4 * i, 0x2000 + i, 4);
        mem_write(0, bufB + 4 * i, 0x3000 + i, 4); mem_write(0, bufB + 64 + 4 * i, 0x4000 + i, 4);
    }
    /* app 0x8007D578 / 0x8007D5CC: PaRAM 64 -> link 65, 65 -> link 64 */
    uint32_t pa[8] = { 0x00125200, bufA | 0x10000000, 0x00020004, 0x021B4004, 64, 0x00024820, 0x0000FFC4, 16 };
    uint32_t pb[8] = { 0x00125200, bufB | 0x10000000, 0x00020004, 0x021B4004, 64, 0x00024800, 0x0000FFC4, 16 };
    for (int i = 0; i < 8; i++) {
        w32(0x02744000 + 37 * 32 + 4 * i, pa[i]);
        w32(0x02744000 + 64 * 32 + 4 * i, pa[i]);
        w32(0x02744000 + 65 * 32 + 4 * i, pb[i]);
    }
    w32(0x02742264, 1u << 5);                    /* region 1 IESRH: TCC 37 */
    w32(0x02742234, 1u << 5);                    /* region 1 EESRH: event 37 */
    /* McBSP0 init 0x00806CA8 (CLKGDV 7) then 0x00806E48 */
    w32(0x021B4008, 0); orw(0x021B4008, 3u << 24);
    w32(0x021B4010, 0); orw(0x021B4010, 1u << 18); orw(0x021B4010, 1u << 16);
    orw(0x021B4010, 1u << 8); orw(0x021B4010, 0xA0);
    w32(0x021B4014, 0); orw(0x021B4014, 1u << 28); orw(0x021B4014, 0x3Fu << 16);
    orw(0x021B4014, 0x1Fu << 8); orw(0x021B4014, 7);
    w32(0x021B4024, 0); orw(0x021B4024, 1u << 11); orw(0x021B4024, 1u << 9);
    orw(0x021B4024, 8); orw(0x021B4024, 2);
    orw(0x021B4008, 1u << 22);                   /* GRST */
    orw(0x021B4008, 1u << 16);                   /* XRST */

}

static void test_mcbsp_pingpong(void)
{
    mcbsp_pingpong_setup();

    /* idle skipping: word events only feed EDMA3; the first interrupt is the
     * completion of buffer A: XRST's own XEVT moved word 1, so 31 more words */
    uint64_t t_word = c6655_soc_next_event_ns(S), t_irq = c6655_soc_next_irq_ns(S);
    uint64_t now0 = c6655_soc_now_ns(S);
    CHECK(t_irq == t_word && t_irq - now0 > 300000, "word events are batched: next event +%llu ns",
          (unsigned long long)(t_word - now0));
    c6655_soc_advance(S, t_irq - now0 - 1);
    CHECK(irq_pulses[6] == 0 && mcbsp_n[0] <= 30, "nothing interrupts before it (words %u deferred ok, INT6 %u)",
          mcbsp_n[0], irq_pulses[6]);
    c6655_soc_advance(S, 1);
    CHECK(irq_pulses[6] == 1 && mcbsp_n[0] == 31, "INT6 exactly at next_irq_ns (words %u, INT6 %u)",
          mcbsp_n[0], irq_pulses[6]);

    c6655_soc_advance(S, NS_PER_S_TEST - (t_irq - now0));
    r32(0x021B4008);                             /* a register read clocks out deferred words */
    CHECK(mcbsp_n[0] >= 88199 && mcbsp_n[0] <= 88201, "44.1 kHz stereo = 88200 words/s: %u", mcbsp_n[0]);
    CHECK(mcbsp_bits == 32, "32-bit words");
    int order = mcbsp_log[0] == 0x1000 && mcbsp_log[1] == 0x2000 && mcbsp_log[2] == 0x1001
             && mcbsp_log[3] == 0x2001 && mcbsp_log[31] == 0x200F && mcbsp_log[32] == 0x3000
             && mcbsp_log[33] == 0x4000;
    CHECK(order, "L/R interleaved from planar buffer, then linked set B (%x %x %x %x .. %x %x %x)",
          mcbsp_log[0], mcbsp_log[1], mcbsp_log[2], mcbsp_log[3], mcbsp_log[31], mcbsp_log[32], mcbsp_log[33]);
    CHECK(!(r32(0x021B4008) & (1u << 17)), "DMA refills DXR at once, so XRDY rests low");
    CHECK(irq_pulses[6] >= 1, "EDMA3_CC_INT1 -> INT6 on buffer completion: %u", irq_pulses[6]);
    CHECK((r32(0x0274226C) >> 5) & 1, "region 1 IPRH bit 5 = TCC 37");
}

/* Batched word clocking must emit exactly what word-by-word clocking does:
 * the same words in the same order, and the same completion interrupts. */
static void test_mcbsp_batch_is_exact(void)
{
    mcbsp_pingpong_setup();
    for (uint64_t t = 0; t < 3 * NS_PER_S_TEST; t += 11337)     /* ~one word period */
        c6655_soc_advance(S, 11337);
    uint64_t sum = mcbsp_sum[0];
    unsigned words = mcbsp_n[0], irqs = irq_pulses[6];
    uint64_t ipr = r32(0x0274226C);

    mcbsp_pingpong_setup();
    c6655_soc_advance(S, (3 * NS_PER_S_TEST + 11336) / 11337 * 11337);
    CHECK(mcbsp_n[0] == words && mcbsp_sum[0] == sum, "same words: %u vs %u, sum %s",
          mcbsp_n[0], words, mcbsp_sum[0] == sum ? "equal" : "DIFFERS");
    CHECK(irq_pulses[6] == irqs && r32(0x0274226C) == ipr, "same completions: INT6 %u vs %u",
          irq_pulses[6], irqs);
    CHECK(words + 4096 >= 3 * 88199, "3 s of 44.1 kHz stereo, less the deferred batch: %u words", words);
}

/* The DSP stepped on its own thread advances the SoC in chunks of any size.
 * With McBSP ping-pong (region 1, TCC 37) and the SPI link (region 2, TCC 31)
 * both running, every SPI exchange must still raise INT9. */
static void test_link_survives_random_advance(void)
{
    mcbsp_pingpong_setup();
    spi_link_setup();
    w32(0x02742234, 1u << 5);                    /* the link setup's global EECR disabled event 37 */
    uint32_t txbuf = 0x00887000, rxbuf = 0x00888000;
    unsigned seen9 = 0, seen6 = 0, rearms = 0;
    int stage9 = 0, stage6 = 0, wait9 = 0, wait6 = 0;
    uint64_t rng = 0x9E3779B97F4A7C15ull, t = 0, last9_t = 0;
#define RND(n) (rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17, rng % (n))
    while (t < 3 * NS_PER_S_TEST) {
        uint64_t chunk = 1 + RND(300000);        /* 1 ns .. 0.3 ms */
        c6655_soc_advance(S, chunk);
        t += chunk;
        /* A threaded core runs each ISR a few chunks late and may be split
         * across chunks, with the other ISR interleaved. */
        if (!stage6 && irq_pulses[6] != seen6) { seen6 = irq_pulses[6]; stage6 = 1; wait6 = RND(3); }
        if (!stage9 && irq_pulses[9] != seen9) { seen9 = irq_pulses[9]; stage9 = 1; wait9 = RND(3); last9_t = t; }
        if (stage6 && wait6-- <= 0) {
            if (stage6 == 1) { isr_enter(24, 25); stage6 = 2; wait6 = RND(2); }
            else { w32(0x02742274, 1u << 5); isr_leave(24); stage6 = 0; }
        }
        if (stage9 && wait9-- <= 0) {
            switch (stage9) {
            case 1: isr_enter(27, 26); stage9 = 2; wait9 = RND(2); break;
            case 2: w32(0x20BF0008, 0); w32(0x02742470, 1u << 31); isr_leave(27);
                    stage9 = 3; wait9 = RND(3); break;
            default:
                w32(0x02741028, 0xC0000000);     /* 0x8007DE60: EECR, SECR 30/31 */
                w32(0x02741040, 0xC0000000);
                app_spi_params(txbuf, rxbuf, 64);
                w32(0x02742430, 0xC0000000);
                w32(0x02742460, 1u << 31);
                w32(0x20BF0008, 0x10300);
                rearms++;
                stage9 = 0;
            }
        }
    }
#undef RND
    CHECK(irq_pulses[6] > 3000, "audio kept completing (late ISRs merge completions): INT6 %u", irq_pulses[6]);
    CHECK(rearms > 1000 && t - last9_t < 5 * 1000000ull,
          "link never stalls: INT9 %u, %u re-arms, last INT9 %llu ms before the end",
          irq_pulses[9], rearms, (unsigned long long)((t - last9_t) / 1000000));
}

/* The app's planar-to-interleaved copy (0x8007CFA8): QDMA channel 0 on PaRAM 80
 * (AB-sync, no completion interrupt) links to static PaRAM 96 (TCC 8, TCINTEN)
 * and the decoder waits on IPR bit 8. The link must start the second transfer. */
static void test_qdma_link_retriggers(void)
{
    fresh();
    w32(0x02740200, 0x00000A1C);                 /* QCHMAP0: set 80, trigger word 7 */
    w32(0x0274108C, 0xFF);                       /* QEESR */
    for (int i = 0; i < 8; i++) {                /* planar left and right */
        mem_write(0, 0x00884000 + 4 * i, 0x100 + i, 4);
        mem_write(0, 0x00884100 + 4 * i, 0x200 + i, 4);
    }
    /* 8 frames of 4 bytes, source step 4, destination step 8 (interleave) */
    uint32_t p96[8] = { 0x0010820C, 0x10884100, 0x00080004, 0x00885004, 0x00080004, 0x0000FFFF, 0, 1 };
    uint32_t p80[8] = { 0x00008204, 0x10884000, 0x00080004, 0x00885000, 0x00080004, 0x02744C00, 0, 1 };
    for (int i = 0; i < 8; i++) w32(0x02744C00 + 4 * i, p96[i]);
    for (int i = 0; i < 8; i++) w32(0x02744A00 + 4 * i, p80[i]);   /* CCNT last: trigger */
    CHECK(mem_read(0, 0x00885000, 4) == 0x100 && mem_read(0, 0x00885008, 4) == 0x101,
          "first set wrote left to even slots (0x%x 0x%x)", mem_read(0, 0x00885000, 4), mem_read(0, 0x00885008, 4));
    CHECK(mem_read(0, 0x00885004, 4) == 0x200 && mem_read(0, 0x0088500C, 4) == 0x201,
          "the linked set ran too, right to odd slots (0x%x 0x%x)",
          mem_read(0, 0x00885004, 4), mem_read(0, 0x0088500C, 4));
    CHECK((r32(0x02741068) >> 8) & 1, "TCC 8 completion is flagged (IPR 0x%08x)", r32(0x02741068));
}

int main(void)
{
    test_ram_blocks();
    test_gpio_handshake();
    test_timers_and_intc();
    test_upp_stage1_windows();
    test_upp_audio_overshoot();
    test_upp_app_isr();
    test_spi_edma_link();
    test_spi_edma_repeated_exchanges();
    test_mcbsp_pingpong();
    test_mcbsp_batch_is_exact();
    test_link_survives_random_advance();
    test_qdma_link_retriggers();
    c6655_soc_free(S);
    printf("soc_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
