/* Prototype, step 4: step 3 plus AVX (simd.inc). Earlier notes:
 * Prototype, step 3: integer functions with memory *and calls* (docs/STATIC_RECOMPILATION_PLAN.md).
 * As step 2 (lift2.c), plus: registers live in a context struct shared by all lifted functions;
 * `call rel32` pushes the guest return address on the guest stack (as the original does when it
 * runs in place) and calls the callee's C function; `jmp rel32` to another function's start is a
 * tail call. A function is accepted when it and everything it calls fit the integer subset.
 *
 * Step 2 notes follow.
 *
 * Guest addresses are host addresses: both this tool (x86-64, under Rosetta 2) and check2.c
 * (arm64) map the game image at IMAGE_BASE and a 64 KiB buffer of random bytes at DATA_BASE, at
 * the same virtual addresses. The original functions run in place in the image; their lifted C
 * reads and writes the same addresses. Arguments are mostly pointers into the buffer. Per case
 * the buffer is restored, the function runs, and rax plus a hash of the buffer are compared.
 *
 * Accepted functions: no calls, no indirect jumps, no SSE/x87; integer instructions from the
 * subset below with register, immediate or memory operands (rip-relative included); rsp is an
 * ordinary register pointing into a local stack.
 *
 * usage: lift4 image.bin funcs.txt outdir [max functions]
 */
#include <Zydis/Zydis.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>

#define IMAGE_BASE 0x200000000000ull
#define DATA_BASE 0x210000000000ull
#define DATA_SIZE 65536
#define CASES 16
#define MAX_INSNS 600

typedef struct {
    uint64_t addr;
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
} Insn;

static uint8_t *img;
static size_t size;
static ZydisDecoder dec;
static FILE *out;

static int gpr_index(ZydisRegister r) {
    ZydisRegister big = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
    if (big < ZYDIS_REGISTER_RAX || big > ZYDIS_REGISTER_R15) return -1;
    return (int)(big - ZYDIS_REGISTER_RAX);
}
static int is_high8(ZydisRegister r) { return r >= ZYDIS_REGISTER_AH && r <= ZYDIS_REGISTER_BH; }
static const char *cname(int idx) {
    static const char *n[16] = {"c->rax", "c->rcx", "c->rdx", "c->rbx", "c->rsp", "c->rbp", "c->rsi", "c->rdi",
                                "c->r8", "c->r9", "c->r10", "c->r11", "c->r12", "c->r13", "c->r14", "c->r15"};
    return n[idx];
}
static const char *mask_of(unsigned bits) {
    return bits == 8 ? "0xffull" : bits == 16 ? "0xffffull" : bits == 32 ? "0xffffffffull" : "~0ull";
}
static const char *sign_of(unsigned bits) {
    return bits == 8 ? "0x80ull" : bits == 16 ? "0x8000ull" : bits == 32 ? "0x80000000ull" : "0x8000000000000000ull";
}

static int mem_ok(const Insn *x, const ZydisDecodedOperand *o) {
    if (o->mem.segment == ZYDIS_REGISTER_FS || o->mem.segment == ZYDIS_REGISTER_GS) return 0;
    if (x->in.address_width != 64) return 0;
    if (o->mem.base != ZYDIS_REGISTER_NONE && o->mem.base != ZYDIS_REGISTER_RIP && gpr_index(o->mem.base) < 0) return 0;
    if (o->mem.index != ZYDIS_REGISTER_NONE && gpr_index(o->mem.index) < 0) return 0;
    return 1;
}

