/* Prototype of the whole recompilation path on the simplest game functions
 * (docs/STATIC_RECOMPILATION_PLAN.md, "Next steps" 3): pure integer functions, i.e. FDE
 * functions whose instructions are all in a small integer subset and touch no memory except
 * their own pushes and pops.
 *
 * For each such function this tool (x86-64, under Rosetta 2)
 *   1. emits a C translation into lifted.c (one C function per guest function; registers are
 *      C locals, flags are computed explicitly and left for clang to drop when unused),
 *   2. runs the original machine code on random SysV arguments and writes the results to
 *      reference.bin.
 * check.c, compiled natively for arm64 together with lifted.c, runs the translations on the same
 * arguments and compares rax.
 *
 * usage: lift image.bin funcs.txt outdir [max functions]
 */
#include <Zydis/Zydis.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define CASES 64
#define MAX_INSNS 400

typedef struct {
    uint64_t addr;
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
} Insn;

static uint8_t *img;
static size_t size;
static ZydisDecoder dec;

static int gpr_index(ZydisRegister r) {
    ZydisRegister big = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
    if (big < ZYDIS_REGISTER_RAX || big > ZYDIS_REGISTER_R15) return -1;
    return (int)(big - ZYDIS_REGISTER_RAX);
}
static int is_high8(ZydisRegister r) { return r >= ZYDIS_REGISTER_AH && r <= ZYDIS_REGISTER_BH; }

/* mov rbp, rsp / mov rsp, rbp: the frame pointer, kept as an index into the local stack */
static int frame_move(const Insn *x) {
    if (x->in.mnemonic != ZYDIS_MNEMONIC_MOV || x->ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
        x->ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER)
        return 0;
    if (x->ops[0].reg.value == ZYDIS_REGISTER_RBP && x->ops[1].reg.value == ZYDIS_REGISTER_RSP) return 1;
    if (x->ops[0].reg.value == ZYDIS_REGISTER_RSP && x->ops[1].reg.value == ZYDIS_REGISTER_RBP) return 2;
    return 0;
}

static int supported(const Insn *x) {
    const ZydisDecodedInstruction *in = &x->in;
    if (frame_move(x)) return 1;
    if (in->attributes & (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE |
                          ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS))
        return 0;
    for (int i = 0; i < in->operand_count_visible; ++i) {
        const ZydisDecodedOperand *o = &x->ops[i];
        if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) {
            if (gpr_index(o->reg.value) < 0 || gpr_index(o->reg.value) == 4) return 0;
        } else if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (in->mnemonic != ZYDIS_MNEMONIC_LEA) return 0;
            if (o->mem.base == ZYDIS_REGISTER_RIP || o->mem.segment == ZYDIS_REGISTER_FS ||
                o->mem.segment == ZYDIS_REGISTER_GS) return 0;
            if (o->mem.base != ZYDIS_REGISTER_NONE && (gpr_index(o->mem.base) < 0 || gpr_index(o->mem.base) == 4)) return 0;
            if (o->mem.index != ZYDIS_REGISTER_NONE && gpr_index(o->mem.index) < 0) return 0;
        } else if (o->type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            return 0;
        }
    }
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_MOV: case ZYDIS_MNEMONIC_MOVZX: case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD:
    case ZYDIS_MNEMONIC_LEA: case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_NOT: case ZYDIS_MNEMONIC_NEG:
    case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_IMUL: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_TEST:
    case ZYDIS_MNEMONIC_CDQE: case ZYDIS_MNEMONIC_CDQ: case ZYDIS_MNEMONIC_CQO:
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_RET:
        break;
    case ZYDIS_MNEMONIC_PUSH: case ZYDIS_MNEMONIC_POP:
        if (x->ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || x->ops[0].size != 64) return 0;
        break;
    default:
        if (in->meta.category == ZYDIS_CATEGORY_COND_BR || in->meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
            if (x->ops[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) return 0;
            break;
        }
        if (in->meta.category == ZYDIS_CATEGORY_CMOV || in->meta.category == ZYDIS_CATEGORY_SETCC) break;
        return 0;
    }
    if (in->mnemonic == ZYDIS_MNEMONIC_IMUL && in->operand_count_visible == 1) return 0; /* rdx:rax form */
    if ((in->mnemonic == ZYDIS_MNEMONIC_SHL || in->mnemonic == ZYDIS_MNEMONIC_SHR || in->mnemonic == ZYDIS_MNEMONIC_SAR) &&
        in->operand_count_visible == 1) return 1; /* shift by 1 */
    return 1;
}

