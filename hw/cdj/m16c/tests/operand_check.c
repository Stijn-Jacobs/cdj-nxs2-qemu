/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Operand cross-check: the operands m16c_decode extracts for every
 * instruction of a Ghidra M16C/60 listing ("address hexbytes mnemonic
 * operands" per line), compared with the operands Ghidra prints for the
 * same bytes. decode_check covers lengths and mnemonics; this covers which
 * register, displacement, immediate and bit each operand names.
 *
 *   operand_check image.flat base listing.txt
 *
 * Ghidra reads bit,base:16[An] and the SB/FB bit forms as a bit address, so
 * the An forms are compared as that bit address; the semantic difference is
 * in the executor, not here. Branch displacements are compared as targets,
 * ADJNZ without its label (Ghidra does not print it).
 */
#include "../m16c_priv.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPS 4
#define OPLEN   40

typedef char opstr[OPLEN];

static const char *const reg8[4] = { "R0L", "R0H", "R1L", "R1H" };
static const char *const reg16[4] = { "R0", "R1", "R2", "R3" };
static const char *const reg32[3] = { "R2R0", "R3R1", "A1A0" };
static const char *const base_name[7] = { "", "A0", "A1", "SB", "FB", "SP", "A1A0" };
static const char *const creg_name[8] = { "", "INTBL", "INTBH", "FLG", "ISP", "SP", "SB", "FB" };
static const char *const flag_name[8] = { "C", "D", "Z", "S", "B", "O", "I", "U" };

static void imm_str(char *out, int64_t v, uint32_t mask)
{
    snprintf(out, OPLEN, "#%lld", (long long)(v & mask));
}

static void opnd_str(char *out, const m16c_opnd *o, unsigned size)
{
    switch (o->kind) {
    case OK_REG8:  snprintf(out, OPLEN, "%s", reg8[o->reg]); break;
    case OK_REG16: snprintf(out, OPLEN, "%s", reg16[o->reg]); break;
    case OK_AREG:  snprintf(out, OPLEN, "A%u", o->reg); break;
    case OK_REG32: snprintf(out, OPLEN, "%s", reg32[o->reg]); break;
    case OK_IMM:
        imm_str(out, o->disp, size == 1 ? 0xFF : size == 2 ? 0xFFFF : ~0u);
        break;
    case OK_MEM:
        if (o->base == BR_NONE) {
            snprintf(out, OPLEN, "A(%d)", o->disp);
        } else {
            snprintf(out, OPLEN, "D(%d,%s)", o->base == BR_A1A0 ? 0 : o->disp,
                     base_name[o->base]);
        }
        break;
    default:
        snprintf(out, OPLEN, "?");
    }
}

static const char *const pushm_name[8] = { "FB", "SB", "A1", "A0", "R3", "R2", "R1", "R0" };

/* The registers of a PUSHM or POPM mask, R0 first. */
static void list_str(char *out, unsigned mask, int pop)
{
    out[0] = 0;
    for (unsigned n = 0; n < 8; n++) {
        if (mask & 1u << (pop ? n : 7 - n)) {
            strcat(out, pushm_name[7 - n]);
            strcat(out, " ");
        }
    }
}

/* Ghidra's bit,base: the bit number and the base as a byte address. */
static void bit_strs(const m16c_insn *in, opstr *ops, int *n)
{
    const m16c_bitop *b = &in->bit;
    int32_t bits = b->disp;

    if (b->reg.kind != OK_NONE) {
        snprintf(ops[(*n)++], OPLEN, "A(%u)", b->bit);
        opnd_str(ops[(*n)++], &b->reg, 2);
        return;
    }
    if (b->base == BR_A0 || b->base == BR_A1) {
        /* An forms: Ghidra's bit address is dsp16 as written. */
        bits = b->disp / 8;
    }
    snprintf(ops[(*n)++], OPLEN, "A(%d)", bits & 7);
    if (b->base == BR_NONE) {
        snprintf(ops[(*n)++], OPLEN, "A(%d)", bits >> 3);
    } else if (b->disp == 0 && (b->base == BR_A0 || b->base == BR_A1)) {
        snprintf(ops[(*n)++], OPLEN, "D(0,%s)", base_name[b->base]);
    } else {
        snprintf(ops[(*n)++], OPLEN, "D(%d,%s)", bits >> 3, base_name[b->base]);
    }
}

static int our_ops(const m16c_insn *in, opstr *ops)
{
    unsigned size = in->size;
    int n = 0;

    switch (in->op) {
    case OP_NEG: case OP_ABS: case OP_INC: case OP_DEC: case OP_NOT:
    case OP_ROLC: case OP_RORC: case OP_ADCF: case OP_POP: case OP_EXTS:
        opnd_str(ops[n++], &in->dst, size);
        break;
    case OP_PUSH: case OP_PUSHA: case OP_DIVU: case OP_DIV: case OP_DIVX:
    case OP_JMPI: case OP_JSRI:
        opnd_str(ops[n++], &in->src, size);
        break;
    case OP_JMP: case OP_JSR: case OP_JCND:
        snprintf(ops[n++], OPLEN, "A(%u)", in->target);
        break;
    case OP_SHL: case OP_SHA: case OP_ROT:
        if (in->aux) {
            snprintf(ops[n++], OPLEN, "R1H");
        } else {
            imm_str(ops[n++], in->imm, ~0u);
        }
        opnd_str(ops[n++], &in->dst, size);
        break;
    case OP_ADJNZ:
        imm_str(ops[n++], in->imm, ~0u);
        opnd_str(ops[n++], &in->dst, size);
        break;
    case OP_STZX:
        imm_str(ops[n++], in->src.disp, 0xFF);
        imm_str(ops[n++], in->imm2, 0xFF);
        opnd_str(ops[n++], &in->dst, 1);
        break;
    case OP_STZ: case OP_STNZ:
        imm_str(ops[n++], in->src.disp, 0xFF);
        opnd_str(ops[n++], &in->dst, 1);
        break;
    case OP_LDC:
        imm_str(ops[n++], in->src.disp, 0xFFFF);
        snprintf(ops[n++], OPLEN, "%s", creg_name[in->cnd]);
        break;
    case OP_STC:
        snprintf(ops[n++], OPLEN, "%s", creg_name[in->cnd]);
        opnd_str(ops[n++], &in->dst, 2);
        break;
    case OP_PUSHC: case OP_POPC:
        snprintf(ops[n++], OPLEN, "%s", creg_name[in->cnd]);
        break;
    case OP_FSET: case OP_FCLR:
        snprintf(ops[n++], OPLEN, "%s", flag_name[in->cnd]);
        break;
    case OP_ENTER: case OP_INT: case OP_LDIPL:
        imm_str(ops[n++], in->imm, ~0u);
        break;
    case OP_ADDSP:
        imm_str(ops[n++], in->imm, 0xFFFF);
        snprintf(ops[n++], OPLEN, "SP");
        break;
    case OP_PUSHM: case OP_POPM:
        list_str(ops[n++], in->imm, in->op == OP_POPM);
        break;
    case OP_BTST: case OP_BTSTC: case OP_BTSTS: case OP_BSET: case OP_BCLR:
    case OP_BNOT: case OP_BAND: case OP_BNAND: case OP_BOR: case OP_BNOR:
    case OP_BXOR: case OP_BNXOR: case OP_BNTST: case OP_BM:
        bit_strs(in, ops, &n);
        break;
    case OP_RTS: case OP_NOP: case OP_REIT: case OP_EXITD: case OP_WAIT:
    case OP_SMOVF: case OP_SMOVB: case OP_SSTR: case OP_BRK: case OP_UND:
    case OP_INTO: case OP_BMC:
        break;
    default:
        if (in->src.kind != OK_NONE) {
            opnd_str(ops[n++], &in->src, size);
        }
        if (in->dst.kind != OK_NONE) {
            opnd_str(ops[n++], &in->dst, size);
        }
    }
    return n;
}

static int is_reg(const char *s)
{
    static const char *const names[] = {
        "R0L", "R0H", "R1L", "R1H", "R0", "R1", "R2", "R3", "A0", "A1",
        "R2R0", "R3R1", "A1A0", "SB", "FB", "SP", "FLG", "ISP", "INTBL",
        "INTBH", "C", "D", "Z", "S", "B", "O", "I", "U",
    };

    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(s, names[i])) {
            return 1;
        }
    }
    return 0;
}

