#include "mod.h"
#include "../lib/mod.h"

// 辅助：根据用户页表 pgtbl 和用户虚拟地址 va，找到该页对应的物理页基址
static uint64 walk_user_va(pgtbl_t pgtbl, uint64 va)
{
    pte_t *pte = vm_getpte(pgtbl, va, false);
    assert(pte != NULL, "uvm: vm_getpte returned NULL");
    assert((*pte) & PTE_V, "uvm: pte not valid");

    return (uint64)PTE_TO_PA(*pte);  // 页框物理地址（页内偏移还要自己加）
}

/*--------------------part-1: 关于内核空间<->用户空间的数据传递--------------------*/

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    while (len > 0)
    {
        uint64 va0 = PGROUNDDOWN(src);    // 当前用户页首地址
        uint32 off = (uint32)(src - va0); // 页内偏移
        uint32 npage = PGSIZE - off;      // 当前页还能拷多少字节
        uint32 n = (len < npage) ? len : npage;

        uint64 pa0 = walk_user_va(pgtbl, va0); // 这一页的物理基址
        void* k_src = (void*)(pa0 + off);      // 真正的物理地址 + 偏移
        void* k_dst = (void*)dst;

        memmove(k_dst, k_src, n);

        dst += n;
        src += n;
        len -= n;
    }
}

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    while (len > 0)
    {
        uint64 va0 = PGROUNDDOWN(dst);    // 当前用户页首地址
        uint32 off = (uint32)(dst - va0); // 页内偏移
        uint32 npage = PGSIZE - off;      // 当前页还能写多少字节
        uint32 n = (len < npage) ? len : npage;

        uint64 pa0 = walk_user_va(pgtbl, va0);
        void* k_dst = (void*)(pa0 + off); // 写入到此物理地址
        void* k_src = (void*)src;

        memmove(k_dst, k_src, n);

        dst += n;
        src += n;
        len -= n;
    }
}

// 用户态字符串拷贝到内核态
// 最多拷贝maxlen字节, 中途遇到'\0'则终止
// 注意: src dst 不一定是 page-aligned
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    char *dst_ptr = (char *)dst;
    uint32 i;
    for (i = 0; i < maxlen - 1; i++) {
        char c = 0;  // 初始化变量
        // 逐字节拷贝，直到遇到'\0'或达到最大长度
        uvm_copyin(pgtbl, (uint64)&c, src + i, 1);
        dst_ptr[i] = c;
        if (c == '\0') {
            break;  // 遇到字符串结束符则停止
        }
    }
    dst_ptr[i] = '\0';  // 确保字符串以'\0'结尾
}

/*--------------------part-2: mmap_region相关--------------------*/

