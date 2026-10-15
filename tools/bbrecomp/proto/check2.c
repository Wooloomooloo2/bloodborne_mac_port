/* Step 2 checker (arm64): maps the game image and the data buffer at the addresses lift2.c used,
 * runs the lifted functions on the recorded arguments and compares rax and the buffer's hash.
 * usage: check2 image.bin reference2.bin */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>

#define IMAGE_BASE 0x200000000000ull
#define DATA_BASE 0x210000000000ull
#define DATA_SIZE 65536
#define CASES 16

typedef uint64_t (*LiftedFn)(const uint64_t *, uint64_t);
extern const struct { uint64_t addr; LiftedFn fn; } lifted[];
extern const unsigned lifted_count;

static volatile uint64_t current;
static volatile int current_case;
static void on_fault(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "fault %d at %p in lifted %llx case %d\n", sig, info->si_addr,
                     (unsigned long long)current, current_case);
    write(1, buf, (size_t)n);
    _exit(3);
}

static uint64_t hash(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 1099511628211ull; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s image.bin reference2.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); size_t size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *image = mmap((void *)IMAGE_BASE, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    uint8_t *data = mmap((void *)DATA_BASE, DATA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (image != (uint8_t *)IMAGE_BASE || data != (uint8_t *)DATA_BASE) { fprintf(stderr, "fixed mappings failed\n"); return 1; }
    if (fread(image, 1, size, f) != size) return 1;
    fclose(f);
    FILE *r = fopen(argv[2], "rb");
    uint8_t *pristine = malloc(DATA_SIZE);
    if (fread(pristine, 1, DATA_SIZE, r) != DATA_SIZE) return 1;
    static uint64_t stack[4096];
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    uint64_t addr, res[CASES][12];
    unsigned functions = 0, passed = 0, cases = 0, rax_diff = 0, mem_diff = 0;
    while (fread(&addr, 8, 1, r) == 1 && fread(res, sizeof(res), 1, r) == 1) {
        LiftedFn fn = NULL;
        for (unsigned i = 0; i < lifted_count; ++i) if (lifted[i].addr == addr) { fn = lifted[i].fn; break; }
        if (!fn) continue;
        ++functions;
        int ok = 1;
        for (int k = 0; k < CASES; ++k) {
            ++cases;
            memcpy(data, pristine, DATA_SIZE);
            current = addr;
            current_case = k;
            /* the return address slot, then the stack arguments, as the x86 call left them */
            uint64_t *top = &stack[4096 - 8];
            for (int j = 0; j < 4; ++j) top[1 + j] = res[k][6 + j];
            uint64_t got = fn(res[k], (uint64_t)(uintptr_t)top);
            uint64_t h = hash(data, DATA_SIZE);
            if (got != res[k][10] || h != res[k][11]) {
                rax_diff += got != res[k][10];
                mem_diff += h != res[k][11];
                if (ok && functions - passed < 12)
                    printf("%llx: case %d rax x86 %llx lifted %llx%s\n", (unsigned long long)addr, k,
                           (unsigned long long)res[k][10], (unsigned long long)got, h != res[k][11] ? ", memory differs" : "");
                ok = 0;
            }
        }
        passed += ok;
    }
    printf("%u of %u functions match on all %d cases (cases with rax differing %u, memory differing %u, of %u)\n",
           passed, functions, CASES, rax_diff, mem_diff, cases);
    return 0;
}