/* Ghidra's operand text to the form our_ops writes. imm_mask is applied to
 * immediates so a byte 0xff and -1 compare equal. */
static void ghidra_op(char *out, const char *text, uint32_t imm_mask)
{
    char buf[64];
    const char *br;

    while (*text == ' ') {
        text++;
    }
    snprintf(buf, sizeof(buf), "%s", text);
    for (size_t n = strlen(buf); n && buf[n - 1] == ' '; n--) {
        buf[n - 1] = 0;
    }
    if (buf[0] == '#') {
        imm_str(out, strtoll(buf + 1, NULL, 0), imm_mask);
    } else if (buf[0] == '(' || strstr(buf, "( ")) {
        char names[64] = "";

        for (char *tok = strtok(buf, "() "); tok; tok = strtok(NULL, "() ")) {
            strcat(names, tok);
            strcat(names, " ");
        }
        snprintf(out, OPLEN, "%s", names);
    } else if (is_reg(buf)) {
        snprintf(out, OPLEN, "%s", buf);
    } else if ((br = strchr(buf, '['))) {
        char reg[16];
        long long v = 0;

        snprintf(reg, sizeof(reg), "%.*s", (int)strlen(br + 1) - 1, br + 1);
        if (br != buf) {
            v = strtoll(buf, NULL, 0);
        }
        snprintf(out, OPLEN, "D(%lld,%s)", v, reg);
    } else {
        snprintf(out, OPLEN, "A(%lld)", strtoll(buf, NULL, 0));
    }
}

