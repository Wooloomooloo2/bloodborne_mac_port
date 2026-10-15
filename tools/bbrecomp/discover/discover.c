/* Code discovery for the static recompiler (docs/STATIC_RECOMPILATION_PLAN.md, M1): recursive
 * descent over the game image from every known entry point, with switch-table recovery.
 *
 * Roots: FDE function starts (funcs.txt), the entry point, module inits, exported libc functions
 * and relocation targets inside the code (function pointers, vtables, initializer arrays), all
 * from bootinfo.py. Block ends: ret, jmp, ud2/int3/hlt; conditional branches follow both ways;
 * calls continue after the call. "jmp reg" preceded by the compiler's switch sequence
 * (lea base,[rip+T]; movsxd idx,[base+i*4]; add idx,base; jmp idx) reads the 32-bit relative
 * table T, entries while they land inside the enclosing FDE function before the table.
 *
 * usage: discover image.bin funcs.txt roots.txt [out prefix]
 * Writes <prefix>.blocks (block start, length per line) and <prefix>.unresolved (indirect jumps
 * that are not switches), and prints coverage against the FDE ranges and the linear decode.
 */
#include <Zydis/Zydis.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *img;
static size_t size;
static uint8_t *insn_start; /* bitmap: decoded instruction starts */
static uint8_t *covered;    /* bitmap: bytes inside decoded instructions */
static uint8_t *block_seen; /* bitmap: block starts queued */
static ZydisDecoder dec;

typedef struct { uint64_t start, len; } Range;
static Range *funcs;
static size_t nfuncs;
static Range code_ranges[8];
static int ncode;

static uint64_t *stack;
static size_t nstack, capstack;

static uint64_t stat_blocks, stat_insns, stat_switch, stat_switch_entries, stat_unresolved_jmp,
    stat_icall, stat_bad_roots, stat_bad_decode, stat_tailcall_reg, stat_tail_reg, stat_tail_mem;

#define BIT(map, a) ((map)[(a) >> 3] & (1u << ((a) & 7)))
#define SET(map, a) ((map)[(a) >> 3] |= (uint8_t)(1u << ((a) & 7)))

static int in_code(uint64_t a) {
    for (int i = 0; i < ncode; ++i)
        if (a >= code_ranges[i].start && a < code_ranges[i].start + code_ranges[i].len) return 1;
    return 0;
}

static const Range *func_of(uint64_t a) {
    size_t lo = 0, hi = nfuncs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (funcs[mid].start <= a) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const Range *f = &funcs[lo - 1];
    return a < f->start + f->len ? f : NULL;
}

static void push(uint64_t a) {
    if (a >= size || !in_code(a) || BIT(block_seen, a)) return;
    SET(block_seen, a);
    if (nstack == capstack) {
        capstack = capstack ? capstack * 2 : 65536;
        stack = realloc(stack, capstack * sizeof(*stack));
    }
    stack[nstack++] = a;
}

static FILE *unresolved_out, *blocks_out, *switches_out;

/* Recent instructions of the block being decoded (switch pattern). */
typedef struct { uint64_t addr; ZydisDecodedInstruction in; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]; } Insn;
#define WINDOW 12

static int try_switch(const Insn *w, int n, uint64_t jmp_addr, ZydisRegister target_reg) {
    /* add target_reg, base ; movsxd target_reg, [base + idx*4] ; lea base, [rip + T] */
    ZydisRegister base = ZYDIS_REGISTER_NONE;
    uint64_t table = 0;
    int have_movsxd = 0;
    for (int i = n - 1; i >= 0; --i) {
        const Insn *x = &w[i];
        if (x->in.mnemonic == ZYDIS_MNEMONIC_ADD && x->ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            x->ops[0].reg.value == target_reg && x->ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            base == ZYDIS_REGISTER_NONE)
            base = x->ops[1].reg.value;
        else if (x->in.mnemonic == ZYDIS_MNEMONIC_MOVSXD && x->ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                 x->ops[1].mem.scale == 4 && base != ZYDIS_REGISTER_NONE && x->ops[1].mem.base == base)
            have_movsxd = 1;
        else if (x->in.mnemonic == ZYDIS_MNEMONIC_LEA && x->ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                 x->ops[0].reg.value == base && x->ops[1].mem.base == ZYDIS_REGISTER_RIP && have_movsxd) {
            table = x->addr + x->in.length + (uint64_t)x->ops[1].mem.disp.value;
            break;
        }
    }
    if (!table || table + 4 > size) return 0;
    const Range *f = func_of(jmp_addr);
    uint64_t lo = f ? f->start : jmp_addr - 0x100000, hi = f ? f->start + f->len : jmp_addr + 0x100000;
    int entries = 0;
    for (uint64_t e = table; e + 4 <= size && e + 4 <= hi && entries < 4096; e += 4, ++entries) {
        int32_t rel;
        memcpy(&rel, img + e, 4);
        uint64_t t = table + (uint64_t)(int64_t)rel;
        if (t < lo || t >= hi || t >= table) break;
        push(t);
    }
    if (!entries) return 0;
    fprintf(switches_out, "%llx %llx %d %llx %llx\n", (unsigned long long)jmp_addr, (unsigned long long)table,
            entries, (unsigned long long)lo, (unsigned long long)hi);
    ++stat_switch;
    stat_switch_entries += (uint64_t)entries;
    return 1;
}