/* ---- C emission ---- */
static FILE *out;

static const char *cname(int idx) {
    static const char *n[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    return n[idx];
}
/* Expression reading operand o, zero-extended to 64 bits. */
static void read_op(const Insn *x, int i, char *buf, size_t n) {
    const ZydisDecodedOperand *o = &x->ops[i];
    if (o->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        uint64_t v = o->imm.is_signed ? (uint64_t)o->imm.value.s : o->imm.value.u;
        unsigned bits = x->ops[0].size ? x->ops[0].size : 64;
        if (bits < 64) v &= (1ull << bits) - 1;
        snprintf(buf, n, "0x%llxull", (unsigned long long)v);
        return;
    }
    int r = gpr_index(o->reg.value);
    if (is_high8(o->reg.value)) { snprintf(buf, n, "((%s >> 8) & 0xff)", cname(r)); return; }
    switch (o->size) {
    case 8: snprintf(buf, n, "(%s & 0xff)", cname(r)); break;
    case 16: snprintf(buf, n, "(%s & 0xffff)", cname(r)); break;
    case 32: snprintf(buf, n, "(%s & 0xffffffffull)", cname(r)); break;
    default: snprintf(buf, n, "%s", cname(r)); break;
    }
}
static void write_reg(ZydisRegister reg, unsigned size, const char *val) {
    int r = gpr_index(reg);
    if (is_high8(reg)) fprintf(out, "    %s = (%s & ~0xff00ull) | (((%s) & 0xff) << 8);\n", cname(r), cname(r), val);
    else if (size == 8) fprintf(out, "    %s = (%s & ~0xffull) | ((%s) & 0xff);\n", cname(r), cname(r), val);
    else if (size == 16) fprintf(out, "    %s = (%s & ~0xffffull) | ((%s) & 0xffff);\n", cname(r), cname(r), val);
    else if (size == 32) fprintf(out, "    %s = (%s) & 0xffffffffull;\n", cname(r), val);
    else fprintf(out, "    %s = (%s);\n", cname(r), val);
}
static const char *mask_of(unsigned bits) {
    return bits == 8 ? "0xffull" : bits == 16 ? "0xffffull" : bits == 32 ? "0xffffffffull" : "~0ull";
}
static const char *sign_of(unsigned bits) {
    return bits == 8 ? "0x80ull" : bits == 16 ? "0x8000ull" : bits == 32 ? "0x80000000ull" : "0x8000000000000000ull";
}
static void flags_logic(const char *res, unsigned bits) {
    fprintf(out, "    cf = 0; of = 0; zf = ((%s) & %s) == 0; sf = ((%s) & %s) != 0;\n", res, mask_of(bits), res, sign_of(bits));
}
static const char *cond_expr(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_CMOVZ: case ZYDIS_MNEMONIC_SETZ: return "zf";
    case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_CMOVNZ: case ZYDIS_MNEMONIC_SETNZ: return "!zf";
    case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_CMOVB: case ZYDIS_MNEMONIC_SETB: return "cf";
    case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_CMOVNB: case ZYDIS_MNEMONIC_SETNB: return "!cf";
    case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_CMOVBE: case ZYDIS_MNEMONIC_SETBE: return "(cf || zf)";
    case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_CMOVNBE: case ZYDIS_MNEMONIC_SETNBE: return "(!cf && !zf)";
    case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_CMOVS: case ZYDIS_MNEMONIC_SETS: return "sf";
    case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_CMOVNS: case ZYDIS_MNEMONIC_SETNS: return "!sf";
    case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_CMOVO: case ZYDIS_MNEMONIC_SETO: return "of";
    case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_CMOVNO: case ZYDIS_MNEMONIC_SETNO: return "!of";
    case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_CMOVL: case ZYDIS_MNEMONIC_SETL: return "(sf != of)";
    case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_CMOVNL: case ZYDIS_MNEMONIC_SETNL: return "(sf == of)";
    case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_CMOVLE: case ZYDIS_MNEMONIC_SETLE: return "(zf || sf != of)";
    case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_CMOVNLE: case ZYDIS_MNEMONIC_SETNLE: return "(!zf && sf == of)";
    default: return NULL; /* parity: not supported */
    }
}

