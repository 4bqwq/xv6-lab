#include "mod.h"
#include <stddef.h>
/*
    开关中断的基本逻辑:
    1. 多个地方可能开关中断, 因此它不是“开/关”的二元状态，而是“关 关 关 开 开 开”的stack
    2. 在第一次执行关中断时, 记录中断的初始状态为X
    3. 每次关中断, stack中的元素加1
    4. 每次开中断, stack中的元素减1
    5. 如果stack中元素清空, 将中断状态设为初始的X
*/

// 带层数叠加的关中断
void push_off(void)
{
    int old = intr_get();
    intr_off();
    cpu_t *cpu = mycpu();
    if (cpu->noff == 0)
        cpu->origin = old;
    cpu->noff++;
}

// 带层数叠加的开中断
void pop_off(void)
{
    cpu_t *cpu = mycpu();
    assert(intr_get() == 0, "push_off: 1\n"); // 确保此时中断是关闭的
    assert(cpu->noff >= 1, "push_off: 2\n");  // 确保push和pop的对应
    cpu->noff--;
    if (cpu->noff == 0 && cpu->origin == 1) // 只有所有push操作都被抵消且原来状态是开着时
        intr_on();
}

// Atomic exchange
static int xchg(volatile int *addr, int newv) {
    return __sync_lock_test_and_set(addr, newv); // returns old, sets *addr=newv
}
static void xchg_release(volatile int *addr) {
     __sync_lock_release(addr); // resets *addr to 0
}

// 自选锁初始化
void spinlock_init(spinlock_t *lk, char *name) {
    lk -> locked = 0;
    lk -> name = name;
    lk -> cpuid = -1;
}

// 是否持有自旋锁
bool spinlock_holding(spinlock_t *lk) {
    if (lk -> locked == 0)
        return false;
    if (lk -> cpuid != mycpuid())
        return false;
    return true;
}



// message buffers
static char* scpy(char* dst, const char* src, char* end) {
    while (dst < end - 1 && *src) *dst++ = *src++;
    *dst = 0;
    return dst;
}

static const char* msg_acquire(char* buf, size_t n, spinlock_t* lk) {
    char* p = buf;
    char* end = buf + n;
    p = scpy(p, "acquire: lock '", end);
    p = scpy(p, lk->name, end);
    scpy(p, "' already held", end);
    return buf;
}

static const char* msg_release(char* buf, size_t n, spinlock_t* lk) {
    char* p = buf;
    char* end = buf + n;
    p = scpy(p, "release: lock '", end);
    p = scpy(p, lk->name, end);
    scpy(p, "' not held", end);
    return buf;
}

// 获取自选锁
void spinlock_acquire(spinlock_t *lk) {
    push_off();
    char msg_buf[100];
    msg_buf[0] = 0;
    assert(!spinlock_holding(lk), msg_acquire(msg_buf, sizeof msg_buf, lk));
    while (xchg(&lk -> locked, 1) != 0) {} // spin
    __sync_synchronize();
    lk -> cpuid = mycpuid();
}

// 释放自旋锁
void spinlock_release(spinlock_t *lk) {
    char msg_buf[100];
    msg_buf[0] = 0;
    assert(spinlock_holding(lk), msg_release(msg_buf, sizeof msg_buf, lk));
    lk -> cpuid = -1;
    __sync_synchronize();
    xchg_release(&lk -> locked);
    pop_off();
}
