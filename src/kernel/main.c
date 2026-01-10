#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"
#include "proc/mod.h"
#include "fs/mod.h"

volatile static int started = 0;

int main()
{
    int cpuid = r_tp();

    if (cpuid == 0) {

        print_init();
        printf("cpu %d is booting!\n", cpuid);

        pmem_init();
        printf("pmem init done\n");
        kvm_init();
        printf("kvm init done\n");
        kvm_inithart();
        printf("kvm_inithart done\n");
        mmap_init();
        printf("mmap init done\n");
        virtio_disk_init();
        printf("virtio init done\n");
        proc_init();
        printf("proc_init done\n");
        proc_make_first();
        printf("proc_make_first done\n");
        trap_kernel_init();
        printf("trap_kernel_init done\n");
        trap_kernel_inithart();
        printf("trap_kernel_inithart done\n");
        intr_on();
        fs_init();
        proc_fs_init();
        printf("fs_init done\n");

        __sync_synchronize();
        started = 1;

    } else {

        while (started == 0)
            ;
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
        kvm_inithart();
        trap_kernel_inithart();
    }

    proc_scheduler();

    panic("main: never back!");
    return 0;
}
