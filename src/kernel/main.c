#include "arch/mod.h"
#include "lib/mod.h"

void main(void) {
    print_init();
    
    int cpuid = r_tp();
    if (cpuid == 0) {
        printf("cpu %d is booting!\n", cpuid);
        printf("Test: %d %p %x %c %s\n", 998244353, 0x1234, 0x12345678ULL, 'A', "OK");
    }
    for (;;)
        asm volatile("wfi");
}
