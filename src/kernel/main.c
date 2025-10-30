#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

// ========================= 可切换的测试开关 =========================
// 说明：把 0/1 改一下即可控制要跑的用例（默认跑老师最常用的两项）
#define RUN_PMEM_TEST_OOM        0   // 阶段一 Test1：耗尽内存（会 panic，默认关）
#define RUN_PMEM_TEST_USER       1   // 阶段一 Test2：用户区常规分配/释放/归零（默认开）
#define RUN_VM_TEST_DEMO_PRINT   0   // 阶段二 Test1：演示性映射打印（容易刷屏，默认关）
#define RUN_VM_TEST_MAP_UNMAP    1   // 阶段二 Test3：映射+解除映射校验（默认开）

static inline int cpuid(void) { return r_tp(); }

// ----------（可选）打印权限位的小工具 ----------
static void print_flags(int perm) {
    printf("%c%c%c",
        (perm & PTE_R) ? 'R' : '-',
        (perm & PTE_W) ? 'W' : '-',
        (perm & PTE_X) ? 'X' : '-');
}

// ========================= 阶段一：物理内存管理 =========================

// Test1：耗尽内存（README 截图用，跑它会触发 panic，交付默认关闭）
#if RUN_PMEM_TEST_OOM
static void pmem_test_oom(void) {
    printf("=== pmem_test_oom (will panic by design) ===\n");
    for (;;) {
        void *p = pmem_alloc(true);  // 想测用户区把 true 改成 false
        // *((volatile unsigned char*)p) = 0xA5; // 可选小写入
    }
}
#endif

// Test2：常规申请/释放/归零（与老师样例对齐）
extern alloc_region_t user_region;  // mem/mod.h 若未对外声明，这里补 extern
static void pmem_test_user(void) {
    printf("=== pmem_test_user ===\n");
    const int N = 10;
    uint64 pages[N]; for (int i = 0; i < N; i++) pages[i] = 0;

    // Phase 1: Allocate
    printf("=== test_case_2: Phase 1 - Allocate User Pages ===\n");
    for (int i = 0; i < N; i++) {
        pages[i] = (uint64)pmem_alloc(false);
        printf("Allocated user page[%d] @ %p\n", i, (void*)pages[i]);
        // 简单边界检查
        assert(pages[i] >= user_region.begin && pages[i] < user_region.end,
               "pmem_test_user: page out of user region");
        memset((void*)pages[i], 0xAA, PGSIZE);
    }

    // Phase 2: Pre-free check
    printf("=== test_case_2: Phase 2 - Pre-free Check ===\n");
    spinlock_acquire(&user_region.lk);
    int expected_before = (user_region.end - user_region.begin)/PGSIZE - N;
    int actual_before   = user_region.allocable;
    printf("Expected allocable: %d, Actual: %d\n", expected_before, actual_before);
    assert(actual_before == expected_before, "Allocable count incorrect before free");
    spinlock_release(&user_region.lk);

    // Phase 3: Free
    printf("=== test_case_2: Phase 3 - Free Pages ===\n");
    for (int i = 0; i < N; i++) {
        pmem_free(pages[i], false);
        printf("Free user page[%d] @ %p\n", i, (void*)pages[i]);
    }

    // Phase 4: Post-free check
    printf("=== test_case_2: Phase 4 - Post-free Check ===\n");
    spinlock_acquire(&user_region.lk);
    int expected_after = (user_region.end - user_region.begin)/PGSIZE;
    int actual_after   = user_region.allocable;
    printf("Expected allocable: %d, Actual: %d\n", expected_after, actual_after);
    assert(actual_after == expected_after, "Allocable count not restored after free");
    spinlock_release(&user_region.lk);

    // Phase 5: Reallocate & Verify Zero
    printf("=== test_case_2: Phase 5 - Reallocate & Verify Zero ===\n");
    for (int i = 0; i < N; i++) {
        void *p = pmem_alloc(false);
        printf("Reallocated page[%d] @ %p\n", i, p);
        // 验证归零
        bool non_zero = false;
        for (int j = 0; j < (PGSIZE / (int)sizeof(int)); j++) {
            if (((int*)p)[j] != 0) { non_zero = true; break; }
        }
        assert(!non_zero, "Memory not zeroed after free");
        printf("Zero verification passed\n");
    }
    printf("test_case_2 passed!\n");
}

// ========================= 阶段二：页表（Sv39） =========================

