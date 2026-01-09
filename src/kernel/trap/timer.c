#include "../arch/mod.h"
#include "../lib/mod.h"
#include "../proc/mod.h"
#include "type.h"
#include "mod.h"

/*-------------------- 工作在M-mode --------------------*/

// in trap.S M-mode时钟中断处理流程()
extern void timer_vector();

// 每个CPU在时钟中断中需要的临时空间
static uint64 mscratch[NCPU][5];

// 时钟初始化
void timer_init()
{
    // 获取当前cpuid
    int hartid = r_tp();

    // 设置初始值 cmp_time = cur_time + time_interval
    *(uint64*)CLINT_MTIMECMP(hartid) = *(uint64*)CLINT_MTIME + INTERVAL;

    // cur_mscratch 指向当前CPU的msrcatch数组
    uint64* cur_mscratch = mscratch[hartid];

    // cur_mscratch[1] [2] [3]先空着, 在trap.S里使用
    cur_mscratch[3] = CLINT_MTIMECMP(hartid); // cmp_time
    cur_mscratch[4] = INTERVAL;               // interval

    // 存放到临时寄存器, 便于与trap.S中的timer_vector协作
    w_mscratch((uint64)cur_mscratch);

    // 设置 M-mode 中断处理函数
    w_mtvec((uint64)timer_vector);

    // 打开 M-mode 中断总开关
    w_mstatus(r_mstatus() | MSTATUS_MIE);

    // 打开 M-mode 时钟中断分开关
    w_mie(r_mie() | MIE_MTIE);
}

/*--------------------- 工作在S-mode --------------------*/

// 全局系统时钟
static timer_t sys_timer;
static spinlock_t timer_lk;

// 时钟创建
void timer_create()
{
    sys_timer.ticks = 0;
    spinlock_init(&timer_lk, "timer");
}

void timer_update(void)
{
    spinlock_acquire(&timer_lk);
    sys_timer.ticks++;
    proc_wakeup(&sys_timer);
    spinlock_release(&timer_lk);
}

// 获取滴答数量 (不把sys_timer暴露出去, 只提供安全的访问接口)
uint64 timer_get_ticks()
{
    uint64 ticks = 0;
    spinlock_acquire(&timer_lk);
    ticks = sys_timer.ticks;
    spinlock_release(&timer_lk);
    return ticks;
}

// 让进程睡眠ntick个时钟周期
void timer_wait(uint64 ntick)
{
    proc_t *p = myproc();
    spinlock_acquire(&timer_lk);
    uint64 target = sys_timer.ticks + ntick;
    while (sys_timer.ticks < target) {
        if (p != NULL)
            printf("proc %d is sleeping!\n", p->pid);
        proc_sleep(&sys_timer, &timer_lk);
    }
    spinlock_release(&timer_lk);
}
