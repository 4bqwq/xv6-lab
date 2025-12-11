#include "sys.h"

#define PGSIZE 4096

// brk 接口只接受绝对地址, 这里把"当前堆顶 + 偏移"转换成它要的参数
static long long adjust_heap(long long base, long long delta)
{
    long long target = base + delta;
    return syscall(SYS_brk, target);
}

int main()
{
    long long heap_top = syscall(SYS_brk, 0); // 先看初始堆顶

    long long plan[] = {9 * PGSIZE, 0, -5 * PGSIZE};
    for (int i = 0; i < 3; i++) {
        heap_top = adjust_heap(heap_top, plan[i]);
    }

    while (1)
        ;
    return 0;
}