static void decode_block(uint64_t a) {
    Insn window[WINDOW];
    int nw = 0;
    uint64_t start = a;
    ++stat_blocks;
    for (;;) {
        if (a >= size || !in_code(a)) break;
        if (BIT(insn_start, a) && a != start) break; /* runs into decoded code: same stream */
        Insn cur;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, img + a, size - a, &cur.in, cur.ops))) {
            ++stat_bad_decode;
            break;
        }
        cur.addr = a;
        SET(insn_start, a);
        for (uint64_t b = a; b < a + cur.in.length; ++b) SET(covered, b);
        ++stat_insns;
        if (nw == WINDOW) { memmove(window, window + 1, sizeof(Insn) * (WINDOW - 1)); --nw; }
        window[nw++] = cur;
        const uint64_t next = a + cur.in.length;
        const ZydisDecodedInstruction *in = &cur.in;
        const ZydisDecodedOperand *op0 = &cur.ops[0];
        switch (in->meta.category) {
        case ZYDIS_CATEGORY_RET:
            goto done;
        case ZYDIS_CATEGORY_COND_BR:
            if (op0->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) push(next + (uint64_t)op0->imm.value.s);
            push(next);
            goto done;
        case ZYDIS_CATEGORY_UNCOND_BR:
            if (op0->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                push(next + (uint64_t)op0->imm.value.s);
            } else if (op0->type == ZYDIS_OPERAND_TYPE_REGISTER) {
                if (!try_switch(window, nw - 1, a, op0->reg.value)) {
                    /* after the epilogue (pop/add rsp/leave): a tail call through a pointer */
                    const ZydisDecodedInstruction *prev = nw >= 2 ? &window[nw - 2].in : NULL;
                    const int tail = prev && (prev->mnemonic == ZYDIS_MNEMONIC_POP ||
                                              prev->mnemonic == ZYDIS_MNEMONIC_LEAVE ||
                                              (prev->mnemonic == ZYDIS_MNEMONIC_ADD &&
                                               window[nw - 2].ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                                               window[nw - 2].ops[0].reg.value == ZYDIS_REGISTER_RSP));
                    if (tail) {
                        ++stat_tail_reg;
                    } else {
                        ++stat_unresolved_jmp;
                        fprintf(unresolved_out, "%llx jmp-reg\n", (unsigned long long)a);
                    }
                }
            } else if (op0->type == ZYDIS_OPERAND_TYPE_MEMORY && op0->mem.base == ZYDIS_REGISTER_RIP) {
                ++stat_tailcall_reg; /* jmp [rip+slot]: import thunk / tail call through a pointer */
            } else {
                ++stat_tail_mem; /* jmp [reg+disp]: a virtual tail call through a vtable */
            }
            goto done;
        case ZYDIS_CATEGORY_CALL:
            if (op0->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) push(next + (uint64_t)op0->imm.value.s);
            else ++stat_icall;
            break;
        case ZYDIS_CATEGORY_INTERRUPT:
            if (in->mnemonic == ZYDIS_MNEMONIC_INT3) goto done;
            break;
        default:
            if (in->mnemonic == ZYDIS_MNEMONIC_UD2 || in->mnemonic == ZYDIS_MNEMONIC_HLT) goto done;
            break;
        }
        a = next;
    }
