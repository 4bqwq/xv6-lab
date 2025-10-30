#include "mod.h"

// 内核空间和用户空间的可分配物理页分开描述
alloc_region_t kern_region, user_region;

// 把 [begin, end) 切成 4KB 页，建立“空闲页单链表”到 r->list_head
static void region_build(alloc_region_t *r, uint64 begin, uint64 end) {
    r->begin = PGROUNDUP(begin);
    r->end   = PGROUNDDOWN(end);
    spinlock_init(&r->lk, "pmem_region");
    r->allocable = 0;
    r->list_head.next = NULL;

    for (uint64 p = r->begin; p + PGSIZE <= r->end; p += PGSIZE) {
        page_node_t *node = (page_node_t *)p;    // 复用页头 8B 做 next 指针
        node->next = r->list_head.next;
        r->list_head.next = node;
        r->allocable++;
    }
}

// 物理内存的初始化
// 本质上就是填写 kern_region 和 user_region，包括基本数值和空闲链表
void pmem_init(void)
{
    uint64 a_begin = (uint64)ALLOC_BEGIN;
    uint64 a_end   = (uint64)ALLOC_END;

    // 先给内核预留 KERN_PAGES 页，其余给用户；不够则全给内核
    uint64 total_pages = (PGROUNDDOWN(a_end) - PGROUNDUP(a_begin)) / PGSIZE;
    uint64 kern_pages  = total_pages > KERN_PAGES ? KERN_PAGES : total_pages;
    uint64 kern_end    = PGROUNDUP(a_begin) + kern_pages * PGSIZE;

    region_build(&kern_region, a_begin, kern_end);
    region_build(&user_region, kern_end, a_end);

    printf("[pmem] kern [%p, %p) pages=%d; user [%p, %p) pages=%d\n",
           kern_region.begin, kern_region.end,  (int)kern_region.allocable,
           user_region.begin, user_region.end,  (int)user_region.allocable);
}

// 从 region 弹出一个 4KB 页（清零后返回）；失败 panic
static void* region_alloc(alloc_region_t *r) {
    spinlock_acquire(&r->lk);
    page_node_t *n = r->list_head.next;
    if (n) {
        r->list_head.next = n->next;
        r->allocable--;
    }
    spinlock_release(&r->lk);
    if (!n) panic("pmem_alloc: out of memory");
    memset((void*)n, 0, PGSIZE);
    return (void*)n;
}

// 向 region 归还一个 4KB 页；失败 panic
static void region_free(alloc_region_t *r, uint64 page) {
    assert(page % PGSIZE == 0, "pmem_free: not aligned");
    assert(page >= r->begin && page + PGSIZE <= r->end, "pmem_free: out of range");
    page_node_t *n = (page_node_t *)page;
    spinlock_acquire(&r->lk);
    n->next = r->list_head.next;
    r->list_head.next = n;
    r->allocable++;
    spinlock_release(&r->lk);
}

// 尝试返回一个可分配的清零后的物理页；失败则 panic 锁死
void* pmem_alloc(bool in_kernel)
{
    return in_kernel ? region_alloc(&kern_region)
                     : region_alloc(&user_region);
}

// 释放一个物理页；失败则 panic 锁死
void pmem_free(uint64 page, bool in_kernel)
{
    if (in_kernel) region_free(&kern_region, page);
    else           region_free(&user_region, page);
}
