/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Structured port of binutils 2.42 opcodes/tic6x-dis.c:print_insn_tic6x.
 * The control flow deliberately follows the original line for line, so a
 * difference against objdump points at a porting slip rather than at an ISA
 * interpretation.
 */
#include "c66x_decode.h"

#include <stdio.h>
#include <string.h>

#include "binutils/tic6x.h"

const tic6x_insn_format tic6x_insn_format_table[tic6x_insn_format_max] = {
#define FMT(name, num_bits, cst_bits, mask, fields) \
    { num_bits, cst_bits, mask, fields },
#include "binutils/tic6x-insn-formats.h"
#undef FMT
};

const tic6x_ctrl tic6x_ctrl_table[tic6x_ctrl_max] = {
#define CTRL(name, isa, rw, crlo, crhi_mask) \
    { STRINGX(name), CONCAT2(TIC6X_INSN_,isa), CONCAT2(tic6x_rw_,rw), crlo, crhi_mask },
#include "binutils/tic6x-control-registers.h"
#undef CTRL
};

const tic6x_opcode tic6x_opcode_table[tic6x_opcode_max] = {
#define INSNU(name, func_unit, format, type, isa, flags, fixed, ops, var) \
    { STRINGX(name), CONCAT2(tic6x_func_unit_,func_unit), \
      CONCAT3(tic6x_insn_format,_,format), CONCAT2(tic6x_pipeline_,type), \
      CONCAT2(TIC6X_INSN_,isa), flags, fixed, ops, var },
#define INSNUE(name, e, func_unit, format, type, isa, flags, fixed, ops, var) \
    { STRINGX(name), CONCAT2(tic6x_func_unit_,func_unit), \
      CONCAT3(tic6x_insn_format,_,format), CONCAT2(tic6x_pipeline_,type), \
      CONCAT2(TIC6X_INSN_,isa), flags, fixed, ops, var },
#define INSN(name, func_unit, format, type, isa, flags, fixed, ops, var) \
    { STRINGX(name), CONCAT2(tic6x_func_unit_,func_unit), \
      CONCAT4(tic6x_insn_format_,func_unit,_,format), CONCAT2(tic6x_pipeline_,type), \
      CONCAT2(TIC6X_INSN_,isa), flags, fixed, ops, var },
#define INSNE(name, e, func_unit, format, type, isa, flags, fixed, ops, var) \
    { STRINGX(name), CONCAT2(tic6x_func_unit_,func_unit), \
      CONCAT4(tic6x_insn_format_,func_unit,_,format), CONCAT2(tic6x_pipeline_,type), \
      CONCAT2(TIC6X_INSN_,isa), flags, fixed, ops, var },
#include "binutils/tic6x-opcode-table.h"
#undef INSN
#undef INSNE
#undef INSNU
#undef INSNUE
};

const char *c66x_ext_name(int opc);

const char *c66x_opcode_name(int opc)
{
    if (opc >= 0 && opc < tic6x_opcode_max)
        return tic6x_opcode_table[opc].name;
    const char *e = c66x_ext_name(opc);
    return e ? e : "?";
}

unsigned c66x_opcode_count(void) { return tic6x_opcode_max; }
unsigned c66x_opcode_flags(int opc) { return opc < tic6x_opcode_max ? tic6x_opcode_table[opc].flags : 0; }
unsigned c66x_opcode_pipeline(int opc) { return opc < tic6x_opcode_max ? tic6x_opcode_table[opc].type : 0; }
unsigned c66x_opcode_unit(int opc) { return opc < tic6x_opcode_max ? tic6x_opcode_table[opc].func_unit : 0; }

const char *c66x_ctrl_name(int ctrl)
{
    return (ctrl >= 0 && ctrl < tic6x_ctrl_max) ? tic6x_ctrl_table[ctrl].name : "?";
}

unsigned c66x_ctrl_crlo(int ctrl) { return tic6x_ctrl_table[ctrl].crlo; }

static const tic6x_insn_field *field_from_fmt(const tic6x_insn_format *fmt,
                                              tic6x_insn_field_id field)
{
    for (unsigned f = 0; f < fmt->num_fields; f++)
        if (fmt->fields[f].field_id == field)
            return &fmt->fields[f];
    return NULL;
}

static unsigned field_width(const tic6x_insn_field *field)
{
    unsigned width = 0;
    if (!field->num_bitfields)
        return field->bitfields[0].width;
    for (unsigned i = 0; i < field->num_bitfields; i++)
        width += field->bitfields[i].width;
    return width;
}

static unsigned field_bits(unsigned opcode, const tic6x_insn_field *field)
{
    unsigned val = 0;
    if (!field->num_bitfields)
        return (opcode >> field->bitfields[0].low_pos)
               & ((1u << field->bitfields[0].width) - 1);
    for (unsigned i = 0; i < field->num_bitfields; i++)
        val |= ((opcode >> field->bitfields[i].low_pos)
                & ((1u << field->bitfields[i].width) - 1))
               << field->bitfields[i].pos;
    return val;
}

static unsigned le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static bool check_header(const uint8_t *fp, tic6x_fetch_packet_header *h)
{
    h->header = le32(fp + 28);
    if ((h->header & 0xf0000000) != 0xe0000000) {
        memset(h, 0, sizeof(*h));
        return false;
    }
    for (int i = 0; i < 7; i++)
        h->word_compact[i] = (h->header & (1u << (21 + i))) != 0;
    h->prot = (h->header & (1u << 20)) != 0;
    h->rs = (h->header & (1u << 19)) != 0;
    h->dsz = (h->header >> 16) & 0x7;
    h->br = (h->header & (1u << 15)) != 0;
    h->sat = (h->header & (1u << 14)) != 0;
    for (int i = 0; i < 14; i++)
        h->p_bits[i] = (h->header & (1u << i)) != 0;
    return true;
}

static unsigned extract16(const uint8_t *p, const tic6x_fetch_packet_header *h)
{
    unsigned op16 = p[0] | (p[1] << 8);
    op16 |= (h->sat << TIC6X_COMPACT_SAT_POS);
    op16 |= (h->br << TIC6X_COMPACT_BR_POS);
    op16 |= (h->dsz << TIC6X_COMPACT_DSZ_POS);
    return op16;
}

