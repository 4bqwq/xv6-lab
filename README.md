# Lab4：用户态、Trampoline 与系统调用

- **A（周屹枫）**：在 Lab3 基础上完成 Lab4 的全部代码实现（用户态地址空间、trampoline、`trap_user`、系统调用路径、`initcode` 用户程序），以及本 README 撰写。
- **B（严之皓）**：提供 Lab3 代码基础与接口说明，协助对接内存/Trap 模块并参与联调与测试。

---

## 一、环境与运行方式

本实验在 Lab3 的内核框架基础上完成，仍使用 QEMU 作为模拟器。编译与运行方式：

```bash
make build && make run
````

成功后会启动 QEMU，预期输出类似如下（只展示关键部分）：

```text
===== make success! =====
qemu-system-riscv64   -machine virt -bios none -kernel target/kernel/kernel-qemu.elf   -m 128M -smp 2 -nographic  
cpu 0 is booting!
proczero: hello world!
proczero: hello world!
```

说明：

* `cpu 0 is booting!` 来自内核启动过程。
* 两行 `proczero: hello world!` 为我们在 Lab4 中实现的 **用户态系统调用 `SYS_helloworld`** 的输出（用户程序调用两次）。

开发与调试阶段，为了观察执行流程，我们一度加入过若干调试输出（例如 `trap_user_return: enter`、`trap_user_handler: scause = ...`），最终版本可以按需要保留或删去。

---

## 二、虚拟地址布局与基础映射

### 2.1 `type.h` 中的用户态布局宏

在 `src/kernel/mem/type.h` 中，我们补全/确认了与用户态相关的几个关键宏，用于统一内核与用户空间的虚拟地址布局：

```c
// 物理页大小
#define PGSIZE 4096
#define PGROUNDUP(x)   (((uint64)(x) + PGSIZE - 1) & ~(PGSIZE -1))
#define PGROUNDDOWN(x) ((uint64)(x) & ~(PGSIZE -1))

// SV39 最大虚拟地址（512GB）
#define VA_MAX (1ul << 38)

// 用户/内核共享的 trampoline 代码页: 虚拟地址空间的最高一页
#define TRAMPOLINE (VA_MAX - PGSIZE)

// trapframe 所在页: 紧挨着 TRAMPOLINE 之下的一页
#define TRAPFRAME  (TRAMPOLINE - PGSIZE)

// satp 相关
#define SATP_SV39 (8UL << 60)
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)(pagetable)) >> 12))
```

设计要点：

* **TRAMPOLINE**：最高一页，映射 `trampoline.S` 中的用户/内核切换代码（`user_vector` / `user_return`），在**内核页表和所有用户页表中虚拟地址一致**。
* **TRAPFRAME**：紧挨着 `TRAMPOLINE` 之下的一页，用于存放当前进程的 `trapframe_t`，记录用户寄存器现场和“回到内核”所需的信息。

这样设计的好处是：每个进程的“切换代码 + 上下文信息”都位于固定虚拟地址，简化 trap 流程实现。

### 2.2 `kvm.c`：trampoline 的映射

在 `src/kernel/mem/kvm.c` 的 `map_kernel_and_io()` 中，我们在原 Lab2/3 的基础映射上新增了 trampoline 页的映射：

```c
// trampoline: 内核和用户共享的一页跳板代码
vm_mappages(pt, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
```

* 物理地址为链接脚本导出的符号 `trampoline`；
* 权限为 `R|X`，不允许写；
* 此映射在 **内核根页表** 中建立，然后通过 `kvm_clone_kernel_map()` 被复制到各个用户页表，从而保证所有页表中该页的虚拟地址一致。

### 2.3 `proc_pgtbl_init()`：TRAPFRAME 映射

在 `src/kernel/proc/proc.c` 中，我们为单个用户进程创建初始页表的函数为：

```c
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: no mem for user pagetable");
    memset(pgtbl, 0, PGSIZE);

    // 1. 克隆“内核基础映射”：外设 + 内核镜像 + allocator 区 + trampoline
    kvm_clone_kernel_map(pgtbl);

    // 2. 把本进程的 trapframe 映射到用户页表中的 TRAPFRAME 位置（只给 R/W，无 PTE_U）
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);

    return pgtbl;
}
```

注意：

* 这里 **不加 `PTE_U`**，保证用户态无法直接访问 `trapframe` 页；该页只在内核和 `trampoline` 代码中使用。

---

## 三、第一个用户进程 `proczero` 的创建

### 3.1 用户地址空间总体布局

为第一个用户进程 `proczero`，我们设计的用户虚拟地址空间布局如下（高地址在上）：

```text
VA_MAX
  ...
  [ TRAMPOLINE ]    1 page, 映射 trampoline.S 代码 (R-X, 无U)
  [ TRAPFRAME  ]    1 page, 当前进程 trapframe (R/W, 无U)
  [ user stack ]    1 page, 用户栈 (R/W | U)
  ...
        ↑ heap_top
  [ code+data ]     1 page, initcode 程序 (R/W/X | U)
  [ empty guard ]   1 page, 保留，最低 4KB 不用
