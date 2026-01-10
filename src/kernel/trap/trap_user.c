#include "mod.h"
#include "../syscall/mod.h"   // 间接包含 ../syscall/type.h
#include "../mem/type.h"
#include "../mem/mod.h"
#include "../proc/mod.h"

// in trampoline.S
extern char trampoline[];  // 内核和用户切换的代码
extern char user_vector[]; // 用户触发陷阱进入内核
extern char user_return[]; // 内核处理完毕返回用户

// in trap.S
extern char kernel_vector[]; // 内核态 trap 处理流程, 进入内核后应当切换中断处理入口

// in trap_kernel.c
extern char *interrupt_info[16]; // 中断错误信息
extern char *exception_info[16]; // 异常错误信息

// lab3: 中断处理函数
void external_interrupt_handler(void);
void timer_interrupt_handler(void);

// 提前声明，否则前面调用 trap_user_return() 会触发 implicit-function-declaration
void trap_user_return(void);

// 在 user_vector() 里面调用
// 用户态 trap 处理的核心逻辑
void trap_user_handler(void)
{
    uint64 sepc    = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause  = r_scause();
    uint64 stval   = r_stval();
    
    //trial
    //printf("trap_user_handler: scause = %lx\n", scause);

    // 必须来自 U-mode，且此时 S-mode trap 是关闭的
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from user mode");
    assert(intr_get() == 0, "trap_user_handler: interrupts enabled");

    cpu_t *c = mycpu();
    assert(c && c->proc && c->proc->tf, "trap_user_handler: no current proc");
    proc_t      *p  = c->proc;
    trapframe_t *tf = p->tf;

    // 记录发生 trap 时用户态的 PC，方便返回
    tf->user_to_kern_epc = sepc;

    int trap_id = scause & 0xf;

    if (scause & 0x8000000000000000UL) {
        // 1. 中断
        switch (trap_id) {
        case 1: // S-mode software interrupt (timer via SSIP)
        case 5: // S-mode timer interrupt
            timer_interrupt_handler();
            // 用户态被时钟打断，交出CPU让调度器决定下一个运行者
            if (c->proc != NULL && c->proc->state == RUNNING)
                proc_yield();
            break;
        case 9: // S-mode external interrupt
            external_interrupt_handler();
            break;
        default:
            printf("\nunexpected interrupt (from user): %s\n", interrupt_info[trap_id]);
            printf("trap_id = %d, sepc = %p, stval = %p\n", trap_id, sepc, stval);
            panic("trap_user_handler");
        }
    } else {
        // 2. 异常
        switch (trap_id) {
        case 8: { // Environment call from U-mode (ecall)

            // 系统调用是"异常"，返回时 PC 需要跳过 ecall 指令
            tf->user_to_kern_epc += 4;

            // 交给 syscall 分发器，根据 a7 决定调用哪个 sys_xxx
            syscall();

            break;
        }
        case 15: { // Store/AMO page fault
            // 检查是否是栈扩展
            uint64 fault_addr = stval;
            // 用户栈的底部是 TRAPFRAME - p->ustack_npage * PGSIZE
            uint64 current_ustack_bottom = TRAPFRAME - p->ustack_npage * PGSIZE;
            
            // 如果是栈区域的页面错误，尝试扩展栈
            // 故障地址应该在当前栈底部下方一页内，且在TRAPFRAME上方
            if (fault_addr < current_ustack_bottom && fault_addr >= TRAPFRAME - PGSIZE * 10) {
                // 使用uvm_ustack_grow函数扩展用户栈
                uint64 old_ustack_npage = p->ustack_npage;
                uint64 new_ustack_npage = uvm_ustack_grow(p->pgtbl, p->ustack_npage, fault_addr);
                
                if (new_ustack_npage != (uint64)-1) {
                    p->ustack_npage = new_ustack_npage;
                    
                    printf("page fault occured! trap id = %d\n", trap_id);
                    printf("ustack_npage: %d -> %d\n", old_ustack_npage, p->ustack_npage);
                    
                    // 重新执行导致页面错误的指令
                    break;
                } else {
                    panic("trap_user_handler: uvm_ustack_grow failed for stack expansion");
                }
            } else {
                printf("\nunexpected exception (from user): %s\n", exception_info[trap_id]);
                printf("trap_id = %d, sepc = %p, stval = %p\n", trap_id, sepc, stval);
                panic("trap_user_handler");
            }
        }
        default:
            printf("\nunexpected exception (from user): %s\n", exception_info[trap_id]);
            printf("trap_id = %d, sepc = %p, stval = %p, sp = %p, gp = %p, ra = %p\n",
                   trap_id, sepc, stval, tf->sp, tf->gp, tf->ra);
            printf("a0 = %p, a1 = %p, a2 = %p\n", tf->a0, tf->a1, tf->a2);
            panic("trap_user_handler");
        }
    }

    // 处理完毕，准备按规范返回 U-mode
    trap_user_return();
}

// 调用 user_return()
// 内核态返回用户态（切回 U-mode）
void trap_user_return(void)
{
    cpu_t *c = mycpu();
    assert(c && c->proc && c->proc->tf, "trap_user_return: no current proc");

    proc_t      *p  = c->proc;
    trapframe_t *tf = p->tf;

    // 切换前先关中断，避免切换过程中被打断
    intr_off();

    // 将后续来自用户态的 trap 入口设置为 trampoline.S 里的 user_vector
    uint64 uservec = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);
    w_stvec(uservec);
    // user_vector 通过 sscratch 获取 trapframe 地址
    w_sscratch(TRAPFRAME);

    // 填写 trapframe 里“回到内核”时需要用到的几个字段
    tf->user_to_kern_satp       = r_satp();                 // 当前内核页表
    tf->user_to_kern_sp         = p->kstack + PGSIZE;       // 进程的内核栈顶
    tf->user_to_kern_trapvector = (uint64)trap_user_handler;// 下次用户态 trap 的 C 入口
    tf->user_to_kern_hartid     = r_tp();                   // 用于 mycpu()

    // 让 sret 返回到 U-mode，并在 U-mode 打开中断
    uint64 x = r_sstatus();
    x &= ~SSTATUS_SPP; // SPP = 0 -> user mode
    x &= ~SSTATUS_SPIE; // 返回后保持用户态中断关闭
    w_sstatus(x);

    // S 异常返回地址：下一次在用户态执行的位置
    w_sepc(tf->user_to_kern_epc);

    // 告诉 trampoline，要切换到哪个用户页表
    uint64 satp = MAKE_SATP(p->pgtbl);

    // user_return(TRAPFRAME, satp)
    uint64 userret = TRAMPOLINE + ((uint64)user_return - (uint64)trampoline);
    ((void (*)(uint64, uint64))userret)(TRAPFRAME, satp);

    // 不会再返回到这里
}
