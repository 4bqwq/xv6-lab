#include "mod.h"

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
    return -1;
}

/*
    增加一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节,需检查是否是page-aligned)
    成功返回映射空间的起始地址, 失败返回-1
*/
uint64 sys_mmap()
{
    return 0;
}

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节, 需检查是否是page-aligned)
    成功返回0 失败返回-1
*/
uint64 sys_munmap()
{
    return 0;
}
