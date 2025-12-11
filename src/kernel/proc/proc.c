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

// 第一个用户进程
static proc_t proczero;

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
    proc_t *p = &proczero;
    memset(p, 0, sizeof(*p));

    p->pid = 1; // 第一个用户进程，给个固定 pid 即可

    // ---------- 1. 为内核栈分配一页 ----------
    // 内核栈放在“内核可分配区域”里；由于 kernel_pgtbl 对该区域做了 identity 映射，
    // 所以物理地址就是内核虚拟地址。
    uint64 kstack_pa = (uint64)pmem_alloc(true);
    assert(kstack_pa != 0, "proc_make_first: no mem for kernel stack");
    memset((void *)kstack_pa, 0, PGSIZE);
    p->kstack = kstack_pa;

    // ---------- 2. 为 trapframe 分配一页 ----------
    trapframe_t *tf = (trapframe_t *)pmem_alloc(true);
    assert(tf != NULL, "proc_make_first: no mem for trapframe");
    memset(tf, 0, PGSIZE);
    p->tf = tf;

    // ---------- 3. 创建并初始化用户页表 ----------
    p->pgtbl = proc_pgtbl_init((uint64)tf);

    // ---------- 4. 分配并映射用户代码+数据页 ----------
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

    // ---------- 8. 初始化内核态上下文 ----------
    // 从 CPU 自身上下文切换到该进程时：
    //   - 使用该进程的内核栈
    //   - 返回地址为 trap_user_return()，从而进入用户态
    p->ctx.sp = p->kstack + PGSIZE;
    p->ctx.ra = (uint64)trap_user_return;

    // ---------- 9. 把当前 CPU 绑定到 proczero 并切换过去 ----------
    cpu_t *c = mycpu();
    c->proc = p;
}

// 启动第一个进程并切换上下文
void proc_start_first()
{
    proc_t *p = &proczero;
    cpu_t *c = mycpu();
    
    // 从 CPU 的 ctx 切换到进程的 ctx
    swtch(&c->ctx, &p->ctx);

    // 正常情况下不会返回到这里
}