// Test1：演示性映射 + 打印（如需和老师截图类似，打开此开关并使用你精简后的 vm_print）
#if RUN_VM_TEST_DEMO_PRINT
extern void vm_print(pgtbl_t root);
extern void kvm_clone_kernel_map(pgtbl_t dst);
static void vm_test_demo_print(void) {
    printf("=== vm_test_demo_print ===\n");
    pgtbl_t test_pgtbl = (pgtbl_t)pmem_alloc(true);
    memset((void*)test_pgtbl, 0, PGSIZE);
    kvm_clone_kernel_map(test_pgtbl); // 把内核/外设/可分配区基础映射带上

    uint64 mem[5];
    for (int i = 0; i < 5; i++) { mem[i] = (uint64)pmem_alloc(false); memset((void*)mem[i], 0, PGSIZE); }

    printf("\n[test-1]\n");
    vm_mappages(test_pgtbl, 0,                   mem[0], PGSIZE,        PTE_R);
    vm_mappages(test_pgtbl, PGSIZE * 10,         mem[1], PGSIZE/2,      PTE_R | PTE_W);
    vm_mappages(test_pgtbl, PGSIZE * 512,        mem[2], PGSIZE - 1,    PTE_R | PTE_X);
    vm_mappages(test_pgtbl, PGSIZE * 512 * 512,  mem[3], PGSIZE,        PTE_R | PTE_X);
    vm_mappages(test_pgtbl, VA_MAX - PGSIZE,     mem[4], PGSIZE,        PTE_W);
    vm_print(test_pgtbl);

    printf("\n[test-2]\n");
    vm_unmappages(test_pgtbl, PGSIZE * 10,  PGSIZE, true);
    vm_unmappages(test_pgtbl, PGSIZE * 512, PGSIZE, true);
    vm_print(test_pgtbl);
}
#endif

// Test3：映射 + 解除映射 + 校验（老师最终检查最看这个）
static void vm_test_map_unmap(void)
{
    printf("=== test_mapping_and_unmapping ===\n");

    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    memset((void*)pgtbl, 0, PGSIZE);
    printf("new pgtbl @ %p\n", pgtbl);

    uint64 va_1 = 0x00100000;      // 1MB
    uint64 va_2 = 0x00008000;      // 32KB
    uint64 pa_1 = (uint64)pmem_alloc(false);
    uint64 pa_2 = (uint64)pmem_alloc(false);
    printf("alloc pa_1=%p pa_2=%p\n", (void*)pa_1, (void*)pa_2);

    vm_mappages(pgtbl, va_1, pa_1, PGSIZE, PTE_R | PTE_W);
    vm_mappages(pgtbl, va_2, pa_2, PGSIZE, PTE_R | PTE_W);
    printf("map: va=%p -> pa=%p flags=", (void*)va_1, (void*)pa_1); print_flags(PTE_R|PTE_W); printf("\n");
    printf("map: va=%p -> pa=%p flags=", (void*)va_2, (void*)pa_2); print_flags(PTE_R|PTE_W); printf("\n");

    pte_t *pte = vm_getpte(pgtbl, va_1, false);
    assert(pte && (*pte & PTE_V) && PTE_TO_PA(*pte) == pa_1 &&
           ((*pte & (PTE_R|PTE_W))==(PTE_R|PTE_W)), "va_1 map check failed");
    printf("check: va=%p ok -> pa=%p flags=", (void*)va_1, (void*)PTE_TO_PA(*pte)); print_flags(PTE_FLAGS(*pte)); printf("\n");

    pte = vm_getpte(pgtbl, va_2, false);
    assert(pte && (*pte & PTE_V) && PTE_TO_PA(*pte) == pa_2 &&
           ((*pte & (PTE_R|PTE_W))==(PTE_R|PTE_W)), "va_2 map check failed");
    printf("check: va=%p ok -> pa=%p flags=", (void*)va_2, (void*)PTE_TO_PA(*pte)); print_flags(PTE_FLAGS(*pte)); printf("\n");

    vm_unmappages(pgtbl, va_1, PGSIZE, true);
    vm_unmappages(pgtbl, va_2, PGSIZE, true);
    printf("unmap: va=%p len=%d\n", (void*)va_1, (int)PGSIZE);
    printf("unmap: va=%p len=%d\n", (void*)va_2, (int)PGSIZE);

    pte = vm_getpte(pgtbl, va_1, false);
    assert(pte && ((*pte & PTE_V) == 0), "va_1 still valid after unmap");
    printf("check after unmap: va=%p V=0\n", (void*)va_1);
    pte = vm_getpte(pgtbl, va_2, false);
    assert(pte && ((*pte & PTE_V) == 0), "va_2 still valid after unmap");
    printf("check after unmap: va=%p V=0\n", (void*)va_2);

    printf("test_mapping_and_unmapping passed!\n");
}

// ========================= 入口 =========================
void main(void)
{
    // 只让 CPU0 跑，避免多核并发改页表/打印冲突
    if (cpuid() != 0) { for(;;) asm volatile("wfi"); }

    print_init();
    pmem_init();
    kvm_init();
    kvm_inithart();

    printf("cpu %d is booting!\n", cpuid());

#if RUN_PMEM_TEST_OOM
    pmem_test_oom();                 // 会 panic：仅用于 README 截图
#endif
#if RUN_PMEM_TEST_USER
    pmem_test_user();
#endif
#if RUN_VM_TEST_DEMO_PRINT
    vm_test_demo_print();            // 如需页表打印截图再打开
#endif
#if RUN_VM_TEST_MAP_UNMAP
    vm_test_map_unmap();
#endif

    // 保持 QEMU 运行，便于观察；退出用 Ctrl-A 然后 X
    for(;;) asm volatile("wfi");
}

