#include "mod.h"

// 内核页表
static pgtbl_t kernel_pgtbl;

// 根据 pagetable 找到 va 对应的 pte
// 若 alloc=true 且中间页表不存在，就分配一个“页表页”（只置 V）
// 成功返回 PTE 地址，失败返回 NULL
pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    if (va >= VA_MAX) return NULL;

    pgtbl_t cur = pgtbl;
    for (int level = 2; level > 0; level--) {
        uint64 idx = VA_TO_VPN(va, level);
        pte_t *entry = &cur[idx];

        if (!(*entry & PTE_V)) {
            if (!alloc) return NULL;
            void *newpg = pmem_alloc(true);    // 页表页从内核物理区分配
            memset(newpg, 0, PGSIZE);
            *entry = PA_TO_PTE(newpg) | PTE_V; // 中间层页表页：仅 V
        } else {
            // 中间层必须是“页表页”（R/W/X 全 0）
            assert(PTE_CHECK(*entry), "vm_getpte: middle-level not a pgtbl page");
        }

        cur = (pgtbl_t)PTE_TO_PA(*entry);
    }
    // level-0
    return &cur[VA_TO_VPN(va, 0)];
}

// 在 pgtbl 中建立 [va, va + len) -> [pa, pa + len) 的映射
// 检查: va/pa 页对齐, len > 0, va + len <= VA_MAX
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    assert((va % PGSIZE) == 0 && (pa % PGSIZE) == 0, "vm_mappages: not aligned");
    assert(len > 0 && va + len <= VA_MAX, "vm_mappages: range");

    uint64 a = va, last = va + len - 1;
    while (1) {
        pte_t *pte = vm_getpte(pgtbl, a, true);
        assert(pte != NULL, "vm_mappages: no pte");
        assert(!(*pte & PTE_V), "vm_mappages: remap");

        *pte = PA_TO_PTE(pa) | perm | PTE_V;

        if (a >= PGROUNDDOWN(last)) break;
        a += PGSIZE; pa += PGSIZE;
    }
}

// 解除 pgtbl 中 [va, va+len) 的映射
// 如果 freeit == true 则释放对应物理页（默认回收到“用户物理区”）
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    assert((va % PGSIZE) == 0 && len > 0, "vm_unmappages: bad args");

    uint64 a = va, last = va + len - 1;
    while (1) {
        pte_t *pte = vm_getpte(pgtbl, a, false);
        assert(pte && (*pte & PTE_V) && !PTE_CHECK(*pte), "vm_unmappages: not mapped");

        uint64 pa = (uint64)PTE_TO_PA(*pte);
        if (freeit) pmem_free(pa, false);   // 视为用户物理页回收
        *pte = 0;

        if (a >= PGROUNDDOWN(last)) break;
        a += PGSIZE;
    }
}

// 完成 UART、CLINT、PLIC、内核代码区/数据区、可分配区域的页表映射
// 相当于部分填充 kernel_pgtbl
static void map_kernel_and_io(pgtbl_t pt)
{
    // 设备 MMIO：RW
#ifdef UART_BASE
    vm_mappages(pt, (uint64)UART_BASE,  (uint64)UART_BASE,  PGSIZE,   PTE_R|PTE_W);
#endif
#ifdef CLINT_BASE
    vm_mappages(pt, (uint64)CLINT_BASE, (uint64)CLINT_BASE, 0x10000,  PTE_R|PTE_W);
#endif
#ifdef PLIC_BASE
    vm_mappages(pt, (uint64)PLIC_BASE,  (uint64)PLIC_BASE,  0x400000, PTE_R|PTE_W);
#endif

    // 内核镜像（教学阶段简化：整段 RWX；若你们有 etext，可把 text 单独 RX）
#ifndef KERNEL_BASE
#define KERNEL_BASE 0x80000000UL
#endif
    uint64 k_begin = (uint64)KERNEL_BASE;
    uint64 k_end   = (uint64)KERNEL_DATA;   // 老师头文件一般提供 KERNEL_DATA
    if (k_end > k_begin)
        vm_mappages(pt, k_begin, k_begin, k_end - k_begin, PTE_R|PTE_W|PTE_X);

    // 数据/可分配区域：RW
    uint64 ab = PGROUNDUP((uint64)ALLOC_BEGIN);
    uint64 ae = PGROUNDDOWN((uint64)ALLOC_END);
    if (ae > ab)
        vm_mappages(pt, ab, ab, ae - ab, PTE_R|PTE_W);
}

// 相当于“new 一个内核根页表 + 对等映射”
void kvm_init()
{
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(kernel_pgtbl, "kvm_init: no mem for root pgtbl");
    memset(kernel_pgtbl, 0, PGSIZE);
    map_kernel_and_io(kernel_pgtbl);
}

// 每个 CPU 都需要调用, 从不使用页表切换到使用内核页表
// 切换后需要刷新 TLB 里面的缓存
void kvm_inithart()
{
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();
}

// 输出页表内容(for debug)
void vm_print(pgtbl_t pgtbl)
{
    // 顶级页表，次级页表，低级页表
    pgtbl_t pgtbl_2 = pgtbl, pgtbl_1 = NULL, pgtbl_0 = NULL;
    pte_t pte;

    printf("level-2 pgtbl: pa = %p\n", pgtbl_2);
    for (int i = 0; i < PGSIZE / sizeof(pte_t); i++)
    {
        pte = pgtbl_2[i];
        if (!((pte)&PTE_V))
            continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (1)");
        pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);

        for (int j = 0; j < PGSIZE / sizeof(pte_t); j++)
        {
            pte = pgtbl_1[j];
            if (!((pte)&PTE_V))
                continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (2)");
            pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_2);

            for (int k = 0; k < PGSIZE / sizeof(pte_t); k++)
            {
                pte = pgtbl_0[k];
                if (!((pte)&PTE_V))
                    continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n", k, (uint64)PTE_TO_PA(pte), (int)PTE_FLAGS(pte));
            }
        }
    }
}
