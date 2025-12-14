#include "mod.h"

// 这个文件通过make build生成, 是proczero对应的ELF文件
#include "../../user/initcode.h"
#define initcode     target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t *old, context_t *new);

// in trap/trap_user.c
extern void trap_user_return();

// in mem/kvm.c
extern void kvm_clone_kernel_map(pgtbl_t dst);
/* ------------本地变量----------- */

// 进程结构体数组 + 第一个用户进程的指针
static proc_t proc_list[N_PROC];
static proc_t *proczero;

// 全局pid + 保护它的锁
static int global_pid;
static spinlock_t pid_lk;

/* 获取一个pid */
static int __attribute__((unused)) alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&pid_lk);
    assert(global_pid > 0, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&pid_lk);
    return tmp;
}

/* 释放进程锁 + trap_user_return */
static void __attribute__((unused)) proc_return()
{
    proc_t *p = myproc();
    assert(p != NULL, "proc_return: no current proc");
    spinlock_release(&p->lk);
    trap_user_return();
}

/* 进程模块初始化 */
void proc_init()
{
    memset(proc_list, 0, sizeof(proc_list));
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];
        spinlock_init(&p->lk, "proc");
        p->state = UNUSED;
        p->kstack = KSTACK(i);
    }
    proczero = NULL;
    global_pid = 1;
    spinlock_init(&pid_lk, "pid");
}

/* 
    申请一个UNUSED进程结构体(返回时带锁)
    并执行通用的初始化逻辑
*/
proc_t *proc_alloc()
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];

        // 当前CPU已经持有的进程锁直接跳过，避免再次获取导致自旋锁检测报错
        if (spinlock_holding(&p->lk))
            continue;

        spinlock_acquire(&p->lk);
        if (p->state != UNUSED) {
            spinlock_release(&p->lk);
            continue;
        }

        p->pid = alloc_pid();
        p->parent = NULL;
        p->exit_code = 0;
        p->sleep_space = NULL;
        p->heap_top = 0;
        p->ustack_npage = 0;
        p->mmap = NULL;
        memset(p->name, 0, sizeof(p->name));

        // 内核栈采用固定虚拟地址，缺页时补充物理页并清空栈空间
        pgtbl_t kroot = kvm_get_root();
        pte_t *pte = vm_getpte(kroot, p->kstack, false);
        if (pte == NULL || (*pte & PTE_V) == 0 || PTE_CHECK(*pte)) {
            uint64 pa = (uint64)pmem_alloc(true);
            assert(pa != 0, "proc_alloc: no mem for kstack");
            memset((void *)pa, 0, PGSIZE);
            vm_mappages(kroot, p->kstack, pa, PGSIZE, PTE_R | PTE_W);
        }
        memset((void *)p->kstack, 0, PGSIZE);

        // trapframe + 用户页表初始化
        p->tf = (trapframe_t *)pmem_alloc(true);
        assert(p->tf != NULL, "proc_alloc: no mem for trapframe");
        memset(p->tf, 0, PGSIZE);
        p->pgtbl = proc_pgtbl_init((uint64)p->tf);

        // 将同一物理页的内核栈映射到用户页表，U 位置零，便于陷阱入口在切换 satp 前切到内核栈
        pte = vm_getpte(kroot, p->kstack, false);
        assert(pte && (*pte & PTE_V) && !PTE_CHECK(*pte), "proc_alloc: kstack pte missing");
        uint64 kpa = (uint64)PTE_TO_PA(*pte);
        vm_mappages(p->pgtbl, p->kstack, kpa, PGSIZE, PTE_R | PTE_W);

        // 进程上下文把 sp 放到内核栈顶，ra 跳到 proc_return
        memset(&p->ctx, 0, sizeof(p->ctx));
        p->ctx.sp = p->kstack + PGSIZE;
        p->ctx.ra = (uint64)proc_return;

        p->state = RUNNABLE;
        return p;
    }

    return NULL;
}

