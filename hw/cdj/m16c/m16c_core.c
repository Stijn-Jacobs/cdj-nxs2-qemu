/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * M16C core: registers, memory map, the interrupt sequence and the step
 * loop. Instruction semantics are in m16c_exec.c.
 *
 * Timing is an estimate: one cycle per instruction byte plus one, with the
 * multiply and divide instructions charged extra. The GUI firmware's time
 * bases are its timers, which count these cycles, so only the ratio to the
 * timer clock matters.
 */
#include "m16c_priv.h"
#include <stdlib.h>

#define PAGE_MASK    ((1u << M16C_PAGE_BITS) - 1)
#define VEC_UNDEF    0xFFFDCu
#define VEC_OVERFLOW 0xFFFE0u
#define VEC_BRK      0xFFFE4u

m16c_core *m16c_new(const m16c_bus *bus)
{
    m16c_core *c = calloc(1, sizeof(*c));

    c->bus = *bus;
    c->irq_vec = -1;
    return c;
}

void m16c_free(m16c_core *c)
{
    free(c);
}

void m16c_map_ram(m16c_core *c, uint32_t base, uint32_t size, uint8_t *host)
{
    for (uint32_t off = 0; off < size; off += 1u << M16C_PAGE_BITS) {
        uint32_t page = (base + off) >> M16C_PAGE_BITS;

        c->rd[page] = host + off;
        c->wr[page] = host + off;
    }
}

void m16c_map_rom(m16c_core *c, uint32_t base, uint32_t size,
                  const uint8_t *host)
{
    for (uint32_t off = 0; off < size; off += 1u << M16C_PAGE_BITS) {
        c->rd[(base + off) >> M16C_PAGE_BITS] = host + off;
    }
}

uint32_t m16c_read(m16c_core *c, uint32_t addr, unsigned size)
{
    const uint8_t *p;

    addr &= M16C_ADDR_MASK;
    p = c->rd[addr >> M16C_PAGE_BITS];
    if (!p) {
        return c->bus.read(c->bus.opaque, addr, size);
    }
    p += addr & PAGE_MASK;
    if (size == 1) {
        return p[0];
    }
    if ((addr & PAGE_MASK) == PAGE_MASK) {
        return p[0] | m16c_read(c, addr + 1, 1) << 8;
    }
    return p[0] | p[1] << 8;
}

void m16c_write(m16c_core *c, uint32_t addr, uint32_t val, unsigned size)
{
    uint8_t *p;

    addr &= M16C_ADDR_MASK;
    p = c->wr[addr >> M16C_PAGE_BITS];
    if (!p) {
        c->bus.write(c->bus.opaque, addr, val, size);
        return;
    }
    p += addr & PAGE_MASK;
    p[0] = val;
    if (size == 2) {
        if ((addr & PAGE_MASK) == PAGE_MASK) {
            m16c_write(c, addr + 1, val >> 8, 1);
        } else {
            p[1] = val >> 8;
        }
    }
}

void m16c_push(m16c_core *c, uint32_t val, unsigned size)
{
    uint16_t *sp = m16c_sp(c);

    *sp -= size;
    m16c_write(c, *sp, val, size);
}

uint32_t m16c_pop(m16c_core *c, unsigned size)
{
    uint16_t *sp = m16c_sp(c);
    uint32_t val = m16c_read(c, *sp, size);

    *sp += size;
    return val;
}

/* A vector is a 16-bit offset and the 4 high address bits in the third
 * byte. */
static uint32_t vector_target(m16c_core *c, uint32_t addr)
{
    return m16c_read(c, addr, 2) | (m16c_read(c, addr + 2, 1) & 0xF) << 16;
}

void m16c_reset(m16c_core *c)
{
    memset(c->r, 0, sizeof(c->r));
    memset(c->a, 0, sizeof(c->a));
    memset(c->fb, 0, sizeof(c->fb));
    c->sb = c->usp = c->isp = c->flg = 0;
    c->intb = 0;
    c->irq_vec = -1;
    c->waiting = 0;
    c->pc = vector_target(c, 0xFFFFC);
}

/* The saved state is four bytes: PC[15:0], the FLG low byte, then
 * FLG[15:12] over PC[19:16]. hw_level is the new IPL of a maskable
 * interrupt, or -1. */
void m16c_interrupt(m16c_core *c, uint32_t vector_addr, int hw_level,
                    int keep_u, uint32_t ret_pc)
{
    uint16_t saved = c->flg;

    c->flg &= ~(M16C_I | M16C_D);
    if (!keep_u) {
        c->flg &= ~M16C_U;
    }
    if (hw_level >= 0) {
        c->flg = (c->flg & ~FLG_IPL) | hw_level << 12;
    }
    m16c_push(c, (saved >> 8 & 0xF0) | ret_pc >> 16, 1);
    m16c_push(c, saved & 0xFF, 1);
    m16c_push(c, ret_pc & 0xFFFF, 2);
    c->pc = vector_target(c, vector_addr);
}

void m16c_reit(m16c_core *c)
{
    uint32_t pc = m16c_pop(c, 2);
    uint32_t flgl = m16c_pop(c, 1);
    uint32_t top = m16c_pop(c, 1);

    c->flg = (flgl | (top & 0xF0) << 8) & FLG_VALID;
    c->pc = pc | (top & 0xF) << 16;
}

void m16c_int(m16c_core *c, unsigned n, uint32_t ret_pc)
{
    m16c_interrupt(c, c->intb + n * 4, -1, n >= 32, ret_pc);
}

void m16c_into(m16c_core *c, uint32_t ret_pc)
{
    m16c_interrupt(c, VEC_OVERFLOW, -1, 0, ret_pc);
}

