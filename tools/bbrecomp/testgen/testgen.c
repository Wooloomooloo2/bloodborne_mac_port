/* Instruction test vectors for the static recompiler (docs/STATIC_RECOMPILATION_PLAN.md, M1).
 *
 * For every distinct instruction *form* in the game's functions (mnemonic, encoding, operand
 * kinds and sizes, prefixes), one occurrence is re-encoded with its memory operand moved to a
 * scratch buffer ([base + 64], base a register the instruction does not use), then executed on
 * random inputs: all 16 GPRs, RFLAGS, the x87/SSE state (FXSAVE), the upper YMM halves, MXCSR
 * and 256 bytes of memory. Inputs and outputs are written to a file. Run it on an x86-64 CPU or
 * under Rosetta 2 while that exists: the recompiler's semantics library must reproduce every
 * output bit for bit.
 *
 * Nothing from the game is stored: each case is the re-encoded instruction (opcode, registers,
 * a random immediate) and random register/memory values. Forms that cannot be tested in
 * isolation are listed and skipped: control flow, stack operations, system instructions.
 *
 * usage: testgen image.bin funcs.txt out.vec [cases per form] [seed]
 *        (image.bin and funcs.txt from tools/recomp_survey/survey.sh)
 * Output: "BBVEC1\0\0", u32 form count, then per form: u16 key length, key bytes, u32 case
 * count, cases (struct Case below). forms.txt next to the output lists every form.
 */
#include <Zydis/Zydis.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if !defined(__x86_64__)
#error "testgen runs the instructions it tests: build it for x86-64 (arch -x86_64 under Rosetta 2)"
#endif

typedef struct __attribute__((aligned(64))) State {
    uint64_t gpr[16]; /* rax rcx rdx rbx rsp rbp rsi rdi r8..r15 (rsp not loaded) */
    uint64_t rflags;
    uint64_t pad[7];
    uint8_t fx[512]; /* FXSAVE: x87, MXCSR, xmm0-15 (offset 192, 16-byte aligned) */
    uint8_t ymm_hi[16][16];                       /* upper halves of ymm0-15 */
    uint8_t mem[256] __attribute__((aligned(64)));
} State;

typedef struct Case {
    uint8_t bytes[15];
    uint8_t length;
    uint8_t faulted; /* signal number when the instruction trapped (SIGFPE: #DE etc.) */
    uint8_t pad[7];
    State in, out;
} Case;

/* ---- Harness: load a State, run code (the instruction + ret), store a State. ---- */
static uint8_t g_mem[256] __attribute__((aligned(64)));
void *g_code;
State *g_in, *g_out;
uint64_t g_saved_rsp;

