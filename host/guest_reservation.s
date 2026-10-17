// The guest range [0x7000000000, 0xfc00000000) as an empty, inaccessible segment of
// bbgpu-host, so the kernel reserves it at exec, before libmalloc runs. From macOS 27 on,
// libmalloc places its regions (a few MiB plus ~12 GiB guards each) at random addresses in
// this range during process start, so a reservation made in main() can no longer get it.
// build_macos.sh pins the segment with -segaddr/-segprot; host/main.c takes the exact range
// over from it. Size: BBGPU_GUEST_MAX - BBGPU_GUEST_MIN.
.globl _bbgpu_guest_reservation
.zerofill __GUESTRSV,__rsv,_bbgpu_guest_reservation,0x8C00000000,14