// 打印以mmap为首的mmap链
// for debug
void uvm_show_mmaplist(mmap_region_t *mmap)
{
    mmap_region_t *tmp = mmap;
    printf("\nallocated mmap_space:\n");
    if (tmp == NULL)
        printf("empty\n");
    while (tmp != NULL)
    {
        printf("allocated mmap_region: %p ~ %p\n", tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
    }
}

// 两个 mmap_region 区域合并
// 注意: 保留一个 释放一个 不操作 next 指针
// 由uvm_mmap调用
static void mmap_merge(mmap_region_t *mmap_1, mmap_region_t *mmap_2, bool keep_mmap_1)
{
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL region");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin,
           "mmap_merge: regions not adjacent");

    if (keep_mmap_1) {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    } else {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}

// 寻找一块足够大的区域(len), 作为 mmap_region
// 由uvm_mmap调用(处理begin==0的情况)
// 成功返回begin, 失败返回0
static uint64 uvm_mmap_find(mmap_region_t *head_mmap, uint64 len,
                            mmap_region_t **p_last_mmap, mmap_region_t **p_tmp_mmap)
{
    mmap_region_t *prev = NULL;
    mmap_region_t *curr = head_mmap;
    uint64 candidate = MMAP_BEGIN;

    while (curr != NULL) {
        if (candidate + len <= curr->begin) {
            if (p_last_mmap)
                *p_last_mmap = prev;
            if (p_tmp_mmap)
                *p_tmp_mmap = curr;
            return candidate;
        }

        candidate = curr->begin + (uint64)curr->npages * PGSIZE;
        prev = curr;
        curr = curr->next;
    }

    if (candidate + len <= MMAP_END) {
        if (p_last_mmap)
            *p_last_mmap = prev;
        if (p_tmp_mmap)
            *p_tmp_mmap = NULL;
        return candidate;
    }

    if (p_last_mmap)
        *p_last_mmap = NULL;
    if (p_tmp_mmap)
        *p_tmp_mmap = NULL;
    return 0;
}

// 在用户页表和进程mmap链里新增mmap区域 [begin, begin + npages * PGSIZE)
// 调用者保证begin是page-aligned的, 页面权限为perm
// 注意: 如果start==0, 意味着需要内核自主找一块足够大的空间
// 失败则panic卡死
uint64 uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    proc_t *p = myproc();
    if (npages == 0)
        return 0;

    uint64 len = (uint64)npages * PGSIZE;
    mmap_region_t *prev = NULL;
    mmap_region_t *next = NULL;
    uint64 real_begin = begin;

    // begin 为0 时扫描整个 mmap 链和 MMAP 范围找空洞
    if (begin == 0) {
        real_begin = uvm_mmap_find(p->mmap, len, &prev, &next);
        if (real_begin == 0)
            return 0;
    } else {
        if (begin < MMAP_BEGIN || begin + len > MMAP_END)
            return 0;

        uint64 prev_end = MMAP_BEGIN;
        next = p->mmap;
        while (next && next->begin < begin) {
            prev = next;
            prev_end = next->begin + (uint64)next->npages * PGSIZE;
            next = next->next;
        }

        if (begin < prev_end)
            return 0;
        if (next && begin + len > next->begin)
            return 0;
    }

    mmap_region_t *node = mmap_region_alloc();
    node->begin = (begin == 0) ? real_begin : begin;
    node->npages = npages;
    node->next = next;

    if (prev)
        prev->next = node;
    else
        p->mmap = node;

    // vm_mappages 需要物理页基址和权限位 这里给用户态加 PTE_U
    int map_perm = perm | PTE_U;
    uint64 map_begin = node->begin;
    for (uint32 i = 0; i < npages; i++) {
        uint64 va = map_begin + (uint64)i * PGSIZE;
        uint64 page = (uint64)pmem_alloc(false);
        assert(page != 0, "uvm_mmap: pmem_alloc failed");
        memset((void *)page, 0, PGSIZE);
        vm_mappages(p->pgtbl, va, page, PGSIZE, map_perm);
    }

    // 头尾相连时立即合并节点避免链表碎片
    if (prev && prev->begin + (uint64)prev->npages * PGSIZE == node->begin) {
        mmap_region_t *after = node->next;
        mmap_merge(prev, node, true);
        prev->next = after;
        node = prev;
    }

    if (node->next &&
        node->begin + (uint64)node->npages * PGSIZE == node->next->begin) {
        mmap_region_t *victim = node->next;
        mmap_region_t *after = victim->next;
        mmap_merge(node, victim, true);
        node->next = after;
    }

    return (begin == 0) ? real_begin : begin;
}


// 在用户页表和进程mmap链里释放mmap区域 [begin, begin + npages * PGSIZE)
// 失败则panic卡死
bool uvm_munmap(uint64 begin, uint32 npages)
{
    proc_t *p = myproc();
    if (npages == 0)
        return false;

    uint64 len = (uint64)npages * PGSIZE;
    uint64 end = begin + len;
    if (begin < MMAP_BEGIN || end > MMAP_END)
        return false;

    mmap_region_t *prev = NULL;
    mmap_region_t *curr = p->mmap;

    // 先确认请求区间被完整覆盖，避免失败时已经释放部分资源
    uint64 probe = begin;
    mmap_region_t *check = curr;
    while (check && begin >= check->begin + (uint64)check->npages * PGSIZE) {
        prev = check;
        check = check->next;
    }
    while (probe < end) {
        if (check == NULL)
            return false;
        uint64 region_start = check->begin;
        uint64 region_end = region_start + (uint64)check->npages * PGSIZE;
        if (probe < region_start || probe >= region_end)
            return false;
        uint64 chunk_end = (end < region_end) ? end : region_end;
        probe = chunk_end;
        if (probe < end)
            check = check->next;
    }

    // 回到实际操作阶段
    curr = (prev == NULL) ? p->mmap : prev->next;
    while (curr && begin >= curr->begin + (uint64)curr->npages * PGSIZE) {
        prev = curr;
        curr = curr->next;
    }

    // 逐段消费目标区间 每次释放一段真实映射
    uint64 cursor = begin;
    while (cursor < end) {
        if (!curr)
            return false;

        uint64 region_start = curr->begin;
        uint64 region_end = region_start + (uint64)curr->npages * PGSIZE;
        if (cursor < region_start || cursor >= region_end)
            return false;

        uint64 chunk_end = (end < region_end) ? end : region_end;
        uint64 chunk_len = chunk_end - cursor;
        if (chunk_len == 0 || chunk_len % PGSIZE)
            return false;

        vm_unmappages(p->pgtbl, cursor, chunk_len, true);

        uint32 chunk_pages = (uint32)(chunk_len / PGSIZE);
        if (cursor == region_start && chunk_end == region_end) {
            mmap_region_t *next = curr->next;
            if (prev)
                prev->next = next;
            else
                p->mmap = next;
            mmap_region_free(curr);
            curr = next;
        } else if (cursor == region_start) {
            curr->begin = chunk_end;
            curr->npages -= chunk_pages;
            prev = curr;
        } else if (chunk_end == region_end) {
            curr->npages -= chunk_pages;
            prev = curr;
            curr = curr->next;
        } else {
            uint32 left_pages = (uint32)((cursor - region_start) / PGSIZE);
            uint32 right_pages = (uint32)((region_end - chunk_end) / PGSIZE);
            mmap_region_t *tail = mmap_region_alloc();
            tail->begin = chunk_end;
            tail->npages = right_pages;
            tail->next = curr->next;
            curr->npages = left_pages;
            curr->next = tail;
            prev = curr;
            curr = tail;
        }

        cursor = chunk_end;
    }

    return true;
}