bool c66x_prev_parallel(c66x_fp_reader rd, void *opaque, uint32_t addr)
{
    uint8_t fp[32];
    unsigned fp_offset = addr & 0x1f;
    uint32_t fp_addr = addr - fp_offset;
    tic6x_fetch_packet_header header;

    if (rd(opaque, fp_addr, fp))
        return false;
    bool hb = check_header(fp, &header);
    unsigned num_bits = hb && header.word_compact[fp_offset >> 2] ? 16 : 32;

    if (num_bits == 16 && (fp_offset & 0x2) == 2)
        return header.p_bits[(fp_offset >> 2) << 1];
    if (fp_offset >= 4) {
        if (hb && header.word_compact[(fp_offset >> 2) - 1])
            return header.p_bits[(fp_offset >> 1) - 1];
        return (le32(fp + (fp_offset & 0x1c) - 4) & 1) != 0;
    }
    uint8_t prev[32];
    if (rd(opaque, fp_addr - 32, prev))
        return false;
    tic6x_fetch_packet_header ph;
    if (check_header(prev, &ph)) {
        if (ph.word_compact[6])
            return ph.p_bits[13];
        return (le32(prev + 24) & 1) != 0;
    }
    return (le32(prev + 28) & 1) != 0;
}

/* ------------------------------------------------------------------------ */
/* C66x instructions binutils 2.42 lacks (generated from SPRUGH7 by gen_ext.py) */

enum { XF_L12, XF_L12U, XF_S12, XF_S12U, XF_S2, XF_S2U, XF_D2, XF_M32U, XF_MCR,
       XF_MCRU, XF_L1, XF_S1, XF_M1 };
enum { XFLD_SRC1, XFLD_SRC2, XFLD_DST };
enum { XT_REG, XT_XREG, XT_PAIR, XT_XPAIR, XT_QUAD, XT_SCST5, XT_UCST5, XT_LONG, XT_XLONG };

typedef struct c66x_ext_op {
    const char *name;
    char unit;
    uint8_t form;
    uint8_t op;
    uint8_t slots;      /* delay slots from the instruction's page: dst lands at slots + 1 */
    uint8_t nops;
    struct { uint8_t fld, type; } o[3];
} c66x_ext_op;

#include "c66x_ext_table.h"

#define EXT_BASE 2000     /* insn->opc for extension entries = EXT_BASE + index */

static const struct { uint32_t mask, value; uint8_t op_pos, op_w, fixed; } xforms[] = {
    [XF_L12]  = { 0x1c,  0x18,  5, 7, 0 },
    [XF_L12U] = { 0x1c,  0x18,  5, 7, 1 },
    [XF_S12]  = { 0x3c,  0x20,  6, 6, 0 },
    [XF_S12U] = { 0x3c,  0x20,  6, 6, 1 },
    [XF_S2]   = { 0xc3c, 0xc30, 6, 4, 0 },
    [XF_S2U]  = { 0xc3c, 0xc30, 6, 4, 1 },
    [XF_D2]   = { 0xc3c, 0x830, 6, 4, 0 },
    [XF_M32U] = { 0x7c,  0x00,  7, 5, 1 },
    [XF_MCR]  = { 0x83c, 0x30,  6, 5, 0 },
    [XF_MCRU] = { 0x83c, 0x30,  6, 5, 1 },
    [XF_L1]   = { 0xffc, 0x358, 13, 5, 0 },
    [XF_S1]   = { 0xffc, 0xf20, 13, 5, 0 },
    [XF_M1]   = { 0xffc, 0x0f0, 13, 5, 0 },
};

const char *c66x_ext_name(int opc)
{
    int i = opc - EXT_BASE;
    if (i < 0 || i >= (int)(sizeof c66x_ext_table / sizeof c66x_ext_table[0]))
        return NULL;
    return c66x_ext_table[i].name;
}

