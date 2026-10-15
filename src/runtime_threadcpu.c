/* CPU time of game threads for the frame spike log (gnmdriver.cpp puts it in the GPU
 * process's control block at each flip). Apart from runtime_thread.c: the Mach headers declare a
 * thread_create of their own. */
#include "runtime.h"
#include <pthread.h>
#include <string.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

int runtime_main_pthread(pthread_t *out);
int runtime_thread_ports(unsigned *ports, char (*names)[32], int max);

#ifdef __APPLE__
static uint64_t port_cpu_ns(mach_port_t port) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return ((uint64_t)info.user_time.seconds + (uint64_t)info.system_time.seconds) * 1000000000ull +
           ((uint64_t)info.user_time.microseconds + (uint64_t)info.system_time.microseconds) * 1000ull;
}
#endif

/* CPU time all guest threads used since the previous call, and the busiest of them (name and
 * time). One caller (the flip submit); the main thread created by the loader is included. */
void runtime_game_cpu_since_last(uint64_t *total_ns, uint64_t *busiest_ns, char busiest_name[32]) {
    *total_ns = *busiest_ns = 0;
    busiest_name[0] = 0;
#ifdef __APPLE__
    enum { Max = 256 };
    static unsigned last_port[Max];
    static uint64_t last_ns[Max];
    static int last_count;
    unsigned ports[Max];
    char names[Max][32];
    int n = runtime_thread_ports(ports, names, Max);
    uint64_t now_ns[Max];
    for (int i = 0; i < n; ++i) {
        now_ns[i] = port_cpu_ns(ports[i]);
        uint64_t before = 0;
        for (int j = 0; j < last_count; ++j) if (last_port[j] == ports[i]) { before = last_ns[j]; break; }
        uint64_t delta = now_ns[i] >= before ? now_ns[i] - before : now_ns[i];
        *total_ns += delta;
        if (delta > *busiest_ns) { *busiest_ns = delta; memcpy(busiest_name, names[i], 32); }
    }
    for (int i = 0; i < n; ++i) { last_port[i] = ports[i]; last_ns[i] = now_ns[i]; }
    last_count = n;
#endif
}

uint64_t runtime_thread_cpu_ns(int which) {
#ifdef __APPLE__
    pthread_t thread = pthread_self();
    if (!which && !runtime_main_pthread(&thread)) return 0;
    return port_cpu_ns(pthread_mach_thread_np(thread));
#else
    (void)which;
    return 0;
#endif
}