#define OFF_FLAGS 128
#define OFF_FX 192
#define OFF_YMMHI (192 + 512)
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _bb_run_case\n"
    "_bb_run_case:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $8, %rsp\n"
    "    movq %rsp, _g_saved_rsp(%rip)\n"
    "    movq _g_in(%rip), %rdi\n"
    "    fxrstor64 192(%rdi)\n"
    "    vinsertf128 $1, 704(%rdi), %ymm0, %ymm0\n"
    "    vinsertf128 $1, 720(%rdi), %ymm1, %ymm1\n"
    "    vinsertf128 $1, 736(%rdi), %ymm2, %ymm2\n"
    "    vinsertf128 $1, 752(%rdi), %ymm3, %ymm3\n"
    "    vinsertf128 $1, 768(%rdi), %ymm4, %ymm4\n"
    "    vinsertf128 $1, 784(%rdi), %ymm5, %ymm5\n"
    "    vinsertf128 $1, 800(%rdi), %ymm6, %ymm6\n"
    "    vinsertf128 $1, 816(%rdi), %ymm7, %ymm7\n"
    "    vinsertf128 $1, 832(%rdi), %ymm8, %ymm8\n"
    "    vinsertf128 $1, 848(%rdi), %ymm9, %ymm9\n"
    "    vinsertf128 $1, 864(%rdi), %ymm10, %ymm10\n"
    "    vinsertf128 $1, 880(%rdi), %ymm11, %ymm11\n"
    "    vinsertf128 $1, 896(%rdi), %ymm12, %ymm12\n"
    "    vinsertf128 $1, 912(%rdi), %ymm13, %ymm13\n"
    "    vinsertf128 $1, 928(%rdi), %ymm14, %ymm14\n"
    "    vinsertf128 $1, 944(%rdi), %ymm15, %ymm15\n"
    "    pushq 128(%rdi)\n"
    "    popfq\n"
    "    movq 0(%rdi), %rax\n"
    "    movq 8(%rdi), %rcx\n"
    "    movq 16(%rdi), %rdx\n"
    "    movq 24(%rdi), %rbx\n"
    "    movq 40(%rdi), %rbp\n"
    "    movq 48(%rdi), %rsi\n"
    "    movq 64(%rdi), %r8\n"
    "    movq 72(%rdi), %r9\n"
    "    movq 80(%rdi), %r10\n"
    "    movq 88(%rdi), %r11\n"
    "    movq 96(%rdi), %r12\n"
    "    movq 104(%rdi), %r13\n"
    "    movq 112(%rdi), %r14\n"
    "    movq 120(%rdi), %r15\n"
    "    movq 56(%rdi), %rdi\n"
    "    callq *_g_code(%rip)\n"
    "    pushq %rdi\n"
    "    movq _g_out(%rip), %rdi\n"
    "    movq %rax, 0(%rdi)\n"
    "    movq %rcx, 8(%rdi)\n"
    "    movq %rdx, 16(%rdi)\n"
    "    movq %rbx, 24(%rdi)\n"
    "    movq %rbp, 40(%rdi)\n"
    "    movq %rsi, 48(%rdi)\n"
    "    popq 56(%rdi)\n"
    "    movq %r8, 64(%rdi)\n"
    "    movq %r9, 72(%rdi)\n"
    "    movq %r10, 80(%rdi)\n"
    "    movq %r11, 88(%rdi)\n"
    "    movq %r12, 96(%rdi)\n"
    "    movq %r13, 104(%rdi)\n"
    "    movq %r14, 112(%rdi)\n"
    "    movq %r15, 120(%rdi)\n"
    "    pushfq\n"
    "    popq 128(%rdi)\n"
    "    fxsave64 192(%rdi)\n"
    "    vextractf128 $1, %ymm0, 704(%rdi)\n"
    "    vextractf128 $1, %ymm1, 720(%rdi)\n"
    "    vextractf128 $1, %ymm2, 736(%rdi)\n"
    "    vextractf128 $1, %ymm3, 752(%rdi)\n"
    "    vextractf128 $1, %ymm4, 768(%rdi)\n"
    "    vextractf128 $1, %ymm5, 784(%rdi)\n"
    "    vextractf128 $1, %ymm6, 800(%rdi)\n"
    "    vextractf128 $1, %ymm7, 816(%rdi)\n"
    "    vextractf128 $1, %ymm8, 832(%rdi)\n"
    "    vextractf128 $1, %ymm9, 848(%rdi)\n"
    "    vextractf128 $1, %ymm10, 864(%rdi)\n"
    "    vextractf128 $1, %ymm11, 880(%rdi)\n"
    "    vextractf128 $1, %ymm12, 896(%rdi)\n"
    "    vextractf128 $1, %ymm13, 912(%rdi)\n"
    "    vextractf128 $1, %ymm14, 928(%rdi)\n"
    "    vextractf128 $1, %ymm15, 944(%rdi)\n"
    "    cld\n"
    "    vzeroupper\n"
    "    movq _g_saved_rsp(%rip), %rsp\n"
    "    addq $8, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    retq\n");
void bb_run_case(void);
_Static_assert(__builtin_offsetof(State, rflags) == OFF_FLAGS, "flags offset");
_Static_assert(__builtin_offsetof(State, fx) == OFF_FX, "fx offset");
_Static_assert(__builtin_offsetof(State, ymm_hi) == OFF_YMMHI, "ymm offset");

/* ---- Faults: an instruction that traps (#DE, #UD, a bad address) ends its case. ---- */
static sigjmp_buf fault_jump;
static volatile sig_atomic_t fault_signal;
static void on_fault(int sig, siginfo_t *info, void *ctx) {
    (void)info;
    (void)ctx;
    fault_signal = sig;
    siglongjmp(fault_jump, 1);
}

