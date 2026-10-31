// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise actual Mach/Rosetta suspend, register capture and resume without a game. */
#include "runtime.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static GuestThread running, blocked;
static HostMutex registry = HOST_MUTEX_INIT, wait_lock = HOST_MUTEX_INIT;
static HostCond wait_cond = HOST_COND_INIT;
static atomic_int stop, ready;
static unsigned char owner[0x98] __attribute__((aligned(8)));
static unsigned char nodes[3][0x28] __attribute__((aligned(8)));
static void *code;
/* A synthetic busy worker with the register layout expected at the reported PC.
 * This exercises the diagnostic, not the game's collector or co-op behavior. */
extern void spin_with_list(void *pc, void *owner, void *node);
__asm__(".text\n_spin_with_list:\n"
        "pushq %rbx\npushq %r12\npushq %r15\n"
        "movq %rsi,%rbx\nleaq 0x58(%rbx),%r12\nmovq %rdx,%r15\n"
        "callq *%rdi\npopq %r15\npopq %r12\npopq %rbx\nretq\n");
int runtime_main_pthread(pthread_t *out) { (void)out; return 0; }
GuestThread *runtime_thread_get_all(void) { return &running; }
void *runtime_thread_get_lock(void) { return &registry; }
static void *spin(void *unused) {
    (void)unused; atomic_fetch_add(&ready, 1);
    spin_with_list(code, owner, nodes[0]);
    return NULL;
}
static void *waiter(void *unused) {
    (void)unused;
    host_lock(&wait_lock); atomic_fetch_add(&ready, 1);
    while (!atomic_load(&stop)) host_cond_wait(&wait_cond, &wait_lock);
    host_unlock(&wait_lock); return NULL;
}
static void put64(void *p, uintptr_t v) { memcpy(p, &v, sizeof(v)); }
static unsigned occurrences(const char *text, const char *needle) {
    unsigned n = 0;
    while ((text = strstr(text, needle))) { ++n; ++text; }
    return n;
}
int main(int argc, char **argv) {
    const int unreadable = argc > 1 && !strcmp(argv[1], "--unreadable");
    assert(!setenv("BB_SAMPLE_MS", "0", 1));
    const size_t image_size = 0x2500000;
    unsigned char *image = mmap(NULL, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(image != MAP_FAILED);
    const unsigned char busy[] = {0x48,0xb8,0,0,0,0,0,0,0,0,0x83,0x38,0,0x75,4,0xf3,0x90,0xeb,0xf7,0xc3};
    memcpy(image + 0x24a4160, busy, sizeof(busy));
    put64(image + 0x24a4162, (uintptr_t)&stop);
    const unsigned char signature[] = {0x4d,0x8b,0x7f,0x10,0x4d,0x39,0xe7,0x75,0xa5};
    memcpy(image + 0x24a41b2, signature, sizeof(signature));
    assert(!mprotect(image, image_size, PROT_READ | PROT_EXEC));
    code = image + 0x24a4160;
    runtime_sampler_init(image, image_size); /* bounds must work with periodic sampling off */
    put64(owner + 0x68, (uintptr_t)nodes[0]); put64(owner + 0x80, (uintptr_t)nodes[0]);
    for (unsigned i = 0; i < 3; ++i) {
        put64(nodes[i] + 0x10, (uintptr_t)nodes[i == 2 ? 1 : i + 1]);
        put64(nodes[i] + 0x18, (uintptr_t)(i ? nodes[i - 1] : owner + 0x58));
    }
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *inaccessible = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(inaccessible != MAP_FAILED);
    if (unreadable) put64(nodes[0] + 0x10, (uintptr_t)inaccessible);
    FILE *record = tmpfile(); assert(record);
    fflush(stdout); const int original_stdout = dup(STDOUT_FILENO); assert(original_stdout >= 0);
    assert(dup2(fileno(record), STDOUT_FILENO) >= 0);
    strcpy(running.name, "hang-running"); strcpy(blocked.name, "hang-blocked");
    running.next = &blocked; blocked.blocked_on = "condvar"; blocked.blocked_resource = 0x1234;
    assert(!pthread_create(&running.host, NULL, spin, NULL));
    assert(!pthread_create(&blocked.host, NULL, waiter, NULL));
    while (atomic_load(&ready) != 2) usleep(1000);
    usleep(10000); /* let the running worker enter the synthetic instruction window */
    runtime_thread_dump_all();
    atomic_store(&stop, 1); host_lock(&wait_lock);
    host_cond_broadcast(&wait_cond); host_unlock(&wait_lock);
    assert(!pthread_join(running.host, NULL)); assert(!pthread_join(blocked.host, NULL));
    fflush(stdout); assert(dup2(original_stdout, STDOUT_FILENO) >= 0); close(original_stdout);
    rewind(record); char output[16384];
    const size_t length = fread(output, 1, sizeof(output) - 1, record);
    assert(feof(record)); output[length] = 0; fclose(record);
    assert(occurrences(output, "Hang report: snapshot ") == 3);
    assert(occurrences(output, "hang-running") == 3 && occurrences(output, "hang-blocked") == 1);
    assert(occurrences(output, "    rax=") == 3 && occurrences(output, "    r12=") == 3);
    assert(occurrences(output, unreadable ? "List #118: unreadable address" : "List #118: observed cycle") == 3);
    if (!unreadable) assert(occurrences(output, "(repeated)") == 3);
    fputs(output, stdout);
    assert(!munmap(inaccessible, page)); assert(!munmap(image, image_size));
    puts("PASS: Mach/Rosetta registers and list reads; both sampled workers resume and join");
}