static int simd_supported(const Insn *x);
static int supported(const Insn *x) {
    const ZydisDecodedInstruction *in = &x->in;
    if (in->encoding == ZYDIS_INSTRUCTION_ENCODING_VEX) return simd_supported(x);
    if (in->attributes & (ZYDIS_ATTRIB_HAS_LOCK | ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE))
        return 0;
    for (int i = 0; i < in->operand_count_visible; ++i) {
        const ZydisDecodedOperand *o = &x->ops[i];
        if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) {
            if (gpr_index(o->reg.value) < 0) return 0;
        } else if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (!mem_ok(x, o)) return 0;
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
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_RET: case ZYDIS_MNEMONIC_PUSH: case ZYDIS_MNEMONIC_POP:
    case ZYDIS_MNEMONIC_LEAVE:
        break;
    default:
        if (in->meta.category == ZYDIS_CATEGORY_COND_BR || in->meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
            in->meta.category == ZYDIS_CATEGORY_CALL) {
            if (x->ops[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) return 0;
            break;
        }
        if (in->meta.category == ZYDIS_CATEGORY_CMOV || in->meta.category == ZYDIS_CATEGORY_SETCC) break;
        return 0;
    }
    if (in->mnemonic == ZYDIS_MNEMONIC_IMUL && in->operand_count_visible == 1) return 0;
    if ((in->mnemonic == ZYDIS_MNEMONIC_PUSH || in->mnemonic == ZYDIS_MNEMONIC_POP) &&
        (x->ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || x->ops[0].size != 64))
        return 0;
    return 1;
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
    case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_CMOVP: case ZYDIS_MNEMONIC_SETP: return "pf";
    case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_CMOVNP: case ZYDIS_MNEMONIC_SETNP: return "!pf";
    default: return NULL;
    }
}

/* Address expression of a memory operand (guest address = host address). */
static void addr_expr(const Insn *x, const ZydisDecodedOperand *o, char *buf, size_t n) {
    if (o->mem.base == ZYDIS_REGISTER_RIP) {
        uint64_t target = x->addr + x->in.length + (uint64_t)o->mem.disp.value;
        snprintf(buf, n, "(0x%llxull)", (unsigned long long)(IMAGE_BASE + target));
        return;
    }
    int len = snprintf(buf, n, "(0x%llxull", (unsigned long long)o->mem.disp.value);
    if (o->mem.base != ZYDIS_REGISTER_NONE) len += snprintf(buf + len, n - (size_t)len, " + %s", cname(gpr_index(o->mem.base)));
    if (o->mem.index != ZYDIS_REGISTER_NONE)
        len += snprintf(buf + len, n - (size_t)len, " + %s * %u", cname(gpr_index(o->mem.index)), o->mem.scale);
    snprintf(buf + len, n - (size_t)len, ")");
}

static void read_op(const Insn *x, int i, char *buf, size_t n) {
    const ZydisDecodedOperand *o = &x->ops[i];
    if (o->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        uint64_t v = o->imm.is_signed ? (uint64_t)o->imm.value.s : o->imm.value.u;
        unsigned bits = x->ops[0].size ? x->ops[0].size : 64;
        if (bits < 64) v &= (1ull << bits) - 1;
        snprintf(buf, n, "0x%llxull", (unsigned long long)v);
        return;
    }
    if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) {
        char a[200];
        addr_expr(x, o, a, sizeof(a));
        snprintf(buf, n, "(uint64_t)*(volatile uint%u_t *)(uintptr_t)%s", o->size, a);
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
/* Writes val (low `size` bits) to operand i of x: register (merge/zero-extend) or memory. */
static void write_op(const Insn *x, int i, unsigned size, const char *val) {
    const ZydisDecodedOperand *o = &x->ops[i];
    if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) {
        char a[200];
        addr_expr(x, o, a, sizeof(a));
        fprintf(out, "    *(volatile uint%u_t *)(uintptr_t)%s = (uint%u_t)(%s);\n", size, a, size, val);
        return;
    }
    ZydisRegister reg = o->reg.value;
    int r = gpr_index(reg);
    if (is_high8(reg)) fprintf(out, "    %s = (%s & ~0xff00ull) | (((%s) & 0xff) << 8);\n", cname(r), cname(r), val);
    else if (size == 8) fprintf(out, "    %s = (%s & ~0xffull) | ((%s) & 0xff);\n", cname(r), cname(r), val);
    else if (size == 16) fprintf(out, "    %s = (%s & ~0xffffull) | ((%s) & 0xffff);\n", cname(r), cname(r), val);
    else if (size == 32) fprintf(out, "    %s = (%s) & 0xffffffffull;\n", cname(r), val);
    else fprintf(out, "    %s = (%s);\n", cname(r), val);
}
static void zps(const char *res, unsigned bits) {
    fprintf(out, "      zf = ((%s) & %s) == 0; sf = ((%s) & %s) != 0; pf = !__builtin_parity((unsigned)((%s) & 0xff));\n",
            res, mask_of(bits), res, sign_of(bits), res);
}