0
```

### 3.2 `proc_make_first()` 实现步骤

在 `src/kernel/proc/proc.c` 中，`proc_make_first()` 完成了 `proczero` 的创建与切换：

```c
void proc_make_first()
{
    proc_t *p = &proczero;
    memset(p, 0, sizeof(*p));

    p->pid = 1; // 第一个用户进程

    // ---------- 1. 为内核栈分配一页 ----------
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
    const uint64 ucode_va = PGSIZE;   // 空出最低 4KB
    void *ucode_pa = pmem_alloc(false);
    assert(ucode_pa != NULL, "proc_make_first: no mem for user code");
    memset(ucode_pa, 0, PGSIZE);
    memmove(ucode_pa, initcode, initcode_len);

    vm_mappages(p->pgtbl, ucode_va, (uint64)ucode_pa,
                PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);

    // ---------- 5. 分配并映射用户栈页 ----------
    const uint64 ustack_va = TRAPFRAME - PGSIZE;
    void *ustack_pa = pmem_alloc(false);
    assert(ustack_pa != NULL, "proc_make_first: no mem for user stack");
    memset(ustack_pa, 0, PGSIZE);

    vm_mappages(p->pgtbl, ustack_va, (uint64)ustack_pa,
                PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_npage = 1;

    // ---------- 6. 初始化 heap_top ----------
    p->heap_top = ucode_va + PGSIZE;

    // ---------- 7. 初始化用户态寄存器 ----------
    p->tf->sp               = ustack_va + PGSIZE; // 用户栈指针
    p->tf->user_to_kern_epc = ucode_va;          // 用户程序入口 PC

    // ---------- 8. 初始化内核态上下文 ----------
    p->ctx.sp = p->kstack + PGSIZE;             // 进程内核栈
    p->ctx.ra = (uint64)trap_user_return;       // 切过去后从 trap_user_return() 开始

    // ---------- 9. 绑定当前 CPU 并切换 ----------
    cpu_t *c = mycpu();
    c->proc = p;
    swtch(&c->ctx, &p->ctx);

    // 正常情况下不会再返回这里
}
```

核心逻辑：

* 在内核可分配物理区域中为内核栈和 trapframe 各分配一页；
* 通过 `proc_pgtbl_init()` 创建用户页表，并映射 `TRAPFRAME`；
* 在用户页表中映射用户代码页（从 `PGSIZE` 起）和用户栈页（在 `TRAPFRAME` 下方一页），都带 `PTE_U`；
* 通过 `trap_user_return()` 完成从内核态到用户态的第一次切换。

---

## 四、trampoline 与用户态 Trap 路径

### 4.1 `trampoline.S`：`user_vector` / `user_return`

`src/kernel/trap/trampoline.S` 中的代码位于单独的 `trampsec` 段，最终被映射到 `TRAMPOLINE` 页。主要包含两个入口：

1. **`user_vector`**：用户态 Trap 入口（用户 → 内核）
2. **`user_return`**：从内核返回用户态（内核 → 用户）

#### (1) `user_vector`：用户 → 内核

* 使用 `sscratch` 中的 `p->trapframe` 指针，将所有通用寄存器保存到 trapframe 对应偏移；
* 从 trapframe 中恢复：

  * `user_to_kern_sp` → 作为内核栈指针；
  * `user_to_kern_hartid` → 写入 `tp` 寄存器，保证 `mycpu()` 能正常工作；
  * `user_to_kern_trapvector` → 作为 C 入口 `trap_user_handler`；
  * `user_to_kern_satp` → 写入 `satp` 并 `sfence.vma`，切换到内核页表；
* 最后 `jr t0` 进入 `trap_user_handler()`。

#### (2) `user_return`：内核 → 用户

* 参数为 `(trapframe, satp)`：

  * 切换到用户页表：`csrw satp, a1; sfence.vma`；
  * 设置 `sscratch = trapframe`，以便下一次用户→内核 Trap 使用；
  * 从 trapframe 中依次恢复所有通用寄存器；
  * 依赖 C 代码预先设置好的 `sepc` / `sstatus`；
  * 执行 `sret`，回到用户态，从用户 PC 继续执行。

---

## 五、用户态 Trap 处理与系统调用

### 5.1 `trap_user_handler()`：用户态 Trap 的 C 入口

`src/kernel/trap/trap_user.c` 中，我们实现了用户态 Trap 的主处理函数：

```c
void trap_user_handler(void)
{
    uint64 sepc    = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause  = r_scause();
    uint64 stval   = r_stval();

    // 必须来自 U-mode，且当前 S-mode 中断关闭
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from user mode");
    assert(intr_get() == 0, "trap_user_handler: interrupts enabled");

    // 进入内核后，将后续 Trap 入口切换为 kernel_vector
    w_stvec((uint64)kernel_vector);

    cpu_t *c = mycpu();
    assert(c && c->proc && c->proc->tf, "trap_user_handler: no current proc");
    proc_t      *p  = c->proc;
    trapframe_t *tf = p->tf;

    // 记录用户态 PC，后续返回用
    tf->user_to_kern_epc = sepc;

    int trap_id = scause & 0xf;

    if (scause & 0x8000000000000000UL) {
        // 1. 中断：转发到 Lab3 的中断处理逻辑
        switch (trap_id) {
        case 5:  timer_interrupt_handler();    break;  // S-mode timer interrupt
        case 9:  external_interrupt_handler(); break;  // S-mode external interrupt
        default:
            // 未知中断，打印信息后 panic
            ...
        }
    } else {
        // 2. 异常：Lab4 只处理 U-mode ecall，用作系统调用入口
        switch (trap_id) {
        case 8: {    // ecall from U-mode

            uint64 sysnum = tf->a7;    // RISC-V 约定：syscall 号在 a7

            if (sysnum == SYS_helloworld) {
                printf("proczero: hello world!\n");
            } else {
                // 本 Lab 只实现一个 syscall
                ...
            }

            // ecall 返回时 PC 要跳过 ecall 指令
            tf->user_to_kern_epc += 4;
            break;
        }
        default:
            ...
        }
    }

    // 处理完毕，返回用户态
    trap_user_return();
}
```

要点：

* 利用 `scause` 高位区分中断/异常，低位编号区分具体类型；
* 中断（timer / external）仍然走 Lab3 的已有框架；
* 异常中只实现了 `ecall from U-mode`，通过读取 `a7` 作为系统调用号；
* 对于本 Lab4，只实现 `SYS_helloworld` 一个 syscall，其他 syscall 直接视为错误并 panic。

### 5.2 `trap_user_return()`：内核态 → 用户态

```c
void trap_user_return(void)
{
    cpu_t *c = mycpu();
    assert(c && c->proc && c->proc->tf, "trap_user_return: no current proc");

    proc_t      *p  = c->proc;
    trapframe_t *tf = p->tf;

    // 1. 关中断，避免切换过程中被打断
    intr_off();

    // 2. 将后续来自用户态的 Trap 入口设置为 trampoline.S 中的 user_vector
    uint64 uservec = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);
    w_stvec(uservec);

    // 3. 填写 trapframe 中的“返回内核”信息（供 user_vector 使用）
    tf->user_to_kern_satp       = r_satp();                  // 当前内核页表
    tf->user_to_kern_sp         = p->kstack + PGSIZE;        // 进程内核栈顶
    tf->user_to_kern_trapvector = (uint64)trap_user_handler; // 下次 U->S Trap 的 C 入口
    tf->user_to_kern_hartid     = r_tp();                    // 用于 mycpu()

    // 4. 设置 sstatus：让 sret 回到 U-mode 并打开 U-mode 中断
    uint64 x = r_sstatus();
    x &= ~SSTATUS_SPP;   // SPP = 0 -> U-mode
    x |=  SSTATUS_SPIE;  // SPIE = 1 -> sret 后 U 模式中断开启
    w_sstatus(x);

    // 5. 设置 sepc：下一次在用户态执行的 PC
    w_sepc(tf->user_to_kern_epc);

    // 6. 准备切换到用户页表
    uint64 satp = MAKE_SATP(p->pgtbl);

    // 7. 跳转到 user_return(TRAPFRAME, satp)
    uint64 userret = TRAMPOLINE + ((uint64)user_return - (uint64)trampoline);
    ((void (*)(uint64, uint64))userret)(TRAPFRAME, satp);

    // 不会再返回这里
}
```

通过以上步骤，实现了从内核态返回用户态的标准流程，并为后续用户态再次发生 Trap（中断/系统调用）预置好必要的信息。

---

## 六、系统调用接口与用户态测试程序

### 6.1 用户态 syscall 封装

在 `src/user/sys.h` 和 `src/user/syscall_arch.h` 中，已经提供了通用的 syscall 封装，遵循 RISC-V 调用约定：

* syscall 号放在 `a7`；
* 参数依次放在 `a0`~`a5`；
* 调用 `ecall`；
* 返回值从 `a0` 取出。

`sys.h` 利用宏自动根据参数个数展开到 `__syscallN`：

```c
#define __syscall(...) __SYSCALL_DISP(__syscall, __VA_ARGS__)
#define syscall(...)   __syscall(__VA_ARGS__)
```

### 6.2 系统调用号定义

在 `src/user/syscall_num.h` 中，我们为 Lab4 定义了唯一的系统调用号：

```c
#define SYS_helloworld 0   // 约定: 内核应该输出 "helloworld"
```

### 6.3 用户态 `initcode` 程序

`src/user/initcode.c` 中，我们编写了极简用户态测试程序：

```c
#include "sys.h"