static int emit_function(uint64_t start, const Insn *xs, int n) {
    /* first pass: conditions all known */
    for (int i = 0; i < n; ++i) {
        const ZydisDecodedInstruction *in = &xs[i].in;
        if ((in->meta.category == ZYDIS_CATEGORY_COND_BR || in->meta.category == ZYDIS_CATEGORY_CMOV ||
             in->meta.category == ZYDIS_CATEGORY_SETCC) && !cond_expr(in->mnemonic))
            return 0;
    }
    fprintf(out, "uint64_t L_%llx(const uint64_t *a) {\n", (unsigned long long)start);
    fprintf(out, "    uint64_t rax = 0, rcx = a[3], rdx = a[2], rbx = 0, rbp = 0, rsi = a[1], rdi = a[0];\n"
                 "    uint64_t r8 = a[4], r9 = a[5], r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;\n"
                 "    uint64_t stack[64]; int sp = 64; int cf = 0, zf = 0, sf = 0, of = 0;\n"
                 "    (void)cf; (void)zf; (void)sf; (void)of; (void)stack; (void)sp;\n");
    char a[96], b[96], expr[512];
    for (int i = 0; i < n; ++i) {
        const Insn *x = &xs[i];
        const ZydisDecodedInstruction *in = &x->in;
        const ZydisDecodedOperand *o = x->ops;
        unsigned bits = o[0].size;
        fprintf(out, "L%llx:;\n", (unsigned long long)x->addr);
        if (frame_move(x) == 1) { fprintf(out, "    rbp = 0x50000ull + (uint64_t)sp;\n"); continue; }
        if (frame_move(x) == 2) { fprintf(out, "    sp = (int)(rbp - 0x50000ull);\n"); continue; }
        switch (in->mnemonic) {
        case ZYDIS_MNEMONIC_NOP: break;
        case ZYDIS_MNEMONIC_MOV:
            read_op(x, 1, a, sizeof(a)); write_reg(o[0].reg.value, bits, a); break;
        case ZYDIS_MNEMONIC_MOVZX:
            read_op(x, 1, a, sizeof(a)); write_reg(o[0].reg.value, bits, a); break;
        case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD: {
            read_op(x, 1, a, sizeof(a));
            unsigned sb = o[1].size;
            snprintf(expr, sizeof(expr), "(uint64_t)(int64_t)(int%u_t)%s", sb, a);
            write_reg(o[0].reg.value, bits, expr);
            break;
        }
        case ZYDIS_MNEMONIC_CDQE: fprintf(out, "    rax = (uint64_t)(int64_t)(int32_t)rax;\n"); break;
        case ZYDIS_MNEMONIC_CDQ: fprintf(out, "    rdx = ((int32_t)rax < 0) ? 0xffffffffull : 0;\n"); break;
        case ZYDIS_MNEMONIC_CQO: fprintf(out, "    rdx = ((int64_t)rax < 0) ? ~0ull : 0;\n"); break;
        case ZYDIS_MNEMONIC_LEA: {
            const ZydisDecodedOperand *m = &o[1];
            int len = snprintf(expr, sizeof(expr), "(0x%llxull", (unsigned long long)m->mem.disp.value);
            if (m->mem.base != ZYDIS_REGISTER_NONE) len += snprintf(expr + len, sizeof(expr) - (size_t)len, " + %s", cname(gpr_index(m->mem.base)));
            if (m->mem.index != ZYDIS_REGISTER_NONE)
                len += snprintf(expr + len, sizeof(expr) - (size_t)len, " + %s * %u", cname(gpr_index(m->mem.index)), m->mem.scale);
            snprintf(expr + len, sizeof(expr) - (size_t)len, ")");
            if (m->mem.base != ZYDIS_REGISTER_NONE && ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, m->mem.base) == 32) return -1;
            write_reg(o[0].reg.value, bits, expr);
            break;
        }
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: {
            read_op(x, 0, a, sizeof(a)); read_op(x, 1, b, sizeof(b));
            const int sub = in->mnemonic != ZYDIS_MNEMONIC_ADD;
            fprintf(out, "    { uint64_t x = %s, y = %s & %s, r = (x %c y) & %s;\n", a, b, mask_of(bits), sub ? '-' : '+', mask_of(bits));
            if (sub) fprintf(out, "      cf = x < y; of = (((x ^ y) & (x ^ r)) & %s) != 0;\n", sign_of(bits));
            else fprintf(out, "      cf = r < x; of = ((~(x ^ y) & (x ^ r)) & %s) != 0;\n", sign_of(bits));
            fprintf(out, "      zf = r == 0; sf = (r & %s) != 0;\n", sign_of(bits));
            if (in->mnemonic != ZYDIS_MNEMONIC_CMP) { fprintf(out, "  "); write_reg(o[0].reg.value, bits, "r"); }
            fprintf(out, "    }\n");
            break;
        }
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST: {
            read_op(x, 0, a, sizeof(a)); read_op(x, 1, b, sizeof(b));
            char opc = in->mnemonic == ZYDIS_MNEMONIC_OR ? '|' : in->mnemonic == ZYDIS_MNEMONIC_XOR ? '^' : '&';
            fprintf(out, "    { uint64_t r = (%s %c %s) & %s;\n  ", a, opc, b, mask_of(bits));
            flags_logic("r", bits);
            if (in->mnemonic != ZYDIS_MNEMONIC_TEST) { fprintf(out, "  "); write_reg(o[0].reg.value, bits, "r"); }
            fprintf(out, "    }\n");
            break;
        }
        case ZYDIS_MNEMONIC_NOT:
            read_op(x, 0, a, sizeof(a)); snprintf(expr, sizeof(expr), "~%s", a); write_reg(o[0].reg.value, bits, expr); break;
        case ZYDIS_MNEMONIC_NEG:
            read_op(x, 0, a, sizeof(a));
            fprintf(out, "    { uint64_t x = %s, r = (0 - x) & %s; cf = x != 0; of = x == %s; zf = r == 0; sf = (r & %s) != 0;\n  ",
                    a, mask_of(bits), sign_of(bits), sign_of(bits));
            write_reg(o[0].reg.value, bits, "r");
            fprintf(out, "    }\n");
            break;
        case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: { /* CF unchanged */
            read_op(x, 0, a, sizeof(a));
            const int dec_ = in->mnemonic == ZYDIS_MNEMONIC_DEC;
            fprintf(out, "    { uint64_t x = %s, r = (x %c 1) & %s; of = %s == %s; zf = r == 0; sf = (r & %s) != 0;\n  ",
                    a, dec_ ? '-' : '+', mask_of(bits), dec_ ? "x" : "r", sign_of(bits), sign_of(bits));
            write_reg(o[0].reg.value, bits, "r");
            fprintf(out, "    }\n");
            break;
        }
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: {
            read_op(x, 0, a, sizeof(a));
            if (in->operand_count_visible > 1) read_op(x, 1, b, sizeof(b)); else snprintf(b, sizeof(b), "1");
            const char *cm = bits == 64 ? "63" : "31";
            fprintf(out, "    { uint64_t x = %s; unsigned c = (unsigned)(%s) & %s;\n", a, b, cm);
            fprintf(out, "      if (c) { uint64_t r;\n");
            if (in->mnemonic == ZYDIS_MNEMONIC_SHL)
                fprintf(out, "        r = (c >= %u ? 0 : (x << c)) & %s; cf = c <= %u ? ((x >> (%u - c)) & 1) : 0; of = c == 1 ? (((r & %s) != 0) != cf) : of;\n",
                        bits, mask_of(bits), bits, bits, sign_of(bits));
            else if (in->mnemonic == ZYDIS_MNEMONIC_SHR)
                fprintf(out, "        r = c >= %u ? 0 : (x >> c); cf = c <= %u ? ((x >> (c - 1)) & 1) : 0; of = c == 1 ? ((x & %s) != 0) : of;\n",
                        bits, bits, sign_of(bits));
            else
                fprintf(out, "        { int64_t sx = (int64_t)(x << (64 - %u)) >> (64 - %u); r = (uint64_t)(sx >> (c >= %u ? %u : c)) & %s; cf = (uint64_t)(sx >> (c >= %u ? %u : c - 1)) & 1; of = c == 1 ? 0 : of; }\n",
                        bits, bits, bits, bits - 1, mask_of(bits), bits, bits - 1);
            fprintf(out, "        zf = r == 0; sf = (r & %s) != 0;\n  ", sign_of(bits));
            write_reg(o[0].reg.value, bits, "r");
            fprintf(out, "      }\n    }\n");
            break;
        }
        case ZYDIS_MNEMONIC_IMUL: {
            if (in->operand_count_visible == 3) { read_op(x, 1, a, sizeof(a)); read_op(x, 2, b, sizeof(b)); }
            else { read_op(x, 0, a, sizeof(a)); read_op(x, 1, b, sizeof(b)); }
            fprintf(out, "    { __int128 p = (__int128)(int%u_t)(%s) * (__int128)(int%u_t)(%s); uint64_t r = (uint64_t)p & %s;\n",
                    bits, a, bits, b, mask_of(bits));
            fprintf(out, "      cf = of = (p != (__int128)(int%u_t)r); zf = r == 0; sf = (r & %s) != 0;\n  ", bits, sign_of(bits));
            write_reg(o[0].reg.value, bits, "r");
            fprintf(out, "    }\n");
            break;
        }
        case ZYDIS_MNEMONIC_PUSH:
            fprintf(out, "    stack[--sp] = %s;\n", cname(gpr_index(o[0].reg.value))); break;
        case ZYDIS_MNEMONIC_POP:
            fprintf(out, "    %s = stack[sp++];\n", cname(gpr_index(o[0].reg.value))); break;
        case ZYDIS_MNEMONIC_RET:
            fprintf(out, "    return rax;\n"); break;
        default:
            if (in->meta.category == ZYDIS_CATEGORY_COND_BR) {
                fprintf(out, "    if (%s) goto L%llx;\n", cond_expr(in->mnemonic),
                        (unsigned long long)(x->addr + in->length + (uint64_t)o[0].imm.value.s));
            } else if (in->meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
                fprintf(out, "    goto L%llx;\n", (unsigned long long)(x->addr + in->length + (uint64_t)o[0].imm.value.s));
            } else if (in->meta.category == ZYDIS_CATEGORY_CMOV) {
                read_op(x, 1, a, sizeof(a));
                snprintf(expr, sizeof(expr), "%s", a);
                fprintf(out, "    if (%s) {\n  ", cond_expr(in->mnemonic));
                write_reg(o[0].reg.value, bits, expr);
                fprintf(out, "    } else {\n  ");
                if (bits == 32) write_reg(o[0].reg.value, 32, cname(gpr_index(o[0].reg.value))); /* zero-extends */
                fprintf(out, "    }\n");
            } else if (in->meta.category == ZYDIS_CATEGORY_SETCC) {
                snprintf(expr, sizeof(expr), "(uint64_t)(%s)", cond_expr(in->mnemonic));
                write_reg(o[0].reg.value, 8, expr);
            } else {
                return -1;
            }
        }
    }
    fprintf(out, "    return rax;\n}\n\n");
    return 1;
}

