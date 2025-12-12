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

static int mmap_call_seq = 0;
static int munmap_call_seq = 0;

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

// 只关注实验里涉及的两个顶级页表槽位: 低地址(0) + 高地址(TRAPFRAME所在槽)
static void dump_level0_slot(pgtbl_t l1, int idx)
{
    pte_t entry = l1[idx];
    if (!(entry & PTE_V))
        return;

    assert(PTE_CHECK(entry), "show_heap_event: unexpected leaf at level-0");
    pgtbl_t l0 = (pgtbl_t)PTE_TO_PA(entry);
    printf(".. .. level-0 pg tbl %d: pa = %p\n", idx, l0);

    for (int k = 0; k < PGSIZE / sizeof(pte_t); k++) {
        pte_t leaf = l0[k];
        if (!(leaf & PTE_V))
            continue;

        assert(!PTE_CHECK(leaf), "show_heap_event: leaf missing flags");
        printf(".. .. .. physical page %d: pa = %p flags = %d\n",
               k, (void *)PTE_TO_PA(leaf), (int)PTE_FLAGS(leaf));
    }
}

static void dump_level1_slot(pgtbl_t l2, int idx)
{
    pte_t pte = l2[idx];
    if (!(pte & PTE_V))
        return;

    assert(PTE_CHECK(pte), "show_heap_event: unexpected leaf at level-1");
    pgtbl_t l1 = (pgtbl_t)PTE_TO_PA(pte);
    printf(".. level-1 pg tbl %d: pa = %p\n", idx, l1);

    int targets[2];
    int n = 0;
    if (idx == 0)
        targets[n++] = 0;
    else if (idx == VA_TO_VPN(TRAPFRAME, 2))
        targets[n++] = VA_TO_VPN(TRAPFRAME, 1);

    for (int i = 0; i < n; i++)
        dump_level0_slot(l1, targets[i]);
}

// brk 调试输出: 展示返回值 + 页表关键结构, 便于对照测试截图
static void show_heap_event(const char *tag, proc_t *p, uint64 ret_top)
{
    printf("%s event: ret_heap_top = %p\n", tag, ret_top);

    pgtbl_t l2 = p->pgtbl;
    printf("level-2 pg tbl: pa = %p\n", l2);

    dump_level1_slot(l2, 0);
    dump_level1_slot(l2, VA_TO_VPN(TRAPFRAME, 2));

    printf("\n");
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
        uint64 ret = p->heap_top;
        show_heap_event("look", p, ret);
        return ret;
    }

    uint64 old_heap_top = p->heap_top;
    uint64 result = (uint64)-1;

    if (new_heap_top > old_heap_top) {
        // 扩展堆
        uint32 len = (uint32)(new_heap_top - old_heap_top);

        uint64 grown_top = uvm_heap_grow(p->pgtbl, old_heap_top, len);
        if (grown_top == (uint64)-1) {
            printf("sys_brk: grow failed\n");
            return (uint64)-1;
        }
        p->heap_top = grown_top;
        result = grown_top;
        show_heap_event("grow", p, result);
    } else if (new_heap_top < old_heap_top) {
        // 收缩堆
        uint32 len = (uint32)(old_heap_top - new_heap_top);

        uint64 shrunk_top = uvm_heap_ungrow(p->pgtbl, old_heap_top, len);
        if (shrunk_top == (uint64)-1) {
            printf("sys_brk: shrink failed\n");
            return (uint64)-1;
        }
        p->heap_top = shrunk_top;
        result = shrunk_top;
        show_heap_event("ungrow", p, result);
    } else {
        // new_heap_top == old_heap_top
        result = old_heap_top;
        show_heap_event("equal", p, result);
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
    proc_t *p = myproc();

    uint64 start;
    uint64 len;
    arg_uint64(0, &start);
    arg_uint64(1, &len);

    int seq = ++mmap_call_seq;
    printf("sys_mmap[%d] start %p len %p\n", seq, start, len);

    if (!mmap_len_valid(len) || !mmap_start_valid(start))
    {
        printf("sys_mmap[%d] reject invalid args\n", seq);
        return (uint64)-1;
    }

    if (start != 0 && len > MMAP_END - start)
    {
        printf("sys_mmap[%d] reject overflow range\n", seq);
        return (uint64)-1;
    }

    uint32 npages = (uint32)(len / PGSIZE);
    // uvm_mmap 负责分配物理页并调用 vm_mappages 建立映射
    uint64 mapped = uvm_mmap(start, npages, PTE_R | PTE_W);
    if (mapped == 0)
    {
        printf("sys_mmap[%d] alloc fail\n", seq);
        return (uint64)-1;
    }

    printf("sys_mmap[%d] mapped at %p npages %d\n", seq, mapped, npages);
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

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
    proc_t *p = myproc();

    uint64 start;
    uint64 len;
    arg_uint64(0, &start);
    arg_uint64(1, &len);

    int seq = ++munmap_call_seq;
    printf("sys_munmap[%d] start %p len %p\n", seq, start, len);

    if (!mmap_len_valid(len) || start == 0 || (start % PGSIZE) != 0)
    {
        printf("sys_munmap[%d] reject invalid args\n", seq);
        return (uint64)-1;
    }

    if (start < MMAP_BEGIN || start >= MMAP_END)
    {
        printf("sys_munmap[%d] reject out of range\n", seq);
        return (uint64)-1;
    }
    if (len > MMAP_END - start)
    {
        printf("sys_munmap[%d] reject overflow range\n", seq);
        return (uint64)-1;
    }

    uint32 npages = (uint32)(len / PGSIZE);
    // uvm_munmap 会在页表里逐段卸载映射并回收节点
    if (!uvm_munmap(start, npages))
    {
        printf("sys_munmap[%d] fail in unmap\n", seq);
        return (uint64)-1;
    }

    printf("sys_munmap[%d] ok npages %d\n", seq, npages);
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

    return 0;
}