done:
    fprintf(blocks_out, "%llx %llx\n", (unsigned long long)start, (unsigned long long)(a - start));
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*n);
    if (fread(p, 1, *n, f) != *n) { fprintf(stderr, "short read %s\n", path); exit(1); }
    fclose(f);
    return p;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s image.bin funcs.txt roots.txt [out prefix]\n", argv[0]);
        return 2;
    }
    const char *prefix = argc > 4 ? argv[4] : "discover";
    img = read_file(argv[1], &size);
    insn_start = calloc(size / 8 + 1, 1);
    covered = calloc(size / 8 + 1, 1);
    block_seen = calloc(size / 8 + 1, 1);
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    FILE *f = fopen(argv[2], "r");
    unsigned long long s, l;
    size_t cap = 0;
    while (fscanf(f, "%llx %llx", &s, &l) == 2) {
        if (nfuncs == cap) { cap = cap ? cap * 2 : 1 << 18; funcs = realloc(funcs, cap * sizeof(Range)); }
        funcs[nfuncs++] = (Range){s, l};
    }
    fclose(f);

    /* Code ranges: the FDE span of the eboot plus the executable segments of linked modules
     * (the eboot's executable segment also holds read-only data after its code). */
    uint64_t fde_end = 0;
    for (size_t i = 0; i < nfuncs; ++i)
        if (funcs[i].start < 0x50d96dc && funcs[i].start + funcs[i].len > fde_end) fde_end = funcs[i].start + funcs[i].len;
    code_ranges[ncode++] = (Range){0, fde_end};
    char kind[32];
    unsigned long long x, y;
    f = fopen(argv[3], "r");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "exec %llx %llx", &x, &y) == 2 && x != 0 && ncode < 8) code_ranges[ncode++] = (Range){x, y};
    }
    rewind(f);

    uint64_t roots[6] = {0};
    const char *root_names[6] = {"fde", "entry", "init", "export", "reloc", "other"};
    for (size_t i = 0; i < nfuncs; ++i) { push(funcs[i].start); ++roots[0]; }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31s %llx", kind, &x) != 2) continue;
        int k = !strcmp(kind, "entry") ? 1 : !strcmp(kind, "init") ? 2 : !strcmp(kind, "export") ? 3 :
                !strcmp(kind, "reloc") ? 4 : -1;
        if (k < 0) continue;
        if (!in_code(x)) { if (k == 4) continue; ++stat_bad_roots; continue; }
        ++roots[k];
        push(x);
    }
    fclose(f);

    char path[1024];
    snprintf(path, sizeof(path), "%s.blocks", prefix);
    blocks_out = fopen(path, "w");
    snprintf(path, sizeof(path), "%s.switches", prefix);
    switches_out = fopen(path, "w");
    snprintf(path, sizeof(path), "%s.unresolved", prefix);
    unresolved_out = fopen(path, "w");
    while (nstack) decode_block(stack[--nstack]);
    fclose(blocks_out);
    fclose(unresolved_out);
    fclose(switches_out);

    uint64_t fde_bytes = 0, fde_covered = 0, outside = 0, all_covered = 0;
    for (size_t i = 0; i < nfuncs; ++i)
        for (uint64_t b = funcs[i].start; b < funcs[i].start + funcs[i].len && b < size; ++b) {
            ++fde_bytes;
            if (BIT(covered, b)) ++fde_covered;
        }
    for (int r = 0; r < ncode; ++r)
        for (uint64_t b = code_ranges[r].start; b < code_ranges[r].start + code_ranges[r].len; ++b)
            if (BIT(covered, b)) { ++all_covered; if (!func_of(b)) ++outside; }

    printf("roots:");
    for (int k = 0; k < 5; ++k) printf(" %s %llu", root_names[k], (unsigned long long)roots[k]);
    printf(" (relocation values outside code skipped; %llu other roots outside code)\n", (unsigned long long)stat_bad_roots);
    printf("blocks %llu, instructions %llu, undecodable block tails %llu\n", (unsigned long long)stat_blocks,
           (unsigned long long)stat_insns, (unsigned long long)stat_bad_decode);
    printf("code bytes reached %llu; inside FDE functions %llu of %llu (%.2f%%); outside FDEs %llu\n",
           (unsigned long long)all_covered, (unsigned long long)fde_covered, (unsigned long long)fde_bytes,
           100.0 * (double)fde_covered / (double)fde_bytes, (unsigned long long)outside);
    printf("switch tables %llu (%llu entries); tail calls through a pointer: register %llu, vtable %llu, "
           "[rip+slot] %llu; other indirect jmps %llu; indirect calls %llu\n",
           (unsigned long long)stat_switch, (unsigned long long)stat_switch_entries,
           (unsigned long long)stat_tail_reg, (unsigned long long)stat_tail_mem,
           (unsigned long long)stat_tailcall_reg, (unsigned long long)stat_unresolved_jmp, (unsigned long long)stat_icall);
    return 0;
}