static int split_ops(const char *text, char parts[][64])
{
    int n = 0, depth = 0;
    const char *start = text;

    if (!*text) {
        return 0;
    }
    for (const char *p = text;; p++) {
        if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            depth--;
        }
        if ((*p == ',' && !depth) || !*p) {
            snprintf(parts[n++], 64, "%.*s", (int)(p - start), start);
            start = p + 1;
        }
        if (!*p || n == MAX_OPS) {
            break;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    static uint8_t img[0x100000 + 16];
    char line[512];
    unsigned long base, total = 0, bad = 0;
    FILE *f, *l;

    if (argc < 4 || !(f = fopen(argv[1], "rb")) || !(l = fopen(argv[3], "r"))) {
        fprintf(stderr, "usage: %s image.flat base listing.txt\n", argv[0]);
        return 2;
    }
    base = strtoul(argv[2], NULL, 0);
    if (fread(img + base, 1, sizeof(img) - 16 - base, f) == 0) {
        return 2;
    }
    fclose(f);
    while (fgets(line, sizeof(line), l)) {
        char hex[64], mnem[32], text[256] = "", parts[MAX_OPS][64];
        opstr ours[MAX_OPS], theirs[MAX_OPS];
        unsigned long pc;
        m16c_insn in;
        int nours, ntheirs, ok = 1;
        uint32_t mask;

        if (sscanf(line, "%lx %63s %31s %255[^\n]", &pc, hex, mnem, text) < 3 ||
            !strcmp(mnem, "LDINTB") || !m16c_decode(img + pc, pc, &in)) {
            continue;
        }
        total++;
        nours = our_ops(&in, ours);
        ntheirs = split_ops(text, parts);
        mask = in.size == 1 ? 0xFF : in.size == 2 ? 0xFFFF : ~0u;
        if (in.op == OP_SHL || in.op == OP_SHA || in.op == OP_ROT ||
            in.op == OP_ADJNZ || in.op == OP_ENTER || in.op == OP_INT ||
            in.op == OP_LDIPL) {
            mask = ~0u;
        } else if (in.op == OP_ADDSP || in.op == OP_LDC) {
            mask = 0xFFFF;
        } else if (in.op == OP_STZX || in.op == OP_STZ || in.op == OP_STNZ) {
            mask = 0xFF;
        }
        for (int i = 0; i < ntheirs; i++) {
            ghidra_op(theirs[i], parts[i], mask);
        }
        if (in.op == OP_ADJNZ && ntheirs == 2) {
            nours = 2;
        }
        if (nours != ntheirs) {
            ok = 0;
        }
        for (int i = 0; ok && i < nours; i++) {
            ok = !strcmp(ours[i], theirs[i]);
        }
        if (!ok) {
            bad++;
            if (bad <= 60) {
                printf("%06lx %s %s %s\n    ours:", pc, hex, mnem, text);
                for (int i = 0; i < nours; i++) {
                    printf(" [%s]", ours[i]);
                }
                printf("\n  theirs:");
                for (int i = 0; i < ntheirs; i++) {
                    printf(" [%s]", theirs[i]);
                }
                printf("\n");
            }
        }
    }
    printf("%lu instructions, %lu operand mismatches\n", total, bad);
    return bad != 0;
}