/* ---- Random inputs with the special values instruction semantics trip over. ---- */
static uint64_t rng_state;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static uint64_t special64(void) {
    static const uint64_t s[] = {0, 1, ~0ull, 0x7fffffffffffffffull, 0x8000000000000000ull,
                                 0x80000000ull, 0x7fffffffull, 0xffffffffull, 0x100000000ull,
                                 0x8000ull, 0x80ull, 0xffull, 31, 32, 63, 64};
    switch (rnd() % 4) {
    case 0: return s[rnd() % (sizeof(s) / sizeof(s[0]))];
    case 1: return rnd() & 0xff;
    default: return rnd();
    }
}
static uint32_t special_f32(void) {
    static const uint32_t s[] = {0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000,
                                 0xff800000, 0x7fc00000, 0xffc00000, 0x7fa00000, 0x00000001,
                                 0x807fffff, 0x7f7fffff, 0x4b000000, 0x3f000000, 0x4f000000,
                                 0xcf000000};
    switch (rnd() % 4) {
    case 0: return s[rnd() % (sizeof(s) / sizeof(s[0]))];
    case 1: { float f = (float)((int64_t)(rnd() % 2001) - 1000) / 8.0f; uint32_t u; memcpy(&u, &f, 4); return u; }
    default: return (uint32_t)rnd();
    }
}
static void fill_vector(uint8_t *p, size_t bytes) {
    for (size_t i = 0; i < bytes; i += 4) {
        uint32_t v = (rnd() % 3) ? special_f32() : (uint32_t)rnd();
        memcpy(p + i, &v, 4);
    }
}

static void random_state(State *s, int string_op) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 16; ++i) s->gpr[i] = i == 4 ? 0 : special64(); /* rsp is not loaded */
    /* arithmetic flags random, DF only for string instructions, IF + reserved bit 1 set */
    s->rflags = 0x202 | (rnd() & 0x8d5);
    if (string_op && (rnd() & 1)) s->rflags |= 0x400;
    /* FXSAVE image: default x87 control word (0x37f), MXCSR mostly default, a few rounding /
     * FTZ / DAZ variants with all exceptions masked; empty x87 stack with random registers. */
    uint16_t fcw = 0x37f;
    if (rnd() % 8 == 0) fcw = (uint16_t)(0x37f | ((rnd() & 3) << 10) | ((rnd() & 3) << 8));
    memcpy(s->fx + 0, &fcw, 2);
    uint32_t mxcsr = 0x1f80;
    if (rnd() % 6 == 0) mxcsr |= (uint32_t)((rnd() & 3) << 13) | (uint32_t)(rnd() & 0x8040);
    memcpy(s->fx + 24, &mxcsr, 4);
    uint32_t mxcsr_mask = 0xffff;
    memcpy(s->fx + 28, &mxcsr_mask, 4);
    s->fx[4] = (rnd() % 4) ? 0xff : (uint8_t)rnd(); /* abridged tag word: mostly a full stack */
    for (int i = 0; i < 8; ++i) { /* st0-7: finite 80-bit values (random exponent near bias) */
        uint64_t mant = rnd() | (1ull << 63);
        uint16_t se = (uint16_t)((rnd() & 0x8000) | (0x3fff + (int)(rnd() % 64) - 32));
        if (rnd() % 8 == 0) se = (uint16_t)rnd(); /* sometimes anything */
        memcpy(s->fx + 32 + 16 * i, &mant, 8);
        memcpy(s->fx + 40 + 16 * i, &se, 2);
    }
    fill_vector(s->fx + 160, 256); /* xmm0-15 */
    fill_vector(&s->ymm_hi[0][0], sizeof(s->ymm_hi));
    fill_vector(s->mem, sizeof(s->mem));
}

/* ---- Forms ---- */
typedef struct Form {
    char key[160];
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    uint64_t count;
} Form;

static Form *forms;
static size_t nforms, capforms;

