#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

// ========================= 入口 =========================
void main(void)
{
    // 只让 CPU0 跑，避免多核并发改页表/打印冲突
    if (mycpuid() != 0) { for(;;) asm volatile("wfi"); }

    print_init();
    pmem_init();
    kvm_init();
    kvm_inithart();

    trap_kernel_init();
    trap_kernel_inithart();

    printf("cpu %d is booting!\n", mycpuid());

    // 保持 QEMU 运行，便于观察；退出用 Ctrl-A 然后 X
    for(;;) asm volatile("wfi");
}

