/* Runs the lifted functions (lifted.c, compiled natively for arm64) on the arguments in
 * reference.bin and compares rax with the original x86 code's result (lift.c, under Rosetta 2).
 * usage: check reference.bin */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t (*LiftedFn)(const uint64_t *);
extern const struct { uint64_t addr; LiftedFn fn; } lifted[];
extern const unsigned lifted_count;

#define CASES 64

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s reference.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    unsigned functions = 0, passed = 0, cases = 0, case_fail = 0;
    uint64_t addr, r[CASES][7];
    while (fread(&addr, 8, 1, f) == 1 && fread(r, sizeof(r), 1, f) == 1) {
        LiftedFn fn = NULL;
        for (unsigned i = 0; i < lifted_count; ++i) if (lifted[i].addr == addr) { fn = lifted[i].fn; break; }
        if (!fn) { printf("%llx: no lifted function\n", (unsigned long long)addr); continue; }
        ++functions;
        int ok = 1;
        for (int k = 0; k < CASES; ++k) {
            ++cases;
            uint64_t got = fn(r[k]);
            if (got != r[k][6]) {
                ++case_fail;
                if (ok) printf("%llx: case %d args %llx %llx %llx -> x86 %llx, lifted %llx\n", (unsigned long long)addr, k,
                               (unsigned long long)r[k][0], (unsigned long long)r[k][1], (unsigned long long)r[k][2],
                               (unsigned long long)r[k][6], (unsigned long long)got);
                ok = 0;
            }
        }
        passed += ok;
    }
    printf("%u of %u functions match the x86 code on all %d cases (%u of %u cases differ)\n", passed, functions, CASES,
           case_fail, cases);
    return passed == functions ? 0 : 1;
}