/* 
    回收一个进程结构体并释放它包含的资源
    tips: 调用者需要持有进程锁
*/
void proc_free(proc_t *p)
{
    assert(spinlock_holding(&p->lk), "proc_free: lock not held");

    // 回收用户页表与其中的用户物理页、trapframe
    bool tf_recycled = false;
    if (p->pgtbl) {
        // 先去掉用户页表中的内核栈映射，物理页稍后统一回收
        pte_t *upte = vm_getpte(p->pgtbl, p->kstack, false);
        if (upte && (*upte & PTE_V) && !PTE_CHECK(*upte)) {
            vm_unmappages(p->pgtbl, p->kstack, PGSIZE, false);
        }
        uvm_destroy_pgtbl(p->pgtbl);
        p->pgtbl = NULL;
        tf_recycled = true;
    }
    if (p->tf && !tf_recycled) {
        pmem_free((uint64)p->tf, true);
    }
    p->tf = NULL;

    // 清理 mmap 元数据，用户页已在页表销毁时释放
    while (p->mmap) {
        mmap_region_t *node = p->mmap;
        p->mmap = node->next;
        mmap_region_free(node);
    }

    // 释放内核栈物理页并解除映射，避免复用时残留旧栈内容
    pgtbl_t kroot = kvm_get_root();
    if (p->kstack) {
        pte_t *kpte = vm_getpte(kroot, p->kstack, false);
        if (kpte && (*kpte & PTE_V) && !PTE_CHECK(*kpte)) {
            uint64 pa = (uint64)PTE_TO_PA(*kpte);
            vm_unmappages(kroot, p->kstack, PGSIZE, false);
            pmem_free(pa, true);
        }
    }

    p->pid = 0;
    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_npage = 0;
    memset(p->name, 0, sizeof(p->name));
    memset(&p->ctx, 0, sizeof(p->ctx));
    p->state = UNUSED;
}

static bool __attribute__((unused)) verify_page_equal(void *pa_a, void *pa_b)
{
    uint8 *lhs = (uint8 *)pa_a;
    uint8 *rhs = (uint8 *)pa_b;
    for (uint32 i = 0; i < PGSIZE; i++)
    {
        if (lhs[i] != rhs[i])
            return false;
    }
    return true;
}

static bool __attribute__((unused)) verify_range_equal(pgtbl_t a, pgtbl_t b, uint64 begin, uint64 end)
{
    if (begin >= end)
        return true;

    uint64 start = PGROUNDDOWN(begin);
    uint64 stop = PGROUNDUP(end);

    for (uint64 va = start; va < stop; va += PGSIZE)
    {
        pte_t *pte_a = vm_getpte(a, va, false);
        pte_t *pte_b = vm_getpte(b, va, false);

        bool valid_a = pte_a && (*pte_a & PTE_V) && !PTE_CHECK(*pte_a) && ((*pte_a) & PTE_U);
        bool valid_b = pte_b && (*pte_b & PTE_V) && !PTE_CHECK(*pte_b) && ((*pte_b) & PTE_U);

        if (valid_a != valid_b)
            return false;

        if (valid_a && !verify_page_equal((void *)PTE_TO_PA(*pte_a), (void *)PTE_TO_PA(*pte_b)))
            return false;
    }

    return true;
}