/*------------------part-3: 用户空间heap和stack管理相关------------------*/

// 用户堆空间增加, 返回新的堆顶地址 (注意栈顶最大值限制)
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len, int flag) {
    if (len == 0)
        return cur_heap_top;

    uint64 new_heap_top = cur_heap_top + len;
    if (new_heap_top >= TRAPFRAME)
        return (uint64)-1;

    uint64 va_start = PGROUNDUP(cur_heap_top);
    uint64 va_end = PGROUNDUP(new_heap_top);
    int perm = flag | PTE_U;

    for (uint64 va = va_start; va < va_end; va += PGSIZE) {
        uint64 pa = (uint64)pmem_alloc(false);
        if (pa == 0)
            return (uint64)-1;
        vm_mappages(pgtbl, va, pa, PGSIZE, perm);
    }

    return new_heap_top;
}


// 用户堆空间减少, 返回新的堆顶地址
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len)
{
    if (len == 0)
        return cur_heap_top;

    if (len > cur_heap_top) {
        // 防御性：理论上不应该发生
        printf("uvm_heap_ungrow: len %d > cur_heap_top %p\n", len, cur_heap_top);
        return (uint64)-1;
    }

    uint64 new_heap_top = cur_heap_top - len;

    // 释放 [new_heap_top, cur_heap_top) 范围里被整页覆盖的部分
    uint64 va_start = PGROUNDUP(new_heap_top);
    uint64 va_end = PGROUNDDOWN(cur_heap_top - 1);

    if (va_start <= va_end) {
        for (uint64 va = va_start; va <= va_end; va += PGSIZE) {
            // 检查页面是否存在映射
            pte_t *pte = vm_getpte(pgtbl, va, false);
            if (pte && (*pte & PTE_V)) {
                vm_unmappages(pgtbl, va, PGSIZE, true);
            }
        }
    }

    return new_heap_top;
}

