#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

volatile static int started = 0;

void main(void) {
    int cpuid = mycpuid();
    if (cpuid == 0) {
        print_init();
        printf("cpu %d is booting!\n", cpuid);

        // Lab2: 物理内存 + 内核页表
        pmem_init();      // 建立物理页池（基于 ALLOC_BEGIN/END）
        kvm_init();       // 构建内核页表（对等映射）
        kvm_inithart();   // 写 satp，开启分页
        printf("cpu %d: vm on\n", cpuid);

        // 小测试：分配/释放/再分配
        void* ps[8];
        for (int i=0;i<8;i++) ps[i]=pmem_alloc(true);
        for (int i=0;i<8;i+=2) pmem_free((uint64)ps[i], true);
        for (int i=0;i<4;i++) assert(pmem_alloc(true)!=0, "pmem re-alloc fail");

        __sync_synchronize();
        started = 1;
    } else {
        while (started == 0) {}
        __sync_synchronize();
        kvm_inithart();   // 次核也要切 satp
        printf("cpu %d is booting!\n", cpuid);
    }

    for (;;)
        asm volatile("wfi");
}