void m16c_brk(m16c_core *c, uint32_t ret_pc)
{
    /* 0xFF in the fixed vector's top byte hands BRK to variable vector 0. */
    if (m16c_read(c, VEC_BRK + 3, 1) == 0xFF) {
        m16c_interrupt(c, c->intb, -1, 0, ret_pc);
    } else {
        m16c_interrupt(c, VEC_BRK, -1, 0, ret_pc);
    }
}

void m16c_und(m16c_core *c, uint32_t ret_pc)
{
    m16c_interrupt(c, VEC_UNDEF, -1, 0, ret_pc);
}

void m16c_set_irq(m16c_core *c, int vec, unsigned level)
{
    c->irq_vec = vec;
    c->irq_level = level;
}

static int irq_pending(const m16c_core *c)
{
    return c->irq_vec >= 0 && (c->flg & M16C_I) &&
           c->irq_level > (unsigned)(c->flg >> 12 & 7);
}

static void trace_insn(m16c_core *c, const m16c_insn *in)
{
    char name[32];
    unsigned n;

    if (c->cycles < c->trace_from || c->cycles >= c->trace_to) {
        return;
    }
    fprintf(c->trace, "%05x ", in->pc);
    for (n = 0; n < in->len; n++) {
        fprintf(c->trace, "%02x", m16c_read(c, in->pc + n, 1));
    }
    m16c_insn_name(in, name, sizeof(name));
    fprintf(c->trace, "%*s %s\n", 14 - 2 * in->len, "", name);
}

m16c_stop m16c_step(m16c_core *c, uint64_t budget, uint64_t *executed)
{
    uint64_t n = 0;
    m16c_stop why = M16C_STOP_BUDGET;

    c->yield = 0;
    while (n < budget && !c->yield &&
           (!c->deadline || c->cycles < c->deadline)) {
        uint8_t bytes[12];
        m16c_insn in;
        unsigned i;

        if (irq_pending(c)) {
            int vec = c->irq_vec;

            c->irq_vec = -1;
            c->waiting = 0;
            m16c_interrupt(c, c->intb + vec * 4, c->irq_level, 0, c->pc);
            if (c->bus.ack) {
                c->bus.ack(c->bus.opaque, vec);
            }
        }
        if (c->waiting) {
            why = M16C_STOP_WAIT;
            break;
        }
        if (c->brk && c->pc == c->brk && !c->brk_skip) {
            c->brk_skip = 1;
            why = M16C_STOP_BREAK;
            break;
        }
        c->brk_skip = 0;
        for (i = 0; i < sizeof(bytes); i++) {
            bytes[i] = m16c_read(c, c->pc + i, 1);
        }
        if (!m16c_decode(bytes, c->pc, &in)) {
            c->trap_pc = c->pc;
            why = M16C_STOP_UNDEF;
            break;
        }
        if (c->trace) {
            trace_insn(c, &in);
        }
        if (m16c_exec(c, &in)) {
            c->trap_pc = in.pc;
            c->pc = in.pc;
            why = M16C_STOP_UNDEF;
            break;
        }
        n++;
    }
    if (executed) {
        *executed = n;
    }
    return why;
}

void m16c_yield(m16c_core *c)
{
    c->yield = 1;
}

void m16c_set_deadline(m16c_core *c, uint64_t cycles)
{
    c->deadline = cycles;
}

uint64_t m16c_cycles(const m16c_core *c)
{
    return c->cycles;
}

void m16c_skip_cycles(m16c_core *c, uint64_t n)
{
    c->cycles += n;
}

uint32_t m16c_get_reg(const m16c_core *c, unsigned reg)
{
    unsigned bank = m16c_bank(c);

    switch (reg) {
    case M16C_R0: case M16C_R1: case M16C_R2: case M16C_R3:
        return c->r[bank][reg - M16C_R0];
    case M16C_A0: case M16C_A1:
        return c->a[bank][reg - M16C_A0];
    case M16C_FB:   return c->fb[bank];
    case M16C_SB:   return c->sb;
    case M16C_USP:  return c->usp;
    case M16C_ISP:  return c->isp;
    case M16C_SP:   return *m16c_sp((m16c_core *)c);
    case M16C_FLG:  return c->flg;
    case M16C_INTB: return c->intb;
    case M16C_PC:   return c->pc;
    }
    return 0;
}

void m16c_set_reg(m16c_core *c, unsigned reg, uint32_t val)
{
    unsigned bank = m16c_bank(c);

    switch (reg) {
    case M16C_R0: case M16C_R1: case M16C_R2: case M16C_R3:
        c->r[bank][reg - M16C_R0] = val;
        break;
    case M16C_A0: case M16C_A1:
        c->a[bank][reg - M16C_A0] = val;
        break;
    case M16C_FB:   c->fb[bank] = val; break;
    case M16C_SB:   c->sb = val; break;
    case M16C_USP:  c->usp = val; break;
    case M16C_ISP:  c->isp = val; break;
    case M16C_SP:   *m16c_sp(c) = val; break;
    case M16C_FLG:  c->flg = val & FLG_VALID; break;
    case M16C_INTB: c->intb = val & M16C_ADDR_MASK; break;
    case M16C_PC:   c->pc = val & M16C_ADDR_MASK; break;
    }
}

uint32_t m16c_trap_pc(const m16c_core *c)
{
    return c->trap_pc;
}

void m16c_set_trace(m16c_core *c, FILE *f, uint64_t from, uint64_t to)
{
    c->trace = f;
    c->trace_from = from;
    c->trace_to = to;
}

void m16c_set_break(m16c_core *c, uint32_t pc)
{
    c->brk = pc;
    c->brk_skip = 0;
}