static int decode_ext(uint32_t w, c66x_insn *d, char *text, unsigned textlen)
{
    static const int8_t creg_reg[8] = { -1, C66X_B(0), C66X_B(1), C66X_B(2),
                                        C66X_A(1), C66X_A(2), C66X_A(0), -1 };
    static const char *const cregn[8] = { "", "b0", "b1", "b2", "a1", "a2", "a0", "" };
    unsigned creg = w >> 29, z = (w >> 28) & 1;
    unsigned n = sizeof c66x_ext_table / sizeof c66x_ext_table[0];
    for (unsigned i = 0; i < n; i++) {
        const c66x_ext_op *e = &c66x_ext_table[i];
        const typeof(xforms[0]) *f = &xforms[e->form];
        if ((w & f->mask) != f->value)
            continue;
        if (((w >> f->op_pos) & ((1u << f->op_w) - 1)) != e->op)
            continue;
        if (f->fixed ? (w >> 28) != 1 : (creg == 7 || (creg == 0 && z)))
            continue;
        unsigned side = ((w >> 1) & 1) ? 2 : 1;
        unsigned x = (w >> 12) & 1;
        unsigned fld[3] = { (w >> 13) & 31, (w >> 18) & 31, (w >> 23) & 31 };
        d->opc = EXT_BASE + i;
        d->side = side;
        d->unit = (e->unit == 'L' ? 0 : e->unit == 'S' ? 2 : e->unit == 'D' ? 4 : 6) + side - 1;
        d->cond_reg = f->fixed ? -1 : creg_reg[creg];
        d->cond_z = z;
        d->nops = e->nops;
        if (text) {
            if (!f->fixed && creg)
                snprintf(text, textlen, "[%s%s] %s .%c%u%s", z ? "!" : "", cregn[creg],
                         e->name, e->unit, side, x ? "X" : "");
            else
                snprintf(text, textlen, "%s .%c%u%s", e->name, e->unit, side, x ? "X" : "");
        }
        for (unsigned k = 0; k < e->nops; k++) {
            c66x_operand *o = &d->op[k];
            unsigned v = fld[e->o[k].fld];
            int cross = (e->o[k].type == XT_XREG || e->o[k].type == XT_XPAIR
                         || e->o[k].type == XT_XLONG) && x;
            unsigned base = ((side == 2) ^ cross) ? 32 : 0;
            int is_dst = e->o[k].fld == XFLD_DST;
            o->rw = is_dst ? tic6x_rw_write : tic6x_rw_read;
            o->low_first = o->low_last = o->high_first = o->high_last = is_dst ? e->slots + 1 : 1;
            o->xpath = cross;
            switch (e->o[k].type) {
            case XT_REG: case XT_XREG:
                o->kind = C66X_OPK_REG; o->size = 4; o->reg = base + v;
                break;
            case XT_PAIR: case XT_XPAIR:
                o->kind = C66X_OPK_PAIR; o->size = 8; o->reg = base + (v & ~1u); o->reg_hi = o->reg + 1;
                break;
            case XT_LONG: case XT_XLONG:
                /* 40-bit long: the low word and the pair's low 8 bits above it */
                o->kind = C66X_OPK_PAIR; o->size = 5; o->reg = base + (v & ~1u); o->reg_hi = o->reg + 1;
                break;
            case XT_QUAD:
                /* four registers, lowest first: reg..reg+3 */
                o->kind = C66X_OPK_PAIR; o->size = 16; o->reg = base + (v & ~3u); o->reg_hi = o->reg + 1;
                break;
            case XT_SCST5:
                o->kind = C66X_OPK_CONST; o->rw = tic6x_rw_none; o->val = (int32_t)(v << 27) >> 27;
                break;
            case XT_UCST5:
                o->kind = C66X_OPK_CONST; o->rw = tic6x_rw_none; o->val = v;
                break;
            }
            if (text) {
                size_t L = strlen(text);
                const char *sep = k ? "," : " ";
                if (o->kind == C66X_OPK_CONST)
                    snprintf(text + L, textlen - L, "%s%d", sep, o->val);
                else if (o->size == 16)
                    snprintf(text + L, textlen - L, "%s%c%u:%c%u", sep, base ? 'b' : 'a', (o->reg & 31) + 3,
                             base ? 'b' : 'a', o->reg & 31);
                else if (o->kind == C66X_OPK_PAIR)
                    snprintf(text + L, textlen - L, "%s%c%u:%c%u", sep, base ? 'b' : 'a', (o->reg & 31) + 1,
                             base ? 'b' : 'a', o->reg & 31);
                else
                    snprintf(text + L, textlen - L, "%s%c%u", sep, base ? 'b' : 'a', o->reg & 31);
            }
        }
        return 1;
    }
    return 0;
}

#define TXT(i, ...) snprintf(operands[i], sizeof operands[i], __VA_ARGS__)