static void form_key(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, char *key, size_t n) {
    int len = snprintf(key, n, "%s/%s/%s", ZydisMnemonicGetString(in->mnemonic),
                       in->encoding == ZYDIS_INSTRUCTION_ENCODING_VEX ? "vex" : "legacy",
                       (in->attributes & ZYDIS_ATTRIB_HAS_LOCK) ? "lock" :
                       (in->attributes & ZYDIS_ATTRIB_HAS_REP) ? "rep" :
                       (in->attributes & ZYDIS_ATTRIB_HAS_REPE) ? "repe" :
                       (in->attributes & ZYDIS_ATTRIB_HAS_REPNE) ? "repne" : "-");
    for (int i = 0; i < in->operand_count_visible && len < (int)n; ++i) {
        const ZydisDecodedOperand *o = &ops[i];
        const char *kind = "?";
        char buf[32];
        switch (o->type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: {
            ZydisRegisterClass c = ZydisRegisterGetClass(o->reg.value);
            kind = c == ZYDIS_REGCLASS_GPR8 ? "r8" : c == ZYDIS_REGCLASS_GPR16 ? "r16" :
                   c == ZYDIS_REGCLASS_GPR32 ? "r32" : c == ZYDIS_REGCLASS_GPR64 ? "r64" :
                   c == ZYDIS_REGCLASS_XMM ? "xmm" : c == ZYDIS_REGCLASS_YMM ? "ymm" :
                   c == ZYDIS_REGCLASS_X87 ? "st" : "reg";
            /* fixed registers (shifts by cl, implicit-looking explicit ones) keep their name */
            if (o->encoding == ZYDIS_OPERAND_ENCODING_NONE) {
                snprintf(buf, sizeof(buf), "%s", ZydisRegisterGetString(o->reg.value));
                kind = buf;
            }
            break;
        }
        case ZYDIS_OPERAND_TYPE_MEMORY:
            snprintf(buf, sizeof(buf), "%s%u", in->mnemonic == ZYDIS_MNEMONIC_LEA ? "agen" : "m", o->size);
            kind = buf;
            break;
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            snprintf(buf, sizeof(buf), "i%u", o->size);
            kind = buf;
            break;
        default: break;
        }
        len += snprintf(key + len, n - (size_t)len, ",%s", kind);
    }
}

/* Not testable in isolation (control flow, stack, system, non-deterministic). */
static int skipped(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops) {
    switch (in->meta.category) {
    case ZYDIS_CATEGORY_CALL: case ZYDIS_CATEGORY_COND_BR: case ZYDIS_CATEGORY_UNCOND_BR:
    case ZYDIS_CATEGORY_RET: case ZYDIS_CATEGORY_SYSCALL: case ZYDIS_CATEGORY_SYSTEM:
    case ZYDIS_CATEGORY_INTERRUPT: case ZYDIS_CATEGORY_PUSH: case ZYDIS_CATEGORY_POP:
        return 1;
    default: break;
    }
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_LEAVE: case ZYDIS_MNEMONIC_ENTER: case ZYDIS_MNEMONIC_PUSHFQ:
    case ZYDIS_MNEMONIC_POPFQ: case ZYDIS_MNEMONIC_RDTSC: case ZYDIS_MNEMONIC_RDTSCP:
    case ZYDIS_MNEMONIC_CPUID: case ZYDIS_MNEMONIC_UD2: case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_HLT: case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_PAUSE:
    case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHT0: case ZYDIS_MNEMONIC_PREFETCHT1:
    case ZYDIS_MNEMONIC_PREFETCHT2: case ZYDIS_MNEMONIC_PREFETCHW: case ZYDIS_MNEMONIC_LFENCE:
    case ZYDIS_MNEMONIC_SFENCE: case ZYDIS_MNEMONIC_MFENCE: case ZYDIS_MNEMONIC_VZEROUPPER:
    case ZYDIS_MNEMONIC_FXSAVE: case ZYDIS_MNEMONIC_FXSAVE64: case ZYDIS_MNEMONIC_FXRSTOR:
    case ZYDIS_MNEMONIC_FXRSTOR64: case ZYDIS_MNEMONIC_XSAVE: case ZYDIS_MNEMONIC_XRSTOR:
    case ZYDIS_MNEMONIC_LDMXCSR: case ZYDIS_MNEMONIC_FLDENV: case ZYDIS_MNEMONIC_FNSTENV:
    /* privileged / port I/O: only seen where linear decoding runs into data */
    case ZYDIS_MNEMONIC_CLI: case ZYDIS_MNEMONIC_STI: case ZYDIS_MNEMONIC_IN: case ZYDIS_MNEMONIC_OUT:
    case ZYDIS_MNEMONIC_INSB: case ZYDIS_MNEMONIC_INSW: case ZYDIS_MNEMONIC_INSD:
    case ZYDIS_MNEMONIC_OUTSB: case ZYDIS_MNEMONIC_OUTSW: case ZYDIS_MNEMONIC_OUTSD:
    case ZYDIS_MNEMONIC_UD0: case ZYDIS_MNEMONIC_UD1:
        return 1;
    default: break;
    }
    /* anything writing rsp, or reading memory through rsp/rip implicitly */
    for (int i = 0; i < in->operand_count; ++i) {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            (ops[i].reg.value == ZYDIS_REGISTER_RSP || ops[i].reg.value == ZYDIS_REGISTER_ESP ||
             ops[i].reg.value == ZYDIS_REGISTER_SP))
            return 1;
    }
    return 0;
}

