#include "mod.h"

// 检查页对齐并确保长度落在 mmap 设计范围
static bool mmap_len_valid(uint64 len)
{
    return (len != 0) && (len % PGSIZE == 0) &&
           (len <= (MMAP_END - MMAP_BEGIN));
}

// mmap 起点要么交给内核分配要么满足页对齐和边界约束
static bool mmap_start_valid(uint64 start)
{
    return (start == 0) ||
           (start % PGSIZE == 0 && start >= MMAP_BEGIN && start < MMAP_END);
}

uint64 sys_helloworld()
{
    printf("proczero: hello world!\n");
    return 0;
}

/*
    测试: 从用户空间传入一个int类型的数组
    uint64 addr 数组起始地址
    uint32 len  元素数量
    成功返回0
*/
uint64 sys_copyin()
{
    proc_t *p = myproc();

    uint64 uaddr;  // 用户数组起始地址
    uint32 len;    // 元素数量

    // 第 0 个参数：数组指针
    arg_uint64(0, &uaddr);
    // 第 1 个参数：元素个数
    arg_uint32(1, &len);

    if (len > 32) len = 32;  // 防御性：最多读 32 个 int，防止乱传太大

    int buf[32];
    uvm_copyin(p->pgtbl, (uint64)buf, uaddr, len * sizeof(int));

    printf("get a number from user: ");
    for (uint32 i = 0; i < len; i++) {
        printf("%d\n", buf[i]);
        if (i + 1 < len) printf("get a number from user: ");
    }

    return 0;
}

/*
    测试: 向用户空间传出一个int类型的数组
    uint64 addr 数组起始地址
    成功返回拷贝的元素数量
*/
uint64 sys_copyout()
{
    proc_t *p = myproc();

    uint64 uaddr;  // 用户数组起始地址
    arg_uint64(0, &uaddr);

    int kdata[5] = {1, 2, 3, 4, 5};
    uvm_copyout(p->pgtbl, uaddr, (uint64)kdata, sizeof(kdata));

    printf("sys_copyout: wrote [1 2 3 4 5] to user\n");

    return 5;   // 表示拷贝了 5 个元素
}

/*
    测试: 从用户空间传入一个字符串
    uint64 addr 字符串起始地址
    成功返回0
*/
uint64 sys_copyinstr()
{
    proc_t *p = myproc();

    uint64 uaddr;  // 用户字符串起始地址
    arg_uint64(0, &uaddr);

    char buf[STR_MAXLEN + 1];
    memset(buf, 0, sizeof(buf));

    uvm_copyin_str(p->pgtbl, (uint64)buf, uaddr, STR_MAXLEN);

    printf("get string for user: %s\n", buf);

    return 0;
}

/*
    用户堆空间伸缩
    uint64 new_heap_top (如果是0, 代表查询当前堆顶位置)
    成功返回new_heap_top, 失败返回-1
*/
uint64 sys_brk()
{
    proc_t *p = myproc();

    uint64 new_heap_top;
    arg_uint64(0, &new_heap_top);

    // 查询当前堆顶
    if (new_heap_top == 0) {
        return p->heap_top;
    }

    uint64 old_heap_top = p->heap_top;
    uint64 result = (uint64)-1;

    if (new_heap_top > old_heap_top) {
        // 扩展堆
        uint32 len = (uint32)(new_heap_top - old_heap_top);

        uint64 grown_top = uvm_heap_grow(p->pgtbl, old_heap_top, len);
        if (grown_top == (uint64)-1)
            return (uint64)-1;
        p->heap_top = grown_top;
        result = grown_top;
    } else if (new_heap_top < old_heap_top) {
        // 收缩堆
        uint32 len = (uint32)(old_heap_top - new_heap_top);

        uint64 shrunk_top = uvm_heap_ungrow(p->pgtbl, old_heap_top, len);
        if (shrunk_top == (uint64)-1)
            return (uint64)-1;
        p->heap_top = shrunk_top;
        result = shrunk_top;
    } else {
        // new_heap_top == old_heap_top
        result = old_heap_top;
    }

    return result;
}

/*
    增加一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节,需检查是否是page-aligned)
    成功返回映射空间的起始地址, 失败返回-1
*/
uint64 sys_mmap()
{
    uint64 start;
    uint64 len;
    arg_uint64(0, &start);
    arg_uint64(1, &len);

    if (!mmap_len_valid(len) || !mmap_start_valid(start))
        return (uint64)-1;

    if (start != 0 && len > MMAP_END - start)
        return (uint64)-1;

    uint32 npages = (uint32)(len / PGSIZE);
    // uvm_mmap 负责分配物理页并调用 vm_mappages 建立映射
    uint64 mapped = uvm_mmap(start, npages, PTE_R | PTE_W);
    if (mapped == 0)
        return (uint64)-1;

    return mapped;
}

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节, 需检查是否是page-aligned)
    成功返回0 失败返回-1
*/
uint64 sys_munmap()
{
    uint64 start;
    uint64 len;
    arg_uint64(0, &start);
    arg_uint64(1, &len);

    if (!mmap_len_valid(len) || start == 0 || (start % PGSIZE) != 0)
        return (uint64)-1;

    if (start < MMAP_BEGIN || start >= MMAP_END)
        return (uint64)-1;
    if (len > MMAP_END - start)
        return (uint64)-1;

    uint32 npages = (uint32)(len / PGSIZE);
    // uvm_munmap 会在页表里逐段卸载映射并回收节点
    if (!uvm_munmap(start, npages))
        return (uint64)-1;

    return 0;
}

uint64 sys_print_str()
{
    proc_t *p = myproc();
    uint64 uaddr = 0;
    arg_uint64(0, &uaddr);

    char buf[STR_MAXLEN + 1];
    memset(buf, 0, sizeof(buf));
    uvm_copyin_str(p->pgtbl, (uint64)buf, uaddr, STR_MAXLEN);

    printf("%s", buf);
    return 0;
}

uint64 sys_print_int()
{
    uint32 num = 0;
    arg_uint32(0, &num);
    printf("num = %d\n", (int)num);
    return 0;
}

uint64 sys_fork()
{
    int pid = proc_fork();
    proc_yield();
    return (uint64)pid;
}

uint64 sys_wait()
{
    uint64 addr = 0;
    arg_uint64(0, &addr);
    return (uint64)proc_wait(addr);
}

uint64 sys_exit()
{
    int code = 0;
    arg_uint32(0, (uint32 *)&code);
    proc_exit(code);
    return 0;
}

uint64 sys_sleep()
{
    uint64 ntick = 0;
    arg_uint64(0, &ntick);
    timer_wait(ntick);
    return 0;
}

uint64 sys_getpid()
{
    proc_t *p = myproc();
    return (uint64)p->pid;
}