#include "simd.inc"

static int emit_function(uint64_t start, const Insn *xs, int n) {
    for (int i = 0; i < n; ++i) {
        const ZydisDecodedInstruction *in = &xs[i].in;
        if ((in->meta.category == ZYDIS_CATEGORY_COND_BR || in->meta.category == ZYDIS_CATEGORY_CMOV ||
             in->meta.category == ZYDIS_CATEGORY_SETCC) && !cond_expr(in->mnemonic))
            return 0;
    }
    fprintf(out, "void L_%llx(Ctx *c) {\n", (unsigned long long)start);
    fprintf(out, "    int cf = 0, zf = 0, sf = 0, of = 0, pf = 0;\n"
                 "    (void)cf; (void)zf; (void)sf; (void)of; (void)pf;\n");
    char a[256], b[256], expr[768];
    for (int i = 0; i < n; ++i) {
        const Insn *x = &xs[i];
        const ZydisDecodedInstruction *in = &x->in;
        const ZydisDecodedOperand *o = x->ops;
        unsigned bits = o[0].size;
        fprintf(out, "L%llx:;\n", (unsigned long long)x->addr);
        if (in->encoding == ZYDIS_INSTRUCTION_ENCODING_VEX) {
            if (!emit_simd(x)) return -1;
            continue;
        }
        switch (in->mnemonic) {
        case ZYDIS_MNEMONIC_NOP: break;
        case ZYDIS_MNEMONIC_MOV: case ZYDIS_MNEMONIC_MOVZX:
            read_op(x, 1, a, sizeof(a)); write_op(x, 0, bits, a); break;
        case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD:
            read_op(x, 1, a, sizeof(a));
            snprintf(expr, sizeof(expr), "(uint64_t)(int64_t)(int%u_t)%s", o[1].size, a);
            write_op(x, 0, bits, expr);
            break;
        case ZYDIS_MNEMONIC_CDQE: fprintf(out, "    c->rax = (uint64_t)(int64_t)(int32_t)c->rax;\n"); break;
        case ZYDIS_MNEMONIC_CDQ: fprintf(out, "    c->rdx = ((int32_t)c->rax < 0) ? 0xffffffffull : 0;\n"); break;
        case ZYDIS_MNEMONIC_CQO: fprintf(out, "    c->rdx = ((int64_t)c->rax < 0) ? ~0ull : 0;\n"); break;
        case ZYDIS_MNEMONIC_LEA:
            addr_expr(x, &o[1], expr, sizeof(expr)); write_op(x, 0, bits, expr); break;
        case ZYDIS_MNEMONIC_PUSH:
            fprintf(out, "    { uint64_t v = %s; c->rsp -= 8; *(volatile uint64_t *)(uintptr_t)c->rsp = v; }\n", cname(gpr_index(o[0].reg.value))); break;
        case ZYDIS_MNEMONIC_POP:
            fprintf(out, "    { uint64_t v = *(volatile uint64_t *)(uintptr_t)c->rsp; c->rsp += 8; %s = v; }\n", cname(gpr_index(o[0].reg.value))); break;
        case ZYDIS_MNEMONIC_LEAVE:
            fprintf(out, "    c->rsp = c->rbp; c->rbp = *(volatile uint64_t *)(uintptr_t)c->rsp; c->rsp += 8;\n"); break;
        case ZYDIS_MNEMONIC_RET:
            fprintf(out, "    c->rsp += 8; return;\n"); break;
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: {
            read_op(x, 0, a, sizeof(a)); read_op(x, 1, b, sizeof(b));
            const int sub = in->mnemonic != ZYDIS_MNEMONIC_ADD;
            fprintf(out, "    { uint64_t x = %s & %s, y = %s & %s, r = (x %c y) & %s;\n", a, mask_of(bits), b, mask_of(bits),
                    sub ? '-' : '+', mask_of(bits));
            if (sub) fprintf(out, "      cf = x < y; of = (((x ^ y) & (x ^ r)) & %s) != 0;\n", sign_of(bits));
            else fprintf(out, "      cf = r < x; of = ((~(x ^ y) & (x ^ r)) & %s) != 0;\n", sign_of(bits));
            zps("r", bits);
            if (in->mnemonic != ZYDIS_MNEMONIC_CMP) write_op(x, 0, bits, "r");
            fprintf(out, "    }\n");
            break;
        }
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST: {
            read_op(x, 0, a, sizeof(a)); read_op(x, 1, b, sizeof(b));
            char opc = in->mnemonic == ZYDIS_MNEMONIC_OR ? '|' : in->mnemonic == ZYDIS_MNEMONIC_XOR ? '^' : '&';
            fprintf(out, "    { uint64_t r = (%s %c %s) & %s; cf = 0; of = 0;\n", a, opc, b, mask_of(bits));
            zps("r", bits);
            if (in->mnemonic != ZYDIS_MNEMONIC_TEST) write_op(x, 0, bits, "r");
            fprintf(out, "    }\n");
            break;
        }
        case ZYDIS_MNEMONIC_NOT:
            read_op(x, 0, a, sizeof(a)); snprintf(expr, sizeof(expr), "~%s", a); write_op(x, 0, bits, expr); break;
        case ZYDIS_MNEMONIC_NEG:
            read_op(x, 0, a, sizeof(a));
            fprintf(out, "    { uint64_t x = %s & %s, r = (0 - x) & %s; cf = x != 0; of = x == %s;\n", a, mask_of(bits),
                    mask_of(bits), sign_of(bits));
            zps("r", bits);
            write_op(x, 0, bits, "r");
            fprintf(out, "    }\n");
            break;
        case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: {
            read_op(x, 0, a, sizeof(a));
            const int dec_ = in->mnemonic == ZYDIS_MNEMONIC_DEC;
            fprintf(out, "    { uint64_t x = %s & %s, r = (x %c 1) & %s; of = %s == %s;\n", a, mask_of(bits), dec_ ? '-' : '+',
                    mask_of(bits), dec_ ? "x" : "r", sign_of(bits));
            zps("r", bits);
            write_op(x, 0, bits, "r");
            fprintf(out, "    }\n");
            break;
        }
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: {
            read_op(x, 0, a, sizeof(a));
            if (in->operand_count_visible > 1) read_op(x, 1, b, sizeof(b)); else snprintf(b, sizeof(b), "1");
            fprintf(out, "    { uint64_t x = %s & %s; unsigned n = (unsigned)(%s) & %s;\n", a, mask_of(bits), b, bits == 64 ? "63" : "31");
            fprintf(out, "      if (n) { uint64_t r;\n");
            if (in->mnemonic == ZYDIS_MNEMONIC_SHL)
                fprintf(out, "        r = (n >= %u ? 0 : (x << n)) & %s; cf = n <= %u ? ((x >> (%u - n)) & 1) : 0; of = (((r & %s) != 0) != cf);\n",
                        bits, mask_of(bits), bits, bits, sign_of(bits));
            else if (in->mnemonic == ZYDIS_MNEMONIC_SHR)
                fprintf(out, "        r = n >= %u ? 0 : (x >> n); cf = n <= %u ? ((x >> (n - 1)) & 1) : 0; of = (x & %s) != 0;\n",
                        bits, bits, sign_of(bits));
            else
                fprintf(out, "        int64_t sx = (int64_t)(x << (64 - %u)) >> (64 - %u); r = (uint64_t)(sx >> (n >= %u ? %u : n)) & %s; "
                             "cf = (uint64_t)(sx >> (n >= %u ? %u : n - 1)) & 1; of = 0;\n",
                        bits, bits, bits, bits - 1, mask_of(bits), bits, bits - 1);
            zps("r", bits);
            write_op(x, 0, bits, "r");
            fprintf(out, "      }\n    }\n");
            break;
        }
        case ZYDIS_MNEMONIC_IMUL: {
            if (in->operand_count_visible == 3) { read_op(x, 1, a, sizeof(a)); read_op(x, 2, b, sizeof(b)); }
            else { read_op(x, 0, a, sizeof(a)); read_op(x, 1, b, sizeof(b)); }
            fprintf(out, "    { __int128 p = (__int128)(int%u_t)(%s) * (__int128)(int%u_t)(%s); uint64_t r = (uint64_t)p & %s;\n",
                    bits, a, bits, b, mask_of(bits));
            fprintf(out, "      cf = of = (p != (__int128)(int%u_t)r);\n", bits);
            zps("r", bits);
            write_op(x, 0, bits, "r");
            fprintf(out, "    }\n");
            break;
        }
        default:
            if (in->meta.category == ZYDIS_CATEGORY_COND_BR) {
                fprintf(out, "    if (%s) goto L%llx;\n", cond_expr(in->mnemonic),
                        (unsigned long long)(x->addr + in->length + (uint64_t)o[0].imm.value.s));
            } else if (in->meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
                uint64_t t = x->addr + in->length + (uint64_t)o[0].imm.value.s;
                int inside = 0;
                for (int j = 0; j < n; ++j) if (xs[j].addr == t) { inside = 1; break; }
                if (inside) fprintf(out, "    goto L%llx;\n", (unsigned long long)t);
                else fprintf(out, "    L_%llx(c); return;\n", (unsigned long long)t); /* tail call */
            } else if (in->meta.category == ZYDIS_CATEGORY_CALL) {
                uint64_t t = x->addr + in->length + (uint64_t)o[0].imm.value.s;
                fprintf(out, "    c->rsp -= 8; *(volatile uint64_t *)(uintptr_t)c->rsp = 0x%llxull; L_%llx(c);\n",
                        (unsigned long long)(IMAGE_BASE + x->addr + in->length), (unsigned long long)t);
            } else if (in->meta.category == ZYDIS_CATEGORY_CMOV) {
                read_op(x, 1, a, sizeof(a)); /* the source is read (and may fault) either way */
                fprintf(out, "    { uint64_t v = %s;\n", a);
                fprintf(out, "    if (%s) {\n", cond_expr(in->mnemonic));
                write_op(x, 0, bits, "v");
                fprintf(out, "    } else {\n");
                if (bits == 32) write_op(x, 0, 32, cname(gpr_index(o[0].reg.value)));
                fprintf(out, "    } }\n");
            } else if (in->meta.category == ZYDIS_CATEGORY_SETCC) {
                snprintf(expr, sizeof(expr), "(uint64_t)(%s)", cond_expr(in->mnemonic));
                write_op(x, 0, 8, expr);
            } else {
                return -1;
            }
        }
    }
    fprintf(out, "    return;\n}\n\n");
    return 1;
}