/* Open-addressing table of form indices by key. */
#define TABLE_SIZE 65536
static int32_t table[TABLE_SIZE];
static uint32_t hash_key(const char *k) {
    uint32_t h = 2166136261u;
    while (*k) h = (h ^ (uint8_t)*k++) * 16777619u;
    return h;
}
static Form *find_or_add(const char *key) {
    uint32_t h = hash_key(key) & (TABLE_SIZE - 1);
    while (table[h]) {
        Form *f = &forms[table[h] - 1];
        if (!strcmp(f->key, key)) return f;
        h = (h + 1) & (TABLE_SIZE - 1);
    }
    if (nforms == capforms) {
        capforms = capforms ? capforms * 2 : 1024;
        forms = realloc(forms, capforms * sizeof(Form));
    }
    Form *f = &forms[nforms++];
    memset(f, 0, sizeof(*f));
    snprintf(f->key, sizeof(f->key), "%s", key);
    table[h] = (int32_t)nforms;
    return f;
}

static int uses_reg64(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, ZydisRegister r64) {
    for (int i = 0; i < in->operand_count; ++i) {
        const ZydisDecodedOperand *o = &ops[i];
        if (o->type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, o->reg.value) == r64)
            return 1;
        if (o->type == ZYDIS_OPERAND_TYPE_MEMORY &&
            (o->mem.base == r64 || o->mem.index == r64))
            return 1;
    }
    return 0;
}

/* Re-encodes form f with memory at [base + 64] and a random immediate; returns the length. */
static int encode_case(const Form *f, uint8_t *out, int *base_index) {
    ZydisEncoderRequest req;
    if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(&f->in, f->ops,
                                                                     f->in.operand_count_visible, &req)))
        return 0;
    static const ZydisRegister candidates[] = {ZYDIS_REGISTER_R15, ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R13,
                                               ZYDIS_REGISTER_R12, ZYDIS_REGISTER_RBX, ZYDIS_REGISTER_R11};
    ZydisRegister base = ZYDIS_REGISTER_NONE;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
        if (!uses_reg64(&f->in, f->ops, candidates[i])) { base = candidates[i]; break; }
    *base_index = -1;
    for (int i = 0; i < req.operand_count; ++i) {
        ZydisEncoderOperand *o = &req.operands[i];
        if (o->type == ZYDIS_OPERAND_TYPE_MEMORY && f->in.meta.category == ZYDIS_CATEGORY_STRINGOP) {
            continue; /* rsi/rdi point into the buffer instead (main) */
        } else if (o->type == ZYDIS_OPERAND_TYPE_MEMORY && f->in.mnemonic != ZYDIS_MNEMONIC_LEA) {
            if (base == ZYDIS_REGISTER_NONE) return 0;
            o->mem.base = base;
            o->mem.index = ZYDIS_REGISTER_NONE;
            o->mem.scale = 0;
            o->mem.displacement = 64;
            *base_index = (int)(base - ZYDIS_REGISTER_RAX);
        } else if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) { /* lea: address arithmetic only */
            if (o->mem.base == ZYDIS_REGISTER_RIP) o->mem.base = ZYDIS_REGISTER_RBX;
            o->mem.displacement = (int32_t)rnd();
        } else if (o->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            const ZydisDecodedOperand *d = &f->ops[i];
            uint64_t v = special64();
            if (d->size < 64) v &= (1ull << d->size) - 1;
            if (d->imm.is_signed && d->size < 64 && (v >> (d->size - 1)) & 1)
                v |= ~0ull << d->size; /* sign-extend: the encoder takes the value */
            o->imm.u = v;
        }
    }
    req.prefixes &= ~(ZydisInstructionAttributes)(ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS);
    ZyanUSize len = ZYDIS_MAX_INSTRUCTION_LENGTH;
    if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&req, out, &len))) return 0;
    return (int)len;
}