/* ---- reference runs ---- */
static sigjmp_buf jump;
static void on_fault(int sig) { siglongjmp(jump, sig); }
static uint64_t rng = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t arg(void) {
    switch (rnd() % 5) {
    case 0: return rnd() % 16;
    case 1: return (uint64_t)(int64_t)(int32_t)rnd();
    case 2: return rnd() & 0xffffffffull;
    default: return rnd();
    }
}
typedef uint64_t (*Fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s image.bin funcs.txt outdir [max]\n", argv[0]); return 2; }
    size_t max = argc > 4 ? (size_t)atoll(argv[4]) : 100000;
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    img = malloc(size);
    if (fread(img, 1, size, f) != size) return 1;
    fclose(f);
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    char path[1024];
    snprintf(path, sizeof(path), "%s/lifted.c", argv[3]);
    out = fopen(path, "w");
    fprintf(out, "/* Generated by tools/bbrecomp/proto/lift.c from the game image: never commit. */\n"
                 "#include <stdint.h>\n\n");
    snprintf(path, sizeof(path), "%s/reference.bin", argv[3]);
    FILE *ref = fopen(path, "wb");
    signal(SIGSEGV, on_fault); signal(SIGBUS, on_fault); signal(SIGILL, on_fault); signal(SIGFPE, on_fault);
    uint8_t *code = mmap(NULL, 64 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    size_t code_used = 0;

    FILE *fl = fopen(argv[2], "r");
    unsigned long long s, l;
    size_t candidates = 0, emitted = 0, faulted = 0;
    uint64_t *addrs = malloc(200000 * sizeof(uint64_t));
    static Insn xs[MAX_INSNS];
    while (fscanf(fl, "%llx %llx", &s, &l) == 2 && emitted < max) {
        if (l > 4096) continue;
        int n = 0, ok = 1, has_ret = 0;
        for (uint64_t a = s; a < s + l;) {
            if (n == MAX_INSNS) { ok = 0; break; }
            Insn *x = &xs[n];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, img + a, s + l - a, &x->in, x->ops))) {
                /* padding after the last ret is fine */
                break;
            }
            x->addr = a;
            if (x->in.mnemonic == ZYDIS_MNEMONIC_INT3 && has_ret) break; /* padding */
            if (!supported(x)) { ok = 0; break; }
            if (x->in.mnemonic == ZYDIS_MNEMONIC_RET) has_ret = 1;
            /* branches must stay inside the function */
            if (x->in.meta.category == ZYDIS_CATEGORY_COND_BR || x->in.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
                uint64_t t = a + x->in.length + (uint64_t)x->ops[0].imm.value.s;
                if (t < s || t >= s + l) { ok = 0; break; }
            }
            a += x->in.length;
            ++n;
        }
        if (!ok || !has_ret || n < 3) continue;
        for (int i = 0; i < n && ok; ++i) { /* every branch target is a decoded instruction */
            const Insn *x = &xs[i];
            if (x->in.meta.category != ZYDIS_CATEGORY_COND_BR && x->in.meta.category != ZYDIS_CATEGORY_UNCOND_BR) continue;
            uint64_t t = x->addr + x->in.length + (uint64_t)x->ops[0].imm.value.s;
            int found = 0;
            for (int j = 0; j < n; ++j) if (xs[j].addr == t) { found = 1; break; }
            ok = found;
        }
        if (!ok) continue;
        ++candidates;
        char *text = NULL;
        size_t text_len = 0;
        FILE *file_out = out;
        out = open_memstream(&text, &text_len);
        int r = emit_function(s, xs, n);
        fclose(out);
        out = file_out;
        if (r <= 0) { free(text); continue; }
        /* reference: run the original bytes at a fresh address */
        uint8_t *fn = code + code_used;
        code_used += (l + 63) & ~63ull;
        mprotect((void *)((uintptr_t)fn & ~4095ull), ((l + 4095) & ~4095ull) + 4096, PROT_READ | PROT_WRITE);
        memcpy(fn, img + s, l);
        mprotect((void *)((uintptr_t)fn & ~4095ull), ((l + 4095) & ~4095ull) + 4096, PROT_READ | PROT_EXEC);
        uint64_t results[CASES][7];
        int bad = 0;
        for (int k = 0; k < CASES && !bad; ++k) {
            for (int j = 0; j < 6; ++j) results[k][j] = arg();
            if (sigsetjmp(jump, 1)) { bad = 1; break; }
            results[k][6] = ((Fn)fn)(results[k][0], results[k][1], results[k][2], results[k][3], results[k][4], results[k][5]);
        }
        if (bad) { ++faulted; free(text); continue; }
        fwrite(text, 1, text_len, out);
        free(text);
        uint64_t addr = s;
        fwrite(&addr, 8, 1, ref);
        fwrite(results, sizeof(results), 1, ref);
        addrs[emitted++] = s;
    }
    fclose(fl);
    fprintf(out, "typedef uint64_t (*LiftedFn)(const uint64_t *);\n");
    for (size_t i = 0; i < emitted; ++i) fprintf(out, "uint64_t L_%llx(const uint64_t *a);\n", (unsigned long long)addrs[i]);
    fprintf(out, "const struct { uint64_t addr; LiftedFn fn; } lifted[] = {\n");
    for (size_t i = 0; i < emitted; ++i) fprintf(out, "    {0x%llx, L_%llx},\n", (unsigned long long)addrs[i], (unsigned long long)addrs[i]);
    fprintf(out, "};\nconst unsigned lifted_count = %zu;\n", emitted);
    fclose(out);
    fclose(ref);
    fprintf(stderr, "%zu candidate pure-integer functions, %zu lifted with references (%zu faulted when run)\n",
            candidates, emitted, faulted);
    return 0;
}