static void __attribute__((unused)) run_pgtbl_checks(proc_t *p)
{
    printf("[pgtbl-check] begin\n");

    uint64 old_heap_top = p->heap_top;
    uint64 heap_begin = old_heap_top;
    uint64 heap_end = old_heap_top;
    uint64 stack_pages = p->ustack_npage;
    uint64 stack_base = TRAPFRAME - stack_pages * PGSIZE;
    uint64 heap_grow_len = 2 * PGSIZE;
    uint32 mmap_pages = 2;

    bool heap_extended = false;
    bool mmap_ready = false;

    uint64 mmap_addr = 0;
    uint8 *pattern = NULL;
    void **stack_backup = NULL;
    uint64 stack_saved = 0;
    pgtbl_t copied = NULL;

    pattern = (uint8 *)pmem_alloc(true);
    if (pattern == NULL)
    {
        printf("[pgtbl-check] pattern alloc fail\n");
        goto cleanup;
    }

    uint64 new_top = uvm_heap_grow(p->pgtbl, old_heap_top, (uint32)heap_grow_len);
    if (new_top == (uint64)-1)
    {
        printf("[pgtbl-check] heap grow fail\n");
        goto cleanup;
    }
    heap_extended = true;
    p->heap_top = new_top;
    heap_end = new_top;

    for (uint64 off = heap_begin; off < heap_end; off += PGSIZE)
    {
        uint8 fill = (uint8)(0x11 + ((off - heap_begin) / PGSIZE));
        memset(pattern, fill, PGSIZE);
        uvm_copyout(p->pgtbl, off, (uint64)pattern, PGSIZE);
    }

    mmap_addr = uvm_mmap(0, mmap_pages, PTE_R | PTE_W);
    if (mmap_addr == 0)
    {
        printf("[pgtbl-check] mmap fail\n");
        goto cleanup;
    }
    mmap_ready = true;

    for (uint32 i = 0; i < mmap_pages; i++)
    {
        uint8 fill = (uint8)(0x31 + i);
        memset(pattern, fill, PGSIZE);
        uvm_copyout(p->pgtbl, mmap_addr + (uint64)i * PGSIZE, (uint64)pattern, PGSIZE);
    }

    stack_backup = (void **)pmem_alloc(true);
    if (stack_backup == NULL)
    {
        printf("[pgtbl-check] stack backup alloc fail\n");
        goto cleanup;
    }
    memset(stack_backup, 0, PGSIZE);

    for (uint64 i = 0; i < stack_pages; i++)
    {
        void *page = pmem_alloc(true);
        stack_backup[i] = page;
        stack_saved++;
        uvm_copyin(p->pgtbl, (uint64)page, stack_base + i * PGSIZE, PGSIZE);
        uint8 fill = (uint8)(0x51 + i);
        memset(pattern, fill, PGSIZE);
        uvm_copyout(p->pgtbl, stack_base + i * PGSIZE, (uint64)pattern, PGSIZE);
    }

    trapframe_t *tmp_tf = (trapframe_t *)pmem_alloc(true);
    copied = proc_pgtbl_init((uint64)tmp_tf);
    uvm_copy_pgtbl(p->pgtbl, copied, p->heap_top, p->ustack_npage, p->mmap);

    bool heap_ok = verify_range_equal(p->pgtbl, copied, heap_begin, heap_end);
    bool stack_ok = verify_range_equal(p->pgtbl, copied, stack_base, TRAPFRAME);
    bool mmap_ok = verify_range_equal(p->pgtbl, copied, mmap_addr, mmap_addr + (uint64)mmap_pages * PGSIZE);

    uint8 expect_byte = 0x11;
    uint8 new_byte = 0x7d;
    uvm_copyout(p->pgtbl, heap_begin, (uint64)&new_byte, 1);
    uint8 copied_byte = 0;
    uvm_copyin(copied, (uint64)&copied_byte, heap_begin, 1);
    bool isolate_ok = (copied_byte == expect_byte);

    printf("[pgtbl-check] heap copy %s\n", heap_ok ? "ok" : "fail");
    printf("[pgtbl-check] stack copy %s\n", stack_ok ? "ok" : "fail");
    printf("[pgtbl-check] mmap copy %s\n", mmap_ok ? "ok" : "fail");
    printf("[pgtbl-check] copy isolation %s\n", isolate_ok ? "ok" : "fail");

    uvm_destroy_pgtbl(copied);
    copied = NULL;

    trapframe_t *tmp2 = (trapframe_t *)pmem_alloc(true);
    pgtbl_t empty = proc_pgtbl_init((uint64)tmp2);
    uvm_destroy_pgtbl(empty);
    printf("[pgtbl-check] destroy empty ok\n");

cleanup:
    if (copied)
        uvm_destroy_pgtbl(copied);

    if (stack_backup)
    {
        for (uint64 i = 0; i < stack_saved; i++)
        {
            uvm_copyout(p->pgtbl, stack_base + i * PGSIZE, (uint64)stack_backup[i], PGSIZE);
            pmem_free((uint64)stack_backup[i], true);
        }
        pmem_free((uint64)stack_backup, true);
    }

    if (mmap_ready)
    {
        if (!uvm_munmap(mmap_addr, mmap_pages))
            printf("[pgtbl-check] munmap rollback fail\n");
    }

    if (heap_extended)
    {
        uint64 shrink = p->heap_top - old_heap_top;
        uint64 ret = uvm_heap_ungrow(p->pgtbl, p->heap_top, (uint32)shrink);
        if (ret != (uint64)-1)
            p->heap_top = ret;
        else
            p->heap_top = old_heap_top;
    }

    if (pattern)
        pmem_free((uint64)pattern, true);

    printf("[pgtbl-check] end\n");
}

