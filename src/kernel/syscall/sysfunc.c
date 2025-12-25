#include "mod.h"
#include "../lock/mod.h"
#include "../fs/method.h"
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
    printf("hello world!\n");
    return 0;
}

/*
    测试: 向用户空间传入一个int类型的数组
    uint64 addr 数组起始地址
    uint32 len  数组元素数量
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

    uint64 uaddr;
    arg_uint64(0, &uaddr);

    char buf[STR_MAXLEN + 1];
    memset(buf, 0, sizeof(buf));
    uvm_copyin_str(p->pgtbl, (uint64)buf, uaddr, STR_MAXLEN);

    printf("sys_copyinstr: %s\n", buf);
    return 0;
}

/*
    brk
    调整进程的 heap_top (堆边界)
    参数:
        uint64 new_brk: 新的堆顶(用户虚拟地址)
    返回:
        成功: old_brk (原堆顶)
        失败: -1
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

    if (new_heap_top > old_heap_top) {
        uint32 len = (uint32)(new_heap_top - old_heap_top);
        uint64 grown_top = uvm_heap_grow(p->pgtbl, old_heap_top, len);
        if (grown_top == (uint64)-1) return (uint64)-1;
        p->heap_top = grown_top;
        return grown_top;
    }

    if (new_heap_top < old_heap_top) {
        uint32 len = (uint32)(old_heap_top - new_heap_top);
        uint64 shrunk_top = uvm_heap_ungrow(p->pgtbl, old_heap_top, len);
        if (shrunk_top == (uint64)-1) return (uint64)-1;
        p->heap_top = shrunk_top;
        return shrunk_top;
    }

    return old_heap_top;
}


uint64 sys_mmap()
{
    uint64 start, len;
    arg_uint64(0, &start);
    arg_uint64(1, &len);

    if (!mmap_len_valid(len) || !mmap_start_valid(start))
        return (uint64)-1;

    if (start != 0 && len > MMAP_END - start)
        return (uint64)-1;

    uint32 npages = (uint32)(len / PGSIZE);

    // uvm_mmap 负责分配物理页并建立映射
    uint64 mapped = uvm_mmap(start, npages, PTE_R | PTE_W);
    return mapped;
}


uint64 sys_munmap()
{
    uint64 start, len;
    arg_uint64(0, &start);
    arg_uint64(1, &len);

    if (!mmap_len_valid(len) || !mmap_start_valid(start))
        return (uint64)-1;

    if (start == 0)
        return (uint64)-1;
    if (start < MMAP_BEGIN || start >= MMAP_END)
        return (uint64)-1;
    if (len > MMAP_END - start)
        return (uint64)-1;

    uint32 npages = (uint32)(len / PGSIZE);
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
    uint32 x = 0;
    arg_uint32(0, &x);
    printf("%d", x);
    return 0;
}

uint64 sys_fork()
{
    return proc_fork();
}

uint64 sys_wait()
{
    uint64 uaddr = 0;
    arg_uint64(0, &uaddr);
    return (uint64)proc_wait(uaddr);
}

uint64 sys_exit()
{
    uint32 code = 0;
    arg_uint32(0, &code);
    proc_exit((int)code);
    return 0; // 不会到这
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

static sleeplock_t global_slock;
static bool global_slock_inited = false;

static void __attribute__((unused)) ensure_global_slock()
{
    if (!global_slock_inited) {
        sleeplock_init(&global_slock, "global_slock");
        global_slock_inited = true;
    }
}

uint64 sys_sleeplock_acquire()
{
    ensure_global_slock();
    sleeplock_acquire(&global_slock);
    return 0;
}

uint64 sys_sleeplock_release()
{
    ensure_global_slock();
    sleeplock_release(&global_slock);
    return 0;
}

/* ============================================================
   Lab7: 文件系统/磁盘管理 相关测试系统调用 (SYS_alloc_block ~ SYS_flush_buffer)
   注意：这些系统调用是“教学用的调试口”，会把内核指针直接返回给用户态。
   ============================================================ */

// 从 data bitmap 申请一个 block，返回 block 号
uint64 sys_alloc_block()
{
    return (uint64)bitmap_alloc_block();
}

// 释放一个 data block
uint64 sys_free_block()
{
    uint32 block_num = 0;
    arg_uint32(0, &block_num);
    bitmap_free_block(block_num);
    return 0;
}

// 从 inode bitmap 申请一个 inode，返回 inode 号
uint64 sys_alloc_inode()
{
    return (uint64)bitmap_alloc_inode();
}

// 释放一个 inode
uint64 sys_free_inode()
{
    uint32 inode_num = 0;
    arg_uint32(0, &inode_num);
    bitmap_free_inode(inode_num);
    return 0;
}

// 输出 bitmap 状态：arg0=0 打印 data bitmap；arg0=1 打印 inode bitmap
uint64 sys_show_bitmap()
{
    uint32 which = 0;
    arg_uint32(0, &which);
    // method.h: bitmap_print(true) => data bitmap
    bitmap_print(which == 0);
    return 0;
}

// 获取一个描述 block 的 buffer，返回 buffer_t* (以 uint64 形式返回给用户态)
uint64 sys_get_block()
{
    uint32 block_num = 0;
    arg_uint32(0, &block_num);

    buffer_t *b = buffer_get(block_num);
    return (uint64)b;
}

// 将 buf->data 拷贝到用户空间：arg0=buffer_t*，arg1=用户地址
uint64 sys_read_block()
{
    proc_t *p = myproc();

    uint64 baddr = 0;
    uint64 uaddr = 0;
    arg_uint64(0, &baddr);
    arg_uint64(1, &uaddr);

    buffer_t *b = (buffer_t *)baddr;
    if (b == NULL || b->data == NULL) return (uint64)-1;

    uvm_copyout(p->pgtbl, uaddr, (uint64)b->data, BLOCK_SIZE);
    return 0;
}

// 基于用户地址空间更新 buffer->data 并写入磁盘：arg0=buffer_t*，arg1=用户地址
uint64 sys_write_block()
{
    proc_t *p = myproc();

    uint64 baddr = 0;
    uint64 uaddr = 0;
    arg_uint64(0, &baddr);
    arg_uint64(1, &uaddr);

    buffer_t *b = (buffer_t *)baddr;
    if (b == NULL || b->data == NULL) return (uint64)-1;

    uvm_copyin(p->pgtbl, (uint64)b->data, uaddr, BLOCK_SIZE);
    buffer_write(b);
    return 0;
}

// 释放一个描述 block 的 buffer：arg0=buffer_t*
uint64 sys_put_block()
{
    uint64 baddr = 0;
    arg_uint64(0, &baddr);

    buffer_t *b = (buffer_t *)baddr;
    if (b == NULL) return (uint64)-1;

    buffer_put(b);
    return 0;
}

// 输出 buffer 链表状态
uint64 sys_show_buffer()
{
    buffer_print_info();
    return 0;
}

// 释放非活跃链表中 buffer 持有的物理内存资源：arg0=要释放的 buffer 数量上限
// 返回实际释放的 buffer 数量
uint64 sys_flush_buffer()
{
    uint32 n = 0;
    arg_uint32(0, &n);
    return (uint64)buffer_freemem(n);
}