// 处理函数栈增长导致的page fault事件
// 成功返回new_ustack_npage，失败返回-1
uint64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_ustack_npage, uint64 fault_addr)
{
    // 计算当前栈底地址：TRAPFRAME - old_ustack_npage * PGSIZE
    uint64 current_stack_bottom = TRAPFRAME - old_ustack_npage * PGSIZE;
    
    // 检查故障地址是否在栈扩展范围内
    // 故障地址应该低于当前栈底，但不能太远（在合理范围内）
    if (fault_addr < current_stack_bottom && fault_addr >= TRAPFRAME - PGSIZE * 10) {
        // 计算需要扩展到的栈底（页对齐）
        uint64 target_stack_bottom = PGROUNDDOWN(fault_addr);
        
        // 如果目标地址低于当前栈底，需要扩展栈
        if (target_stack_bottom < current_stack_bottom) {
            // 计算需要添加的页数
            uint64 pages_to_add = (current_stack_bottom - target_stack_bottom) / PGSIZE;
            
            // 逐页分配并映射
            for (uint64 i = 0; i < pages_to_add; i++) {
                uint64 new_page_va = current_stack_bottom - (i + 1) * PGSIZE;
                
                // 检查该虚拟地址是否已经映射，避免重复映射
                pte_t *pte = vm_getpte(pgtbl, new_page_va, false);
                if (pte && (*pte & PTE_V)) {
                    // 如果页面已经存在映射，跳过
                    continue;
                }
                
                // 分配物理页面
                uint64 page = (uint64)pmem_alloc(false);
                if (page == 0) {
                    // 如果分配失败，需要回滚已分配的页面
                    for (uint64 j = 0; j < i; j++) {
                        uint64 va_to_unmap = current_stack_bottom - (j + 1) * PGSIZE;
                        // 检查页面是否已映射再进行取消映射
                        pte_t *pte_to_unmap = vm_getpte(pgtbl, va_to_unmap, false);
                        if (pte_to_unmap && (*pte_to_unmap & PTE_V)) {
                            vm_unmappages(pgtbl, va_to_unmap, PGSIZE, true);
                        }
                    }
                    return -1;
                }
                
                // 清零页面内容
                memset((void*)page, 0, PGSIZE);
                
                // 映射到用户页表
                vm_mappages(pgtbl, new_page_va, page, PGSIZE, PTE_R | PTE_W | PTE_U);
            }
            
            // 返回新的栈页面数
            return old_ustack_npage + pages_to_add;
        }
    }
    
    return -1; // 失败
}

/*----------------------part-4: 用户页表管理相关----------------------*/

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表level = 3
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    if (pgtbl == NULL || level == 0)
        return;

    for (int i = 0; i < PGSIZE / (int)sizeof(pte_t); i++)
    {
        pte_t pte = pgtbl[i];
        if ((pte & PTE_V) == 0)
            continue;

        if (PTE_CHECK(pte) && level > 1)
        {
            // 这一项存放的是下一级页表页的物理地址
            destroy_pgtbl((pgtbl_t)PTE_TO_PA(pte), level - 1);
        }
        else if (pte & PTE_U)
        {
            // 用户页是从 user_region 申请的物理页, 需要归还
            pmem_free((uint64)PTE_TO_PA(pte), false);
        }

        pgtbl[i] = 0;
    }

    // 当前页表页来自内核物理页池, 处理完子项后整体释放
    pmem_free((uint64)pgtbl, true);
}

// 页表销毁
void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    pte_t *tf_pte = vm_getpte(pgtbl, TRAPFRAME, false);
    if (tf_pte && (*tf_pte & PTE_V))
    {
        uint64 tf_pa = (uint64)PTE_TO_PA(*tf_pte);
        vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, false);
        pmem_free(tf_pa, true);
    }
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false); // 不能释放，因为所有进程共用区域
    destroy_pgtbl(pgtbl, 3);
}

// 连续虚拟空间的复制
// 在uvm_copy_pgtbl中使用
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    if (begin >= end)
        return;

    uint64 start = PGROUNDDOWN(begin);
    uint64 stop = PGROUNDUP(end);

    for (uint64 va = start; va < stop; va += PGSIZE)
    {
        pte_t *pte = vm_getpte(old, va, false);
        if (pte == NULL || (*pte & PTE_V) == 0 || PTE_CHECK(*pte))
            continue;

        if (((*pte) & PTE_U) == 0)
            continue;

        uint64 pa = (uint64)PTE_TO_PA(*pte);
        int flags = (int)PTE_FLAGS(*pte);

        uint64 page = (uint64)pmem_alloc(false);
        memmove((void *)page, (const void *)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 拷贝页表 (拷贝并不包括 trapframe 和 trampoline)
// 拷贝的页表管理的物理页是原来页表的复制品
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint64 ustack_npage, mmap_region_t *mmap)
{
    uint64 heap_end = heap_top;
    if (heap_end < PGSIZE)
        heap_end = PGSIZE;
    heap_end = PGROUNDUP(heap_end);
    copy_range(old, new, PGSIZE, heap_end);

    if (ustack_npage > 0)
    {
        uint64 stack_begin = TRAPFRAME - ustack_npage * PGSIZE;
        copy_range(old, new, stack_begin, TRAPFRAME);
    }

    for (mmap_region_t *node = mmap; node != NULL; node = node->next)
    {
        uint64 begin = node->begin;
        uint64 end = begin + (uint64)node->npages * PGSIZE;
        copy_range(old, new, begin, end);
    }
}
