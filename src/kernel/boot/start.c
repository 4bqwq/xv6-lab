#include "../arch/mod.h"

// 每个CPU在运行操作系统时需要一个初始的函数栈
__attribute__((aligned(16))) uint8 CPU_stack[4096 * NCPU];

extern void main();
extern void timer_init(void);

void start()
{
    // 暂时不开启分页，使用物理地址
    w_satp(0);

    // 将能委托的异常与中断都委托给 S 模式
    w_medeleg(~0ULL);
    w_mideleg(~0ULL);

    // 允许 S 模式访问 mcycle/minstret/mtime
    w_mcounteren(0x7);

    // 切换到S-mode后无法访问M-mode的寄存器
    // 所以需要将hartid存到可访问的寄存器tp
    int id = r_mhartid();
    w_tp(id);
    
    // 初始化定时器
    timer_init();

    // 修改mstatus寄存器，假装上一个状态是S-mode
    uint64 status = r_mstatus();
    status &= ~MSTATUS_MPP_MASK;
    status |= MSTATUS_MPP_S;
    w_mstatus(status);

    // 设置M-mode的返回地址
    w_mepc((uint64)main);

    // 触发状态迁移，回到上一个状态（M-mode->S-mode）
    asm volatile("mret");

    // If we ever get back here, something went wrong.
    for (;;)
        asm volatile("wfi");
}
