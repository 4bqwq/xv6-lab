#include "mod.h"
#include "../proc/type.h"

//================= 链接脚本符号（按你的 kernel.ld 名字有差异就改） =================
extern char KERNEL_START[];   // 内核镜像起始
extern char KERNEL_END[];     // 内核镜像结束
extern char trampoline[];

//================= 内核根页表 ======================================================
static pgtbl_t kernel_pgtbl;

//================= 工具：构造 satp(Sv39) ==========================================
//static inline uint64 MAKE_SATP(pgtbl_t root) {
//    return (8ULL << 60) | (((uint64)root) >> 12);   // MODE=Sv39(8) | ASID=0 | PPN
//}

//================= 页表走访：返回叶子 PTE 指针 ====================================
pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    if (va >= VA_MAX) return NULL;

    pgtbl_t cur = pgtbl;
    for (int level = 2; level > 0; level--) {
        uint64 idx = VA_TO_VPN(va, level);
        pte_t *entry = &cur[idx];

        if (!(*entry & PTE_V)) {
            if (!alloc) return NULL;
            void *newpg = pmem_alloc(true);       // 中间层页表页：从内核区分配
            memset(newpg, 0, PGSIZE);
            *entry = PA_TO_PTE(newpg) | PTE_V;    // 中间层仅置 V
        } else {
            // 中间层必须是“页表页”（R/W/X 全 0）
            assert(PTE_CHECK(*entry), "vm_getpte: middle-level not a pgtbl page");
        }
        cur = (pgtbl_t)PTE_TO_PA(*entry);
    }
    return &cur[VA_TO_VPN(va, 0)];                // 叶子 PTE
}

//================= 建立映射：支持 len 非页对齐 =====================================
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    assert(len > 0 && va + len <= VA_MAX, "vm_mappages: range");
    assert((va % PGSIZE) == 0 && (pa % PGSIZE) == 0, "vm_mappages: va/pa not page-aligned");

    uint64 a    = va;
    uint64 p    = pa;
    uint64 last = PGROUNDDOWN(va + len - 1);

    for (;;) {
        pte_t *pte = vm_getpte(pgtbl, a, true);
        assert(pte != NULL, "vm_mappages: no pte");
        assert(!(*pte & PTE_V), "vm_mappages: remap");
        *pte = PA_TO_PTE(p) | perm | PTE_V;

        if (a == last) break;
        a += PGSIZE; p += PGSIZE;
    }
}

//================= 解除映射：可选释放物理页 ========================================
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    assert((va % PGSIZE) == 0 && len > 0, "vm_unmappages: bad args");

    uint64 a    = va;
    uint64 last = PGROUNDDOWN(va + len - 1);

    for (;;) {
        pte_t *pte = vm_getpte(pgtbl, a, false);
        assert(pte && (*pte & PTE_V) && !PTE_CHECK(*pte), "vm_unmappages: not mapped");

        uint64 pa = (uint64)PTE_TO_PA(*pte);
        if (freeit) pmem_free(pa, false);    // 本实验解映射的多是“用户页”，统一回收到用户区
        *pte = 0;

        if (a == last) break;
        a += PGSIZE;
    }
}

//================= 基础“内核与外设”映射（identity） ================================
static void map_kernel_and_io(pgtbl_t pt)
{
#ifdef UART_BASE
    vm_mappages(pt, (uint64)UART_BASE,  (uint64)UART_BASE,  PGSIZE,   PTE_R|PTE_W);
#endif
#ifdef CLINT_BASE
    vm_mappages(pt, (uint64)CLINT_BASE, (uint64)CLINT_BASE, 0x10000,  PTE_R|PTE_W);
#endif
#ifdef PLIC_BASE
    vm_mappages(pt, (uint64)PLIC_BASE,  (uint64)PLIC_BASE,  0x400000, PTE_R|PTE_W);
#endif

    // 内核镜像：整段 RWX（教学简化；若有 etext 可把 text 单独 RX）
    uint64 k_begin = (uint64)KERNEL_START;
    uint64 k_end   = (uint64)KERNEL_END;
    if (k_end > k_begin)
        vm_mappages(pt, k_begin, k_begin, k_end - k_begin, PTE_R|PTE_W|PTE_X);

    // 可分配物理区（allocator 管理的物理内存）：RW
    uint64 ab = PGROUNDUP((uint64)ALLOC_BEGIN);
    uint64 ae = PGROUNDDOWN((uint64)ALLOC_END);
    if (ae > ab)
        vm_mappages(pt, ab, ab, ae - ab, PTE_R|PTE_W);
    
    //trampoline: 内核和用户共享的一页跳板代码
    vm_mappages(pt, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

//================= 初始化内核根页表 & 开启分页 =====================================
void kvm_init()
{
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(kernel_pgtbl != NULL , "kvm_init: no mem for root pgtbl");
    memset(kernel_pgtbl, 0, PGSIZE);
    map_kernel_and_io(kernel_pgtbl);

    // 预先为每个进程槽位准备一页内核栈，切换时使用固定虚拟地址
    for (int i = 0; i < N_PROC; i++) {
        uint64 va = KSTACK(i);
        uint64 pa = (uint64)pmem_alloc(true);
        assert(pa != 0, "kvm_init: no mem for kstack");
        memset((void *)pa, 0, PGSIZE);
        vm_mappages(kernel_pgtbl, va, pa, PGSIZE, PTE_R | PTE_W);
    }
}

void kvm_inithart()
{
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();
}

//================= 对外辅助：导出 root；克隆“内核基础映射”到别的页表 ===============
pgtbl_t kvm_get_root(void) { return kernel_pgtbl; }

void kvm_clone_kernel_map(pgtbl_t dst)
{
    // 直接复用同一套“基础 identity 映射”
    map_kernel_and_io(dst);
}

//================= 调试：打印页表 ================================================
void vm_print(pgtbl_t pgtbl)
{

    pgtbl_t pgtbl_2 = pgtbl, pgtbl_1 = NULL, pgtbl_0 = NULL;
    pte_t pte;

    printf("level-2 pgtbl: pa = %p\n", pgtbl_2);
    int hi_slot = VA_TO_VPN(TRAPFRAME, 2);
    for (int i = 0; i < PGSIZE / sizeof(pte_t); i++) {
        if (i != 0 && i != hi_slot)
            continue;
        pte = pgtbl_2[i];
        if (!(pte & PTE_V)) continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (1)");
        pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);

        for (int j = 0; j < PGSIZE / sizeof(pte_t); j++) {
            pte = pgtbl_1[j];
            if (!(pte & PTE_V)) continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (2)");
            pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_0);

            for (int k = 0; k < PGSIZE / sizeof(pte_t); k++) {
                pte = pgtbl_0[k];
                if (!(pte & PTE_V)) continue;
                int flags = (int)PTE_FLAGS(pte);
                if ((flags & PTE_U) == 0 && i != hi_slot)
                    continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n",
                       k, (void*)PTE_TO_PA(pte), flags);
            }
        }
    }
}