// 获得一个初始化过的用户页表
// 完成trapframe和trampoline的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    // 1. 分配并清空根页表（放在“内核区域”）
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: no mem for user pagetable");
    memset(pgtbl, 0, PGSIZE);

    // 2. 克隆“内核基础映射”：外设 + 内核镜像 + allocator 区 + trampoline
    kvm_clone_kernel_map(pgtbl);

    // 3. 把本进程的 trapframe 映射到用户页表中的 TRAPFRAME 位置（只给 R/W，无 PTE_U）
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);

    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trampoline  (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问

    注意: 用用户空间的地址映射需要标记 PTE_U
*/
void proc_make_first()
{
    proc_t *p = proc_alloc();
    assert(p != NULL, "proc_make_first: no free proc");
    proczero = p;

    // ---------- 1. 分配并映射用户代码+数据页 ----------
    // 空出最低的 4KB (0 ~ PGSIZE-1)，因此代码从 PGSIZE 开始
    const uint64 ucode_va = PGSIZE;

    void *ucode_pa = pmem_alloc(false); // 从“用户物理区域”分配
    assert(ucode_pa != NULL, "proc_make_first: no mem for user code");
    memset(ucode_pa, 0, PGSIZE);
    memmove(ucode_pa, initcode, initcode_len);

    // 用户代码页：R/W/X + U
    vm_mappages(p->pgtbl, ucode_va, (uint64)ucode_pa,
                PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);

    // ---------- 5. 分配并映射用户栈页 ----------
    // 栈放在 TRAPFRAME 下方一页
    const uint64 ustack_va = TRAPFRAME - PGSIZE;

    void *ustack_pa = pmem_alloc(false);
    assert(ustack_pa != NULL, "proc_make_first: no mem for user stack");
    memset(ustack_pa, 0, PGSIZE);

    // 用户栈：R/W + U
    vm_mappages(p->pgtbl, ustack_va, (uint64)ustack_pa,
                PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_npage = 1;

    // ---------- 6. 初始化 heap_top ----------
    // 暂时让 heap_top 指向 code+data 之后，后续 lab 实现 brk/sbrk 时会用到
    p->heap_top = ucode_va + PGSIZE;

    // ---------- 7. 初始化用户态寄存器 ----------
    // 用户栈指针：指向栈顶
    p->tf->sp = ustack_va + PGSIZE;
    // “用户程序入口 PC” 保存在 trapframe 的 user_to_kern_epc 字段中
    p->tf->user_to_kern_epc = ucode_va;
    
    //p->tf->a0 = TRAPFRAME;

    // ---------- 8. 初始化 mmap 链表 ----------
    p->mmap = 0;  // 初始时 mmap 链表为空

    // ---------- 9. 准备调度 ----------
    p->state = RUNNABLE;
    spinlock_release(&p->lk);

    // ---------- 10. 把当前 CPU 绑定到 proczero ----------
    cpu_t *c = mycpu();
    c->proc = p;
}

// 启动第一个进程并切换上下文
void proc_start_first()
{
    proc_t *p = proczero;
    cpu_t *c = mycpu();
    
    // 从 CPU 的 ctx 切换到进程的 ctx
    swtch(&c->ctx, &p->ctx);

    // 正常情况下不会返回到这里
}

/*
    父进程产生子进程
    UNUSED -> RUNNABLE
*/
int proc_fork()
{
    proc_t *parent = myproc();
    spinlock_acquire(&parent->lk);

    proc_t *child = proc_alloc();
    if (child == NULL) {
        spinlock_release(&parent->lk);
        return -1;
    }

    // 复制父进程 trapframe，子进程返回值置0
    memmove(child->tf, parent->tf, sizeof(trapframe_t));
    child->tf->a0 = 0;

    // 复制进程元数据
    child->parent = parent;
    child->heap_top = parent->heap_top;
    child->ustack_npage = parent->ustack_npage;
    memmove(child->name, parent->name, sizeof(child->name));

    // 复制 mmap 元数据
    child->mmap = NULL;
    mmap_region_t **tail = &child->mmap;
    for (mmap_region_t *n = parent->mmap; n != NULL; n = n->next) {
        mmap_region_t *cpy = mmap_region_alloc();
        cpy->begin = n->begin;
        cpy->npages = n->npages;
        cpy->next = NULL;
        *tail = cpy;
        tail = &cpy->next;
    }

    // 复制用户页表和其中的用户物理页
    uvm_copy_pgtbl(parent->pgtbl, child->pgtbl,
                   parent->heap_top, parent->ustack_npage, child->mmap);

    // 子进程就绪
    child->state = RUNNABLE;
    spinlock_release(&child->lk);
    spinlock_release(&parent->lk);
    return child->pid;
}

/*
    进程主动放弃CPU控制权
    RUNNING->RUNNABLE
*/
void proc_yield()
{
    proc_t *p = myproc();
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

/*
    当父进程退出时, 让它的所有子进程认proczero为父
    因为proczero永不退出, 可以回收子进程的资源
*/
static void __attribute__((unused)) proc_reparent(proc_t *parent)
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];
        if (p == parent)
            continue;
        spinlock_acquire(&p->lk);
        if (p->parent == parent) {
            p->parent = proczero;
        }
        spinlock_release(&p->lk);
    }
}

