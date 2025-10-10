#include "arch/mod.h"
#include "lib/mod.h"

volatile static int started = 0;

void main(void) {
    int cpuid = mycpuid();
    if (cpuid == 0) {
        print_init();
        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
        started = 1;
    } else {
        while (started == 0) {}
        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
    }

    for (;;)
        asm volatile("wfi");
}