int c66x_decode(c66x_fp_reader rd, void *opaque, uint32_t addr,
                c66x_insn *out, char *text, unsigned textlen)
{
    uint8_t fp[32];
    unsigned fp_offset = addr & 0x1f;
    uint32_t fp_addr = addr - fp_offset;
    tic6x_fetch_packet_header header;
    unsigned num_bits;
    unsigned opcode;
    bool bad_offset = false;

    memset(out, 0, sizeof(*out));
    out->addr = addr;
    out->opc = -1;
    out->cond_reg = -1;
    out->unit = -1;
    if (text && textlen)
        text[0] = 0;

    if (rd(opaque, fp_addr, fp))
        return -1;

    bool hb = check_header(fp, &header);
    if (hb) {
        if (fp_offset & 0x1)
            bad_offset = true;
        if ((fp_offset & 0x3) && (fp_offset >= 28 || !header.word_compact[fp_offset >> 2]))
            bad_offset = true;
        if (fp_offset == 28) {
            out->opc = -2;
            out->size = 4;
            out->opcode = header.header;
            if (text)
                snprintf(text, textlen, "<fetch packet header 0x%.8x>", header.header);
            return 4;
        }
        num_bits = header.word_compact[fp_offset >> 2] ? 16 : 32;
    } else {
        num_bits = 32;
        if (fp_offset & 0x3)
            bad_offset = true;
    }
    out->compact = hb;
    out->prot = hb && header.prot;

    if (bad_offset) {
        out->size = 1;
        if (text)
            snprintf(text, textlen, ".byte 0x%.2x", fp[fp_offset]);
        return 1;
    }

    if (num_bits == 16) {
        opcode = extract16(fp + fp_offset, &header);
        out->p = header.p_bits[fp_offset >> 1];
    } else {
        opcode = le32(fp + fp_offset);
        out->p = opcode & 1;
    }
    out->opcode = opcode;
    out->size = num_bits / 8;

    for (int opcode_id = 0; opcode_id < tic6x_opcode_max; opcode_id++) {
        const tic6x_opcode *const opc = &tic6x_opcode_table[opcode_id];
        const tic6x_insn_format *const fmt = &tic6x_insn_format_table[opc->format];
        const tic6x_insn_field *creg_field;
        const char *cond = "";
        const char *func_unit;
        char func_unit_buf[8];
        unsigned func_unit_side = 0;
        unsigned func_unit_data_side = 0;
        unsigned func_unit_cross = 0;
        unsigned t_val = 0;
        char operands[TIC6X_MAX_OPERANDS][24] = { { 0 } };
        uint32_t operands_addresses[TIC6X_MAX_OPERANDS] = { 0 };
        bool operands_text[TIC6X_MAX_OPERANDS] = { false };
        bool operands_pcrel[TIC6X_MAX_OPERANDS] = { false };
        unsigned num_operands;
        bool fixed_ok;
        bool operands_ok;
        bool have_t = false;
        c66x_insn d;
        int cond_reg = -1, cond_z = 0;

        if (opc->flags & TIC6X_FLAG_MACRO)
            continue;
        if (fmt->num_bits != num_bits)
            continue;
        if ((opcode & fmt->mask) != fmt->cst_bits)
            continue;

        creg_field = field_from_fmt(fmt, tic6x_field_creg);
        if (creg_field) {
            static const char *const conds[8][2] = {
                { "", NULL },
                { "[b0] ", "[!b0] " },
                { "[b1] ", "[!b1] " },
                { "[b2] ", "[!b2] " },
                { "[a1] ", "[!a1] " },
                { "[a2] ", "[!a2] " },
                { "[a0] ", "[!a0] " },
                { NULL, NULL }
            };
            static const int8_t creg_reg[8] = { -1, C66X_B(0), C66X_B(1), C66X_B(2),
                                                C66X_A(1), C66X_A(2), C66X_A(0), -1 };
            const tic6x_insn_field *z_field = field_from_fmt(fmt, tic6x_field_z);
            unsigned creg_value = field_bits(opcode, creg_field);
            unsigned z_value = field_bits(opcode, z_field);
            cond = conds[creg_value][z_value];
            if (cond == NULL)
                continue;
            cond_reg = creg_reg[creg_value];
            cond_z = z_value;
        }

        if (opc->flags & TIC6X_FLAG_INSN16_SPRED) {
            const tic6x_insn_field *cc_field = field_from_fmt(fmt, tic6x_field_cc);
            unsigned s_value, z_value;
            static const char *const conds[2][2] = {
                { "[a0] ", "[!a0] " },
                { "[b0] ", "[!b0] " }
            };
            if (cc_field) {
                unsigned cc_value = field_bits(opcode, cc_field);
                s_value = (cc_value & 0x2) >> 1;
                z_value = (cc_value & 0x1);
            } else {
                s_value = field_bits(opcode, field_from_fmt(fmt, tic6x_field_s));
                z_value = field_bits(opcode, field_from_fmt(fmt, tic6x_field_z));
            }
            cond = conds[s_value][z_value];
            cond_reg = s_value ? C66X_B(0) : C66X_A(0);
            cond_z = z_value;
        }

        fixed_ok = true;
        for (unsigned fix = 0; fix < opc->num_fixed_fields; fix++) {
            const tic6x_insn_field *const field =
                field_from_fmt(fmt, opc->fixed_fields[fix].field_id);
            unsigned fb = field_bits(opcode, field);
            if (fb < opc->fixed_fields[fix].min_val || fb > opc->fixed_fields[fix].max_val) {
                fixed_ok = false;
                break;
            }
        }
        if (!fixed_ok)
            continue;

        d = *out;
        d.cond_reg = cond_reg;
        d.cond_z = cond_z;

        if (opc->func_unit == tic6x_func_unit_nfu)
            func_unit = "";
        else {
            char func_unit_char;
            const char *data_str;
            bool have_areg = false;
            bool have_cross = false;

            func_unit_side = (opc->flags & TIC6X_FLAG_SIDE_B_ONLY) ? 2 : 0;
            func_unit_cross = 0;
            func_unit_data_side = (opc->flags & TIC6X_FLAG_SIDE_T2_ONLY) ? 2 : 0;

            for (unsigned fld_num = 0; fld_num < opc->num_variable_fields; fld_num++) {
                const tic6x_coding_field *const enc = &opc->variable_fields[fld_num];
                const tic6x_insn_field *field = field_from_fmt(fmt, enc->field_id);
                unsigned fld_val = field_bits(opcode, field);

                switch (enc->coding_method) {
                case tic6x_coding_fu:
                    func_unit_side = (fld_val ? 2 : 1);
                    break;
                case tic6x_coding_data_fu:
                    func_unit_data_side = (fld_val ? 2 : 1);
                    break;
                case tic6x_coding_xpath:
                    have_cross = true;
                    func_unit_cross = fld_val;
                    break;
                case tic6x_coding_rside:
                    have_t = true;
                    t_val = fld_val;
                    func_unit_data_side = (t_val ? 2 : 1);
                    break;
                case tic6x_coding_areg:
                    have_areg = true;
                    break;
                default:
                    break;
                }
            }

            if (have_areg && !func_unit_data_side)
                func_unit_cross = func_unit_side == 1;

            switch (opc->func_unit) {
            case tic6x_func_unit_d: func_unit_char = 'D'; d.unit = 4; break;
            case tic6x_func_unit_l: func_unit_char = 'L'; d.unit = 0; break;
            case tic6x_func_unit_m: func_unit_char = 'M'; d.unit = 6; break;
            case tic6x_func_unit_s: func_unit_char = 'S'; d.unit = 2; break;
            default: return -1;
            }
            d.unit += func_unit_side - 1;
            d.side = func_unit_side;

            switch (func_unit_data_side) {
            case 1: data_str = "T1"; break;
            case 2: data_str = "T2"; break;
            default: data_str = ""; break;
            }

            if (opc->flags & TIC6X_FLAG_INSN16_BSIDE && func_unit_side == 1)
                func_unit_cross = 1;

            snprintf(func_unit_buf, sizeof func_unit_buf, " .%c%u%s%s",
                     func_unit_char, func_unit_side,
                     (func_unit_cross ? "X" : ""), data_str);
            func_unit = func_unit_buf;
        }

        operands_ok = true;
        num_operands = opc->num_operands;
        for (unsigned op_num = 0; op_num < num_operands; op_num++) {
            unsigned mem_base_reg = 0;
            bool mem_base_reg_known = false;
            bool mem_base_reg_known_long = false;
            unsigned mem_offset = 0;
            bool mem_offset_known = false;
            bool mem_offset_known_long = false;
            unsigned mem_mode = 0;
            bool mem_mode_known = false;
            unsigned mem_scaled = 0;
            bool mem_scaled_known = false;
            unsigned crlo = 0;
            bool crlo_known = false;
            unsigned crhi = 0;
            bool crhi_known = false;
            bool spmask_skip_operand = false;
            unsigned fcyc_bits = 0;
            bool prev_sploop_found = false;
            const tic6x_operand_info *oi = &opc->operand_info[op_num];
            c66x_operand *o = &d.op[op_num];
            char side_ch = func_unit_side == 2 ? 'b' : 'a';
            unsigned side_base = func_unit_side == 2 ? 32 : 0;

            o->size = oi->size;
            o->rw = oi->rw;
            o->low_first = oi->low_first;
            o->low_last = oi->low_last;
            o->high_first = oi->high_first;
            o->high_last = oi->high_last;

            switch (oi->form) {
            case tic6x_operand_b15reg:
                operands_text[op_num] = true;
                TXT(op_num, "b15");
                o->kind = C66X_OPK_REG; o->reg = C66X_B(15);
                continue;
            case tic6x_operand_zreg:
                operands_text[op_num] = true;
                TXT(op_num, "%c0", side_ch);
                o->kind = C66X_OPK_REG; o->reg = side_base;
                continue;
            case tic6x_operand_retreg:
                operands_text[op_num] = true;
                TXT(op_num, "%c3", side_ch);
                o->kind = C66X_OPK_REG; o->reg = side_base + 3;
                continue;
            case tic6x_operand_irp:
                operands_text[op_num] = true;
                TXT(op_num, "irp");
                o->kind = C66X_OPK_IRP;
                continue;
            case tic6x_operand_nrp:
                operands_text[op_num] = true;
                TXT(op_num, "nrp");
                o->kind = C66X_OPK_NRP;
                continue;
            case tic6x_operand_ilc:
                operands_text[op_num] = true;
                TXT(op_num, "ilc");
                o->kind = C66X_OPK_ILC;
                continue;
            case tic6x_operand_hw_const_minus_1:
                operands_text[op_num] = true; TXT(op_num, "-1");
                o->kind = C66X_OPK_CONST; o->val = -1;
                continue;
            case tic6x_operand_hw_const_0:
                operands_text[op_num] = true; TXT(op_num, "0");
                o->kind = C66X_OPK_CONST; o->val = 0;
                continue;
            case tic6x_operand_hw_const_1:
                operands_text[op_num] = true; TXT(op_num, "1");
                o->kind = C66X_OPK_CONST; o->val = 1;
                continue;
            case tic6x_operand_hw_const_5:
                operands_text[op_num] = true; TXT(op_num, "5");
                o->kind = C66X_OPK_CONST; o->val = 5;
                continue;
            case tic6x_operand_hw_const_16:
                operands_text[op_num] = true; TXT(op_num, "16");
                o->kind = C66X_OPK_CONST; o->val = 16;
                continue;
            case tic6x_operand_hw_const_24:
                operands_text[op_num] = true; TXT(op_num, "24");
                o->kind = C66X_OPK_CONST; o->val = 24;
                continue;
            case tic6x_operand_hw_const_31:
                operands_text[op_num] = true; TXT(op_num, "31");
                o->kind = C66X_OPK_CONST; o->val = 31;
                continue;
            default:
                break;
            }

            for (unsigned fld_num = 0; fld_num < opc->num_variable_fields; fld_num++) {
                const tic6x_coding_field *const enc = &opc->variable_fields[fld_num];
                const tic6x_insn_field *field;
                unsigned fld_val;
                unsigned reg_base = 0;
                int signed_fld_val;
                char reg_side = '?';

                if (enc->operand_num != op_num)
                    continue;
                field = field_from_fmt(fmt, enc->field_id);
                fld_val = field_bits(opcode, field);
                switch (enc->coding_method) {
                case tic6x_coding_cst_s3i:
                    if (fld_val == 0x00) fld_val = 0x10;
                    if (fld_val == 0x07) fld_val = 0x08;
                    /* fall through */
                case tic6x_coding_ucst:
                case tic6x_coding_ulcst_dpr_byte:
                case tic6x_coding_ulcst_dpr_half:
                case tic6x_coding_ulcst_dpr_word:
                case tic6x_coding_lcst_low16:
                    switch (oi->form) {
                    case tic6x_operand_asm_const:
                    case tic6x_operand_link_const:
                        operands_text[op_num] = true;
                        TXT(op_num, "%u", fld_val);
                        o->kind = C66X_OPK_CONST; o->val = fld_val;
                        break;
                    case tic6x_operand_mem_long:
                        mem_offset = fld_val;
                        mem_offset_known_long = true;
                        break;
                    default:
                        return -1;
                    }
                    break;

                case tic6x_coding_lcst_high16:
                    operands_text[op_num] = true;
                    TXT(op_num, "%u", fld_val << 16);
                    o->kind = C66X_OPK_CONST; o->val = fld_val << 16;
                    break;

                case tic6x_coding_scst_l3i:
                    operands_text[op_num] = true;
                    if (fld_val == 0)
                        signed_fld_val = 8;
                    else {
                        signed_fld_val = (int)fld_val;
                        signed_fld_val ^= (1 << (field_width(field) - 1));
                        signed_fld_val -= (1 << (field_width(field) - 1));
                    }
                    TXT(op_num, "%d", signed_fld_val);
                    o->kind = C66X_OPK_CONST; o->val = signed_fld_val;
                    break;

                case tic6x_coding_scst:
                    operands_text[op_num] = true;
                    signed_fld_val = (int)fld_val;
                    signed_fld_val ^= (1 << (field_width(field) - 1));
                    signed_fld_val -= (1 << (field_width(field) - 1));
                    TXT(op_num, "%d", signed_fld_val);
                    o->kind = C66X_OPK_CONST; o->val = signed_fld_val;
                    break;

                case tic6x_coding_ucst_minus_one:
                    operands_text[op_num] = true;
                    TXT(op_num, "%u", fld_val + 1);
                    o->kind = C66X_OPK_CONST; o->val = fld_val + 1;
                    break;

                case tic6x_coding_pcrel:
                case tic6x_coding_pcrel_half:
                    signed_fld_val = (int)fld_val;
                    signed_fld_val ^= (1 << (field_width(field) - 1));
                    signed_fld_val -= (1 << (field_width(field) - 1));
                    if (hb && enc->coding_method == tic6x_coding_pcrel_half)
                        signed_fld_val *= 2;
                    else
                        signed_fld_val *= 4;
                    operands_pcrel[op_num] = true;
                    operands_addresses[op_num] = fp_addr + signed_fld_val;
                    o->kind = C66X_OPK_ADDR; o->val = fp_addr + signed_fld_val;
                    break;

                case tic6x_coding_regpair_msb:
                    if (oi->form != tic6x_operand_regpair)
                        return -1;
                    operands_text[op_num] = true;
                    TXT(op_num, "%c%u:%c%u", side_ch, (fld_val | 0x1), side_ch, (fld_val | 0x1) - 1);
                    o->kind = C66X_OPK_PAIR;
                    o->reg_hi = side_base + (fld_val | 1);
                    o->reg = side_base + (fld_val | 1) - 1;
                    break;

                case tic6x_coding_pcrel_half_unsigned:
                    operands_pcrel[op_num] = true;
                    operands_addresses[op_num] = fp_addr + 2 * fld_val;
                    o->kind = C66X_OPK_ADDR; o->val = fp_addr + 2 * fld_val;
                    break;

                case tic6x_coding_reg_shift:
                    fld_val <<= 1;
                    /* fall through */
                case tic6x_coding_reg:
                    /* SPRU732J 3.9.2.2: RS selects A16-A23/B16-B23 for the 3-bit
                     * register fields only. binutils also offsets the 5-bit
                     * register of LSDmvto/LSDmvfr (G-1, G-2), which decoded
                     * 0x80072576 as mv b22,a19 where the firmware means b6. */
                    if (num_bits == 16 && header.rs && !(opc->flags & TIC6X_FLAG_INSN16_NORS)
                        && field_width(field) <= 3)
                        reg_base = 16;
                    switch (oi->form) {
                    case tic6x_operand_treg:
                        operands_text[op_num] = true;
                        reg_side = t_val ? 'b' : 'a';
                        TXT(op_num, "%c%u", reg_side, reg_base + fld_val);
                        o->kind = C66X_OPK_REG;
                        o->reg = (t_val ? 32 : 0) + reg_base + fld_val;
                        break;
                    case tic6x_operand_reg:
                        operands_text[op_num] = true;
                        reg_side = side_ch;
                        TXT(op_num, "%c%u", reg_side, reg_base + fld_val);
                        o->kind = C66X_OPK_REG;
                        o->reg = side_base + reg_base + fld_val;
                        break;
                    case tic6x_operand_reg_nors:
                        operands_text[op_num] = true;
                        TXT(op_num, "%c%u", side_ch, fld_val);
                        o->kind = C66X_OPK_REG;
                        o->reg = side_base + fld_val;
                        break;
                    case tic6x_operand_reg_bside:
                        operands_text[op_num] = true;
                        TXT(op_num, "b%u", reg_base + fld_val);
                        o->kind = C66X_OPK_REG;
                        o->reg = 32 + reg_base + fld_val;
                        break;
                    case tic6x_operand_reg_bside_nors:
                        operands_text[op_num] = true;
                        TXT(op_num, "b%u", fld_val);
                        o->kind = C66X_OPK_REG;
                        o->reg = 32 + fld_val;
                        break;
                    case tic6x_operand_xreg: {
                        bool bs = (func_unit_side == 2) ^ func_unit_cross;
                        operands_text[op_num] = true;
                        TXT(op_num, "%c%u", bs ? 'b' : 'a', reg_base + fld_val);
                        o->kind = C66X_OPK_REG;
                        o->reg = (bs ? 32 : 0) + reg_base + fld_val;
                        o->xpath = func_unit_cross != 0;
                        break;
                    }
                    case tic6x_operand_dreg:
                        operands_text[op_num] = true;
                        reg_side = (func_unit_data_side == 2) ? 'b' : 'a';
                        TXT(op_num, "%c%u", reg_side, reg_base + fld_val);
                        o->kind = C66X_OPK_REG;
                        o->reg = (func_unit_data_side == 2 ? 32 : 0) + reg_base + fld_val;
                        break;
                    case tic6x_operand_regpair:
                    case tic6x_operand_xregpair:
                    case tic6x_operand_tregpair:
                    case tic6x_operand_dregpair: {
                        bool bs;
                        if (oi->form == tic6x_operand_regpair)
                            bs = func_unit_side == 2;
                        else if (oi->form == tic6x_operand_xregpair)
                            bs = (func_unit_side == 2) ^ func_unit_cross;
                        else if (oi->form == tic6x_operand_tregpair)
                            bs = t_val != 0;
                        else
                            bs = func_unit_data_side == 2;
                        operands_text[op_num] = true;
                        if (fld_val & 1)
                            operands_ok = false;
                        reg_side = bs ? 'b' : 'a';
                        TXT(op_num, "%c%u:%c%u", reg_side, reg_base + fld_val + 1,
                            reg_side, reg_base + fld_val);
                        o->kind = C66X_OPK_PAIR;
                        o->reg = (bs ? 32 : 0) + reg_base + fld_val;
                        o->reg_hi = o->reg + 1;
                        o->xpath = oi->form == tic6x_operand_xregpair && func_unit_cross;
                        break;
                    }
                    case tic6x_operand_mem_deref:
                        operands_text[op_num] = true;
                        TXT(op_num, "*%c%u", side_ch, reg_base + fld_val);
                        o->kind = C66X_OPK_MEM;
                        o->reg = side_base + reg_base + fld_val;
                        o->mem_mode = 1;
                        o->mem_scale = 0;
                        o->val = 0;
                        break;
                    case tic6x_operand_mem_short:
                    case tic6x_operand_mem_ndw:
                        mem_base_reg = fld_val;
                        mem_base_reg_known = true;
                        break;
                    default:
                        return -1;
                    }
                    break;

                case tic6x_coding_reg_ptr:
                    switch (oi->form) {
                    case tic6x_operand_mem_short:
                    case tic6x_operand_mem_ndw:
                        if (fld_val > 0x3u)
                            return -1;
                        mem_base_reg = 0x4 | fld_val;
                        mem_base_reg_known = true;
                        break;
                    default:
                        return -1;
                    }
                    break;

                case tic6x_coding_areg:
                    switch (oi->form) {
                    case tic6x_operand_areg:
                        operands_text[op_num] = true;
                        TXT(op_num, "b%u", fld_val ? 15u : 14u);
                        o->kind = C66X_OPK_REG;
                        o->reg = C66X_B(fld_val ? 15 : 14);
                        break;
                    case tic6x_operand_mem_long:
                        mem_base_reg = fld_val ? 15u : 14u;
                        mem_base_reg_known_long = true;
                        break;
                    default:
                        return -1;
                    }
                    break;

                case tic6x_coding_mem_offset_minus_one_noscale:
                case tic6x_coding_mem_offset_minus_one:
                    fld_val += 1;
                    /* fall through */
                case tic6x_coding_mem_offset_noscale:
                case tic6x_coding_mem_offset:
                    mem_offset = fld_val;
                    mem_offset_known = true;
                    if (num_bits == 16) {
                        mem_mode_known = true;
                        mem_mode = TIC6X_INSN16_MEM_MODE_VAL(opc->flags);
                        mem_scaled_known = true;
                        mem_scaled = true;
                        if (opc->flags & TIC6X_FLAG_INSN16_B15PTR) {
                            mem_base_reg_known = true;
                            mem_base_reg = 15;
                        }
                        /* binutils compares the same enumerator twice here, so
                         * 16-bit noscale offsets stay scaled; kept for fidelity. */
                        if (enc->coding_method == tic6x_coding_mem_offset_noscale
                            || enc->coding_method == tic6x_coding_mem_offset_noscale)
                            mem_scaled = false;
                    }
                    break;

                case tic6x_coding_mem_mode:
                    mem_mode = fld_val;
                    mem_mode_known = true;
                    break;

                case tic6x_coding_scaled:
                    mem_scaled = fld_val;
                    mem_scaled_known = true;
                    break;

                case tic6x_coding_crlo:
                    crlo = fld_val;
                    crlo_known = true;
                    break;

                case tic6x_coding_crhi:
                    crhi = fld_val;
                    crhi_known = true;
                    break;

                case tic6x_coding_fstg:
                case tic6x_coding_fcyc:
                    if (!prev_sploop_found) {
                        uint32_t search_fp_addr = fp_addr;
                        unsigned search_fp_offset = fp_offset;
                        bool search_fp_header_based = hb;
                        tic6x_fetch_packet_header search_fp_header = header;
                        uint8_t search_fp[32];
                        unsigned search_num_bits;
                        unsigned search_opcode;
                        unsigned sploop_ii = 0;

                        memcpy(search_fp, fp, 32);
                        for (int i = 0; i < 48 * 8; i++) {
                            if (search_fp_offset & 2)
                                search_fp_offset -= 2;
                            else if (search_fp_offset >= 4) {
                                if (search_fp_header_based
                                    && search_fp_header.word_compact[(search_fp_offset >> 2) - 1])
                                    search_fp_offset -= 2;
                                else
                                    search_fp_offset -= 4;
                            } else {
                                search_fp_addr -= 32;
                                if (rd(opaque, search_fp_addr, search_fp))
                                    break;
                                search_fp_header_based = check_header(search_fp, &search_fp_header);
                                if (search_fp_header_based)
                                    search_fp_offset = search_fp_header.word_compact[6] ? 26 : 24;
                                else
                                    search_fp_offset = 28;
                            }

                            if (search_fp_header_based)
                                search_num_bits = search_fp_header.word_compact[search_fp_offset >> 2] ? 16 : 32;
                            else
                                search_num_bits = 32;
                            if (search_num_bits == 16)
                                /* binutils passes the current packet's header here. */
                                search_opcode = extract16(search_fp + search_fp_offset, &header);
                            else
                                search_opcode = le32(search_fp + search_fp_offset);

                            if (search_num_bits == 32
                                && ((search_opcode & 0x003ffffe) == 0x00038000
                                    || (search_opcode & 0x003ffffe) == 0x0003a000
                                    || (search_opcode & 0x003ffffe) == 0x0003e000)) {
                                prev_sploop_found = true;
                                sploop_ii = ((search_opcode >> 23) & 0x1f) + 1;
                            } else if (search_num_bits == 16
                                       && (search_opcode & 0x3c7e) == 0x0c66) {
                                prev_sploop_found = true;
                                sploop_ii = (((search_opcode >> 7) & 0x7)
                                             | ((search_opcode >> 11) & 0x8)) + 1;
                            }
                            if (prev_sploop_found) {
                                if (sploop_ii <= 1) fcyc_bits = 0;
                                else if (sploop_ii <= 2) fcyc_bits = 1;
                                else if (sploop_ii <= 4) fcyc_bits = 2;
                                else if (sploop_ii <= 8) fcyc_bits = 3;
                                else if (sploop_ii <= 14) fcyc_bits = 4;
                                else prev_sploop_found = false;
                            }
                            if (prev_sploop_found)
                                break;
                        }
                    }
                    if (!prev_sploop_found) {
                        operands_ok = false;
                        operands_text[op_num] = true;
                        break;
                    }
                    if (fcyc_bits > field_width(field))
                        return -1;
                    if (enc->coding_method == tic6x_coding_fstg) {
                        int t = 0;
                        for (int i = fcyc_bits; i < 6; i++)
                            t = (t << 1) | ((fld_val >> i) & 1);
                        operands_text[op_num] = true;
                        TXT(op_num, "%u", t);
                        o->kind = C66X_OPK_FSTG; o->val = t;
                    } else {
                        operands_text[op_num] = true;
                        TXT(op_num, "%u", fld_val & ((1 << fcyc_bits) - 1));
                        o->kind = C66X_OPK_FCYC; o->val = fld_val & ((1 << fcyc_bits) - 1);
                    }
                    break;

                case tic6x_coding_spmask:
                    if (fld_val == 0)
                        spmask_skip_operand = true;
                    else {
                        char *p = operands[op_num];
                        operands_text[op_num] = true;
                        for (unsigned i = 0; i < 8; i++)
                            if (fld_val & (1 << i)) {
                                *p++ = "LSDM"[i / 2];
                                *p++ = '1' + (i & 1);
                                *p++ = ',';
                            }
                        p[-1] = 0;
                    }
                    o->kind = C66X_OPK_UNITS;
                    o->val = fld_val;
                    break;

                case tic6x_coding_fu:
                case tic6x_coding_data_fu:
                case tic6x_coding_xpath:
                case tic6x_coding_rside:
                    break;

                default:
                    return -1;
                }

                if (mem_base_reg_known_long && mem_offset_known_long) {
                    operands_text[op_num] = true;
                    TXT(op_num, "*+b%u(%u)", mem_base_reg, mem_offset * oi->size);
                    o->kind = C66X_OPK_MEM;
                    o->reg = C66X_B(mem_base_reg);
                    o->mem_mode = 1;
                    o->mem_scale = 1;
                    o->val = mem_offset * oi->size;
                }

                if (mem_base_reg_known && mem_offset_known && mem_mode_known
                    && (mem_scaled_known || oi->form != tic6x_operand_mem_ndw)) {
                    char base[4];
                    bool offset_is_reg;
                    bool offset_scaled;
                    char offset[4];
                    char offsetp[6];

                    snprintf(base, 4, "%c%u", side_ch, mem_base_reg);
                    o->kind = C66X_OPK_MEM;
                    o->reg = side_base + mem_base_reg;
                    o->mem_mode = mem_mode;

                    offset_is_reg = (mem_mode & 4) != 0;
                    if (offset_is_reg) {
                        if (num_bits == 16 && header.rs && !(opc->flags & TIC6X_FLAG_INSN16_NORS))
                            reg_base = 16;
                        snprintf(offset, 4, "%c%u", side_ch, reg_base + mem_offset);
                        if (oi->form == tic6x_operand_mem_ndw)
                            offset_scaled = mem_scaled != 0;
                        else
                            offset_scaled = true;
                        o->mem_offreg = side_base + reg_base + mem_offset;
                        o->mem_scale = offset_scaled ? oi->size : 1;
                    } else {
                        if (oi->form == tic6x_operand_mem_ndw) {
                            offset_scaled = mem_scaled != 0;
                            snprintf(offset, 4, "%u", mem_offset);
                            o->val = mem_offset;
                            o->mem_scale = offset_scaled ? oi->size : 1;
                        } else {
                            offset_scaled = false;
                            snprintf(offset, 4, "%u", mem_offset * oi->size);
                            o->val = mem_offset * oi->size;
                            o->mem_scale = 1;
                        }
                    }

                    if (offset_scaled)
                        snprintf(offsetp, 6, "[%s]", offset);
                    else
                        snprintf(offsetp, 6, "(%s)", offset);

                    operands_text[op_num] = true;
                    switch (mem_mode & ~4u) {
                    case 0: TXT(op_num, "*-%s%s", base, offsetp); break;
                    case 1: TXT(op_num, "*+%s%s", base, offsetp); break;
                    case 2:
                    case 3: operands_ok = false; break;
                    case 8: TXT(op_num, "*--%s%s", base, offsetp); break;
                    case 9: TXT(op_num, "*++%s%s", base, offsetp); break;
                    case 10: TXT(op_num, "*%s--%s", base, offsetp); break;
                    case 11: TXT(op_num, "*%s++%s", base, offsetp); break;
                    default: return -1;
                    }
                }

                if (crlo_known && crhi_known) {
                    tic6x_rw rw = oi->rw;
                    int crid;
                    for (crid = 0; crid < tic6x_ctrl_max; crid++) {
                        if (crlo == tic6x_ctrl_table[crid].crlo
                            && (crhi & tic6x_ctrl_table[crid].crhi_mask) == 0
                            && (rw == tic6x_rw_read
                                ? (tic6x_ctrl_table[crid].rw == tic6x_rw_read
                                   || tic6x_ctrl_table[crid].rw == tic6x_rw_read_write)
                                : (tic6x_ctrl_table[crid].rw == tic6x_rw_write
                                   || tic6x_ctrl_table[crid].rw == tic6x_rw_read_write)))
                            break;
                    }
                    operands_text[op_num] = true;
                    if (crid == tic6x_ctrl_max)
                        operands_ok = false;
                    else {
                        TXT(op_num, "%s", tic6x_ctrl_table[crid].name);
                        o->kind = C66X_OPK_CTRL;
                        o->val = crid;
                    }
                }

                if (operands_text[op_num] || operands_pcrel[op_num] || spmask_skip_operand)
                    break;
            }

            if (spmask_skip_operand) {
                num_operands = 0;
                break;
            }
            if (!operands_text[op_num] && !operands_pcrel[op_num])
                return -1;
        }

        if (!operands_ok)
            continue;

        d.opc = opcode_id;
        d.nops = num_operands;
        if (num_operands == 0 && opc->num_operands == 1
            && opc->operand_info[0].form == tic6x_operand_func_unit) {
            d.op[0].kind = C66X_OPK_UNITS;
            d.op[0].val = 0;
        }
        *out = d;

        if (text) {
            int n = snprintf(text, textlen, "%s%s%s", cond, opc->name, func_unit);
            for (unsigned op_num = 0; op_num < num_operands && n < (int)textlen; op_num++) {
                n += snprintf(text + n, textlen - n, "%c", op_num == 0 ? ' ' : ',');
                if (operands_pcrel[op_num])
                    n += snprintf(text + n, textlen - n, "0x%x", operands_addresses[op_num]);
                else
                    n += snprintf(text + n, textlen - n, "%s", operands[op_num]);
            }
            if (hb && header.prot && n < (int)textlen)
                snprintf(text + n, textlen - n, " || nop 5");
        }
        return num_bits / 8;
    }

    if (num_bits == 32 && decode_ext(opcode, out, text, textlen)) {
        out->prot = hb && header.prot;
        return 4;
    }
    if (text)
        snprintf(text, textlen, "<undefined instruction 0x%.*x>", (int)num_bits / 4, opcode);
    return num_bits / 8;
}