/*
    唤醒等待呼叫的进程
    由proc_exit调用
    tips: 调用者需要持有p的进程锁
*/
static void __attribute__((unused)) proc_try_wakeup(proc_t *p)
{
    if (p == NULL)
        return;
    spinlock_acquire(&p->lk);
    if (p->state == SLEEPING && p->sleep_space == p)
        p->state = RUNNABLE;
    spinlock_release(&p->lk);
}

/*
    进程退出
    RUNNING -> ZOMBIE
*/
void proc_exit(int exit_code)
{
    proc_t *p = myproc();

    // 记录退出状态
    spinlock_acquire(&p->lk);
    p->exit_code = exit_code;

    // 处理过继
    proc_reparent(p);

    // 唤醒父进程
    proc_try_wakeup(p->parent);

    // 标记为ZOMBIE并切换到调度器，不再返回
    p->state = ZOMBIE;
    proc_sched();
    panic("proc_exit: unreachable");
}

/*
    父进程等待一个子进程进入ZOMBIE状态
    1. 如果等到: 释放子进程, 返回子进程的pid, 将子进程的退出状态传出到user_addr
    2. 如果发现没孩子: 返回-1
    3. 如果没等到: 父进程进入睡眠状态 
*/
int proc_wait(uint64 user_addr)
{
    proc_t *cur = myproc();

    while (1) {
        int have_child = 0;
        for (int i = 0; i < N_PROC; i++) {
            proc_t *p = &proc_list[i];
            spinlock_acquire(&p->lk);
            if (p->parent == cur) {
                have_child = 1;
                if (p->state == ZOMBIE) {
                    int code = p->exit_code;
                    int pid = p->pid;
                    if (user_addr != 0)
                        uvm_copyout(cur->pgtbl, user_addr, (uint64)&code, sizeof(int));
                    proc_free(p);
                    spinlock_release(&p->lk);
                    return pid;
                }
            }
            spinlock_release(&p->lk);
        }

        if (!have_child)
            return -1;

        spinlock_acquire(&cur->lk);
        proc_sleep(cur, &cur->lk);
        spinlock_release(&cur->lk);
    }
}

/*
    进程等待sleep_space对应的资源, 进入睡眠状态
    RUNNING -> SLEEPING
*/
void proc_sleep(void *sleep_space, spinlock_t *lock)
{
    proc_t *p = myproc();
    assert(p != NULL, "proc_sleep: no current proc");
    assert(lock != NULL && spinlock_holding(lock), "proc_sleep: lock not held");

    if (lock != &p->lk) {
        spinlock_acquire(&p->lk);
        spinlock_release(lock);
    }

    p->sleep_space = sleep_space;
    p->state = SLEEPING;
    proc_sched();

    p->sleep_space = NULL;
    if (lock != &p->lk) {
        spinlock_release(&p->lk);
        spinlock_acquire(lock);
    }
}

/*
    唤醒所有等待sleep_space的进程
    SLEEPING -> RUNNABLE
*/
void proc_wakeup(void *sleep_space)
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];
        spinlock_acquire(&p->lk);
        if (p->state == SLEEPING && p->sleep_space == sleep_space)
            p->state = RUNNABLE;
        spinlock_release(&p->lk);
    }
}

/* 
    用户进程切换到调度器
    tips: 调用者保证持有当前进程的锁
*/
void proc_sched()
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;
    assert(p != NULL, "proc_sched: no current proc");
    assert(spinlock_holding(&p->lk), "proc_sched: need lock");
    assert(intr_get() == 0, "proc_sched: interrupts must be off");
    c->proc = NULL;
    spinlock_release(&p->lk);
    swtch(&p->ctx, &c->ctx);
    // scheduler reacquires p->lk before switching us back in, so the lock remains held
}

/* 
    调度器
    RUNNABLE->RUNNING
*/
void proc_scheduler()
{
    cpu_t *c = mycpu();
    c->proc = NULL;

    while (1) {
        intr_on();
        for (int i = 0; i < N_PROC; i++) {
            proc_t *p = &proc_list[i];
            spinlock_acquire(&p->lk);
            if (p->state == RUNNABLE) {
                p->state = RUNNING;
                c->proc = p;
                swtch(&c->ctx, &p->ctx);
                c->proc = NULL;
                // proc_sched/exit is responsible for releasing the process lock,
                // so the scheduler does not touch it here.
            } else {
                spinlock_release(&p->lk);
            }
        }
    }
}