/* ---- reference runs ---- */
static sigjmp_buf jump;
static void on_fault(int sig) { siglongjmp(jump, sig); }
static uint64_t rng = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t arg(void) {
    switch (rnd() % 6) {
    case 0: return rnd() % 16;
    case 1: return (uint64_t)(int64_t)(int32_t)rnd();
    default: return DATA_BASE + 4096 + ((rnd() % (DATA_SIZE - 8192)) & ~7ull); /* a pointer into the buffer */
    }
}
static uint32_t flt(void) {
    static const uint32_t sp[] = {0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x00000001u};
    if (rnd() % 10 == 0) return sp[rnd() % 8];
    float f = (float)((int64_t)(rnd() % 20001) - 10000) / 64.0f;
    uint32_t u; memcpy(&u, &f, 4); return u;
}
static uint64_t hash(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 1099511628211ull; }
    return h;
}
/* Calls fn with six register and four stack arguments, xmm0-7 from a[12..27], every other
 * register zero (as the lifted code starts), and stores xmm0 after the call to xout. */
uint64_t bb_call6(uint64_t fn, const uint64_t *a, uint64_t *xout);
__asm__(".text\n.p2align 4\n_bb_call6:\n"
        "    pushq %rbx\n    pushq %rbp\n    pushq %r12\n    pushq %r13\n    pushq %r14\n    pushq %r15\n"
        "    subq $56, %rsp\n"
        "    movq %rdx, 32(%rsp)\n"
        "    movq %rdi, %r11\n    movq %rsi, %rax\n"
        "    movq 48(%rax), %rdi\n    movq %rdi, 0(%rsp)\n    movq 56(%rax), %rdi\n    movq %rdi, 8(%rsp)\n"
        "    movq 64(%rax), %rdi\n    movq %rdi, 16(%rsp)\n    movq 72(%rax), %rdi\n    movq %rdi, 24(%rsp)\n"
        "    vmovdqu 96(%rax), %xmm0\n    vmovdqu 112(%rax), %xmm1\n    vmovdqu 128(%rax), %xmm2\n    vmovdqu 144(%rax), %xmm3\n"
        "    vmovdqu 160(%rax), %xmm4\n    vmovdqu 176(%rax), %xmm5\n    vmovdqu 192(%rax), %xmm6\n    vmovdqu 208(%rax), %xmm7\n"
        "    vpxor %xmm8, %xmm8, %xmm8\n    vpxor %xmm9, %xmm9, %xmm9\n    vpxor %xmm10, %xmm10, %xmm10\n    vpxor %xmm11, %xmm11, %xmm11\n"
        "    vpxor %xmm12, %xmm12, %xmm12\n    vpxor %xmm13, %xmm13, %xmm13\n    vpxor %xmm14, %xmm14, %xmm14\n    vpxor %xmm15, %xmm15, %xmm15\n"
        "    movq 0(%rax), %rdi\n    movq 8(%rax), %rsi\n    movq 16(%rax), %rdx\n"
        "    movq 24(%rax), %rcx\n    movq 32(%rax), %r8\n    movq 40(%rax), %r9\n"
        "    xorl %eax, %eax\n    xorl %ebx, %ebx\n    xorl %ebp, %ebp\n    xorl %r10d, %r10d\n"
        "    xorl %r12d, %r12d\n    xorl %r13d, %r13d\n    xorl %r14d, %r14d\n    xorl %r15d, %r15d\n"
        "    callq *%r11\n"
        "    movq 32(%rsp), %rdx\n    vmovdqu %xmm0, (%rdx)\n    vzeroupper\n"
        "    addq $56, %rsp\n"
        "    popq %r15\n    popq %r14\n    popq %r13\n    popq %r12\n    popq %rbp\n    popq %rbx\n"
        "    retq\n");

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s image.bin funcs.txt outdir [max]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    img = malloc(size);
    if (fread(img, 1, size, f) != size) return 1;
    fclose(f);
    /* the image in place (code runs there) and the data buffer, at fixed addresses */
    uint8_t *mapped = mmap((void *)IMAGE_BASE, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    uint8_t *data = mmap((void *)DATA_BASE, DATA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mapped != (uint8_t *)IMAGE_BASE || data != (uint8_t *)DATA_BASE) { fprintf(stderr, "fixed mappings failed\n"); return 1; }
    memcpy(mapped, img, size);
    /* code executable; everything else read-only: a function that writes the game's globals
     * faults and is left out (the arm64 side could not be kept in step with it) */
    mprotect(mapped, 0x2bc0000, PROT_READ | PROT_EXEC);
    mprotect(mapped + 0x2bc0000, ((size - 0x2bc0000) + 4095) & ~4095ull, PROT_READ);
    uint8_t *pristine = malloc(DATA_SIZE);
    for (size_t i = 0; i < DATA_SIZE; i += 8) {
        uint64_t v = (rnd() % 3) ? (rnd() & 0xffff) : rnd();
        memcpy(pristine + i, &v, 8);
    }
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    char path[1024];
    snprintf(path, sizeof(path), "%s/lifted4.c", argv[3]);
    FILE *file_out = fopen(path, "w");
    fprintf(file_out, "/* Generated by tools/bbrecomp/proto/lift4.c from the game image: never commit. */\n"
                      "#include <stdint.h>\n\n");
    snprintf(path, sizeof(path), "%s/reference4.bin", argv[3]);
    FILE *ref = fopen(path, "wb");
    fwrite(pristine, 1, DATA_SIZE, ref);
    signal(SIGSEGV, on_fault); signal(SIGBUS, on_fault); signal(SIGILL, on_fault); signal(SIGFPE, on_fault);
    signal(SIGALRM, on_fault); /* a case running over 50 ms (a loop on a random count) is left out */

    /* Pass 1: every function that fits the subset, with its call / tail-jump targets. */
    typedef struct { uint64_t start, len; uint32_t first_target, ntargets; int alive; } Fun;
    Fun *fun = calloc(200000, sizeof(Fun));
    uint64_t *targets = malloc(4000000 * sizeof(uint64_t));
    size_t nfun = 0, ntargets = 0;
    FILE *fl = fopen(argv[2], "r");
    unsigned long long s, l;
    static Insn xs[MAX_INSNS];
    while (fscanf(fl, "%llx %llx", &s, &l) == 2) {
        if (l > 8192 || s >= 0x2bc0000) continue;
        int n = 0, ok = 1, has_end = 0;
        size_t t0 = ntargets;
        for (uint64_t a = s; a < s + l;) {
            if (n == MAX_INSNS) { ok = 0; break; }
            Insn *x = &xs[n];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, img + a, s + l - a, &x->in, x->ops))) break;
            x->addr = a;
            if (x->in.mnemonic == ZYDIS_MNEMONIC_INT3 && has_end) break;
            if (!supported(x)) { ok = 0; break; }
            if (x->in.mnemonic == ZYDIS_MNEMONIC_RET || x->in.meta.category == ZYDIS_CATEGORY_UNCOND_BR) has_end = 1;
            a += x->in.length;
            ++n;
        }
        if (!ok || !has_end || n < 2) { ntargets = t0; continue; }
        for (int i = 0; i < n && ok; ++i) {
            const Insn *x = &xs[i];
            const int cat = x->in.meta.category;
            if (cat != ZYDIS_CATEGORY_COND_BR && cat != ZYDIS_CATEGORY_UNCOND_BR && cat != ZYDIS_CATEGORY_CALL) continue;
            uint64_t t = x->addr + x->in.length + (uint64_t)x->ops[0].imm.value.s;
            int inside = 0;
            for (int j = 0; j < n; ++j) if (xs[j].addr == t) { inside = 1; break; }
            if (cat == ZYDIS_CATEGORY_COND_BR) { ok = inside; continue; }
            if (cat == ZYDIS_CATEGORY_UNCOND_BR && inside) continue;
            targets[ntargets++] = t; /* call or tail jump: must be a lifted function's start */
        }
        if (!ok) { ntargets = t0; continue; }
        fun[nfun++] = (Fun){s, l, (uint32_t)t0, (uint32_t)(ntargets - t0), 1};
    }
    fclose(fl);
    /* Pass 2: drop functions whose targets are not (still) accepted, until nothing changes. */
    int changed = 1, rounds = 0;
    while (changed) {
        changed = 0;
        ++rounds;
        for (size_t i = 0; i < nfun; ++i) {
            if (!fun[i].alive) continue;
            for (uint32_t k = 0; k < fun[i].ntargets; ++k) {
                uint64_t t = targets[fun[i].first_target + k];
                size_t lo = 0, hi = nfun; /* fun[] is sorted by start (funcs.txt is) */
                while (lo < hi) { size_t mid = (lo + hi) / 2; if (fun[mid].start < t) lo = mid + 1; else hi = mid; }
                if (lo == nfun || fun[lo].start != t || !fun[lo].alive) { fun[i].alive = 0; changed = 1; break; }
            }
        }
    }
    size_t alive = 0, callers = 0;
    for (size_t i = 0; i < nfun; ++i) if (fun[i].alive) { ++alive; callers += fun[i].ntargets > 0; }
    fprintf(stderr, "%zu functions fit the subset, %zu after removing those that call others (%d rounds), %zu of them call\n",
            nfun, alive, rounds, callers);
    /* Pass 3: emit every accepted function (callers need their callees), test each. */
    fputs(simd_prelude, file_out);
    fprintf(file_out, "typedef struct { uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15; uint32_t v[16][8]; } Ctx;\n");
    for (size_t i = 0; i < nfun; ++i)
        if (fun[i].alive) fprintf(file_out, "void L_%llx(Ctx *c);\n", (unsigned long long)fun[i].start);
    fprintf(file_out, "\n");
    size_t emitted = 0, tested = 0, faulted = 0, tested_callers = 0;
    uint64_t *addrs = malloc(200000 * sizeof(uint64_t));
    for (size_t i = 0; i < nfun; ++i) {
        if (!fun[i].alive) continue;
        s = fun[i].start;
        l = fun[i].len;
        int n = 0, has_end = 0;
        for (uint64_t a = s; a < s + l && n < MAX_INSNS;) {
            Insn *x = &xs[n];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, img + a, s + l - a, &x->in, x->ops))) break;
            x->addr = a;
            if (x->in.mnemonic == ZYDIS_MNEMONIC_INT3 && has_end) break;
            if (x->in.mnemonic == ZYDIS_MNEMONIC_RET || x->in.meta.category == ZYDIS_CATEGORY_UNCOND_BR) has_end = 1;
            a += x->in.length;
            ++n;
        }
        out = file_out;
        if (emit_function(s, xs, n) <= 0) { fprintf(stderr, "emit failed for %llx\n", s); return 1; }
        addrs[emitted++] = s;
        const uint64_t fn = IMAGE_BASE + s;
        uint64_t results[CASES][30]; /* args 0-9, rax, hash, xmm0-7 in (12-27), xmm0 out (28-29) */
        int bad = 0;
        for (int k = 0; k < CASES && !bad; ++k) {
            for (int j = 0; j < 10; ++j) results[k][j] = arg();
            for (int j = 12; j < 28; ++j) results[k][j] = (uint64_t)flt() | ((uint64_t)flt() << 32);
            memcpy(data, pristine, DATA_SIZE);
            struct itimerval limit = {{0, 0}, {0, 50000}}, off = {{0, 0}, {0, 0}};
            if (sigsetjmp(jump, 1)) { setitimer(ITIMER_REAL, &off, NULL); bad = 1; break; }
            setitimer(ITIMER_REAL, &limit, NULL);
            results[k][10] = bb_call6(fn, results[k], &results[k][28]);
            setitimer(ITIMER_REAL, &off, NULL);
            results[k][11] = hash(data, DATA_SIZE);
        }
        if (bad) { ++faulted; continue; }
        uint64_t addr = s;
        fwrite(&addr, 8, 1, ref);
        fwrite(results, sizeof(results), 1, ref);
        ++tested;
        tested_callers += fun[i].ntargets > 0;
    }
    fprintf(file_out, "typedef void (*LiftedFn)(Ctx *);\n");
    fprintf(file_out, "const struct { uint64_t addr; LiftedFn fn; } lifted[] = {\n");
    for (size_t i = 0; i < emitted; ++i) fprintf(file_out, "    {0x%llx, L_%llx},\n", (unsigned long long)addrs[i], (unsigned long long)addrs[i]);
    fprintf(file_out, "};\nconst unsigned lifted_count = %zu;\n", emitted);
    fclose(file_out);
    fclose(ref);
    fprintf(stderr, "%zu functions lifted, %zu tested against the original (%zu of them call others), %zu faulted on random inputs\n",
            emitted, tested, tested_callers, faulted);
    return 0;
}