static int is_string_op(const ZydisDecodedInstruction *in) {
    return in->meta.category == ZYDIS_CATEGORY_STRINGOP;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s image.bin funcs.txt out.vec [cases per form] [seed]\n", argv[0]);
        return 2;
    }
    int cases = argc > 4 ? atoi(argv[4]) : 32;
    rng_state = argc > 5 ? strtoull(argv[5], NULL, 0) : 0x9e3779b97f4a7c15ull;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc(size);
    if (fread(img, 1, size, f) != size) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    /* 1. Forms, by recursive-descent-free linear decode of each function up to its first
     *    undecodable byte (switch tables sit at function ends). */
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    FILE *fl = fopen(argv[2], "r");
    if (!fl) { perror(argv[2]); return 1; }
    unsigned long long s, r;
    uint64_t total = 0, skipped_count = 0;
    while (fscanf(fl, "%llx %llx", &s, &r) == 2) {
        for (uint64_t a = s; a < s + r && a < size;) {
            ZydisDecodedInstruction in;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, img + a, s + r - a, &in, ops))) break;
            a += in.length;
            ++total;
            if (skipped(&in, ops)) { ++skipped_count; continue; }
            char key[160];
            form_key(&in, ops, key, sizeof(key));
            Form *fm = find_or_add(key);
            if (!fm->count++) { fm->in = in; memcpy(fm->ops, ops, sizeof(ops)); }
        }
    }
    fclose(fl);
    fprintf(stderr, "%llu instructions, %llu skipped (control flow, stack, system), %zu forms\n",
            (unsigned long long)total, (unsigned long long)skipped_count, nforms);

    /* 2. Run. */
    /* Every case gets fresh code bytes at an address never used before: Rosetta 2 keeps
     * translations by address, and a rewritten page could run an earlier case's instruction. */
    const size_t code_region = (size_t)(nforms + 1) * (size_t)cases * 32 + 4096;
    uint8_t *region = mmap(NULL, code_region, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    size_t code_offset = 0;
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);

    FILE *out = fopen(argv[3], "wb");
    char list_path[1024];
    snprintf(list_path, sizeof(list_path), "%s.forms.txt", argv[3]);
    FILE *list = fopen(list_path, "w");
    fwrite("BBVEC1\0\0", 1, 8, out);
    uint32_t nf = (uint32_t)nforms;
    fwrite(&nf, 4, 1, out);
    static State in_state, out_state;
    Case c;
    uint64_t faults = 0, unencodable = 0;
    for (size_t i = 0; i < nforms; ++i) {
        Form *fm = &forms[i];
        uint16_t klen = (uint16_t)strlen(fm->key);
        fwrite(&klen, 2, 1, out);
        fwrite(fm->key, 1, klen, out);
        uint32_t written = 0;
        long count_pos = ftell(out);
        fwrite(&written, 4, 1, out);
        int form_faults = 0;
        for (int k = 0; k < cases; ++k) {
            memset(&c, 0, sizeof(c));
            int base_index;
            int len = encode_case(fm, c.bytes, &base_index);
            if (!len) { ++unencodable; break; }
            c.length = (uint8_t)len;
            random_state(&in_state, is_string_op(&fm->in));
            if (base_index >= 0) in_state.gpr[base_index] = (uint64_t)(uintptr_t)g_mem;
            /* implicit memory operands: their base register points into the buffer */
            for (int o = fm->in.operand_count_visible; o < fm->in.operand_count; ++o) {
                const ZydisDecodedOperand *op = &fm->ops[o];
                if (op->type != ZYDIS_OPERAND_TYPE_MEMORY || op->mem.base == ZYDIS_REGISTER_NONE) continue;
                ZydisRegister b = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, op->mem.base);
                if (b >= ZYDIS_REGISTER_RAX && b <= ZYDIS_REGISTER_R15 && b != ZYDIS_REGISTER_RSP)
                    in_state.gpr[b - ZYDIS_REGISTER_RAX] =
                        (uint64_t)(uintptr_t)g_mem + (fm->in.mnemonic == ZYDIS_MNEMONIC_XLAT ? 0 : 64);
            }
            /* div/idiv: three cases in four get a quotient that fits (else #DE) */
            if ((fm->in.mnemonic == ZYDIS_MNEMONIC_DIV || fm->in.mnemonic == ZYDIS_MNEMONIC_IDIV) && k % 4) {
                const ZydisDecodedOperand *d = &fm->ops[0];
                unsigned bits = d->size;
                int sgn = fm->in.mnemonic == ZYDIS_MNEMONIC_IDIV;
                uint64_t lo_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
                uint64_t lo = in_state.gpr[0] & lo_mask;
                uint64_t hi = sgn && ((lo >> (bits - 1)) & 1) ? lo_mask : 0;
                if (bits == 8) {
                    in_state.gpr[0] = (in_state.gpr[0] & ~0xffffull) | (hi << 8) | lo;
                } else {
                    in_state.gpr[2] = (in_state.gpr[2] & ~lo_mask) | hi;
                }
                uint64_t divisor = (rnd() % 1000) + 1;
                if (d->type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    ZydisRegister b = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, d->reg.value);
                    int idx = (int)(b - ZYDIS_REGISTER_RAX);
                    int high8 = d->reg.value >= ZYDIS_REGISTER_AH && d->reg.value <= ZYDIS_REGISTER_BH;
                    if (high8) idx = (int)(d->reg.value - ZYDIS_REGISTER_AH);
                    uint64_t shift = high8 ? 8 : 0;
                    if (idx != 0 && idx != 2)
                        in_state.gpr[idx] = (in_state.gpr[idx] & ~(lo_mask << shift)) | ((divisor & lo_mask) << shift);
                } else {
                    memcpy(in_state.mem + 64, &divisor, bits / 8);
                }
            }
            if (is_string_op(&fm->in)) { /* rsi/rdi into the buffer, short counts */
                in_state.gpr[6] = (uint64_t)(uintptr_t)(g_mem + 96);
                in_state.gpr[7] = (uint64_t)(uintptr_t)(g_mem + 128);
                in_state.gpr[1] = rnd() % 9;
            }
            uint8_t *code = region + code_offset;
            code_offset += 32;
            uint8_t *code_page = (uint8_t *)((uintptr_t)code & ~(uintptr_t)4095);
            mprotect(code_page, 4096, PROT_READ | PROT_WRITE);
            memcpy(code, c.bytes, (size_t)len);
            code[len] = 0xc3; /* ret */
            mprotect(code_page, 4096, PROT_READ | PROT_EXEC);
            memcpy(g_mem, in_state.mem, sizeof(g_mem));
            memset(&out_state, 0, sizeof(out_state));
            g_code = code;
            g_in = &in_state;
            g_out = &out_state;
            fault_signal = 0;
            if (!sigsetjmp(fault_jump, 1)) {
                bb_run_case();
            } else {
                c.faulted = (uint8_t)fault_signal;
                ++faults;
                ++form_faults;
                __asm__ volatile("cld; vzeroupper");
            }
            memcpy(out_state.mem, g_mem, sizeof(g_mem));
            /* addresses of the scratch buffer are not part of the semantics: store offsets */
            for (int g = 0; g < 16; ++g) {
                if (in_state.gpr[g] - (uint64_t)(uintptr_t)g_mem < 256) in_state.gpr[g] -= (uint64_t)(uintptr_t)g_mem;
                if (out_state.gpr[g] - (uint64_t)(uintptr_t)g_mem < 256) out_state.gpr[g] -= (uint64_t)(uintptr_t)g_mem;
            }
            c.in = in_state;
            c.out = out_state;
            fwrite(&c, sizeof(c), 1, out);
            ++written;
        }
        long end = ftell(out);
        fseek(out, count_pos, SEEK_SET);
        fwrite(&written, 4, 1, out);
        fseek(out, end, SEEK_SET);
        fprintf(list, "%8llu  %-60s %u cases%s\n", (unsigned long long)fm->count, fm->key, written,
                form_faults ? " (some trapped)" : written ? "" : " (not encodable here)");
    }
    fclose(out);
    fclose(list);
    fprintf(stderr, "%zu forms written to %s (%llu cases trapped, %llu forms not re-encodable); list in %s\n",
            nforms, argv[3], (unsigned long long)faults, (unsigned long long)unencodable, list_path);
    return 0;
}
