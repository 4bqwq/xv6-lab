#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

static volatile int started = 0;

// ========================= 入口 =========================
void main(void)
{
    if (mycpuid() == 0) {
        print_init();
        pmem_init();
        kvm_init();
        trap_kernel_init();

        __sync_synchronize();
        started = 1;
    } else {
        while (started == 0) { }
        __sync_synchronize();
    }

    kvm_inithart();
    trap_kernel_inithart();

    printf("cpu %d is booting!\n", mycpuid());

    for (;;) asm volatile("wfi");
}