int main()
{
    syscall(SYS_helloworld);
    syscall(SYS_helloworld);
    while (1)
        ;
    return 0;
}
```

在 `proc_make_first()` 中，我们将编译好的 `initcode` ELF 裁剪后的镜像复制进用户的 `code+data` 页，并将 `user_to_kern_epc` 初始化为该页的起始虚拟地址 `PGSIZE`，从而实现：

1. 内核启动完成后调用 `proc_make_first()`；
2. 切换到用户态执行 `initcode`；
3. 用户态发出两次 `SYS_helloworld` 系统调用；
4. 内核在 `trap_user_handler()` 中识别 syscall 号并打印两行 `"proczero: hello world!"`。

---

## 七、测试与调试过程

### 7.1 正常路径验证

通过 `make build && make run`，观察 QEMU 输出：

* 出现 `cpu 0 is booting!` 表示内核启动流程正常；
* 紧接着出现两行 `proczero: hello world!` 表明：

  * 用户态 `ecall` 成功进入 `user_vector` → `trap_user_handler()`；
  * 系统调用号解析和打印逻辑正确；
  * `trap_user_return()` 和 `user_return()` 整体路径正常，可以返回用户态继续执行。

### 7.2 调试信息

开发调试阶段，我们曾加入以下调试信息（可按需要保留或删除）：

* 在 `trap_user_handler()` 中打印 `scause`，确认确实收到了异常号 8（`ecall from U-mode`）；
* 在 `trap_user_return()` 中打印标记字符串（例如 `trap_user_return: enter`），用来确认执行到了返回路径；
* 在必要时，通过 GDB 或更多 `printf` 检查 `satp`、`sepc`、`user_to_kern_epc` 等寄存器与字段是否合理。

---

## 八、总结与体会

在 Lab3 的基础上，Lab4 将整个 Trap 体系从“内核态单向响应中断”扩展为“用户态与内核态双向切换”的完整闭环，主要收获包括：

1. **理解 RISC-V 模式切换机制**
   通过操作 `sstatus`、`sepc`、`satp` 等寄存器，真正走了一遍从 U-mode → S-mode （Trap）再回到 U-mode 的路径。

2. **掌握统一虚拟地址下的 trampoline 设计思想**
   将 `TRAMPOLINE` / `TRAPFRAME` 固定在虚拟地址空间顶部，并在内核/用户页表中共享，极大简化了用户/内核切换代码的实现。

3. **完成第一个用户进程的构造**
   不再停留在“用户程序”这一抽象层面，而是从页表映射、栈初始化、上下文切换等细节出发，亲手“加载”并运行了第一个用户进程。

4. **打通系统调用最小路径**
   利用 `ecall` + `a7` 约定，实现了一个最小可用的 syscall：`SYS_helloworld`。虽然功能很简单，但为后续扩展更多系统调用接口（如文件、进程管理等）奠定了完整的框架。

整体而言，Lab4 把“操作系统能跑用户程序”这句话拆解为一系列非常具体的工程步骤，对内核与用户之间的边界和交互方式有了更清晰的认识，也为后续实验（如多进程、内存管理、更多 syscall）提供了坚实基础。

