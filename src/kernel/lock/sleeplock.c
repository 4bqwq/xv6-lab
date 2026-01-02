#include "mod.h"
#include "../proc/mod.h"

// 睡眠锁初始化
void sleeplock_init(sleeplock_t *lk, char *name)
{
    lk->locked = 0;
    lk->name = name;
    lk->pid = -1;
    spinlock_init(&lk->lock, name);
}

// 检查当前进程是否持有睡眠锁
bool sleeplock_holding(sleeplock_t *lk)
{
    proc_t *p = myproc();
    if (p == NULL)
        return false;

    bool holding = false;
    spinlock_acquire(&lk->lock);
    holding = (lk->locked && lk->pid == p->pid);
    spinlock_release(&lk->lock);
    return holding;
}

// 当前进程尝试获取睡眠锁, 失败进入睡眠状态
void sleeplock_acquire(sleeplock_t *lk)
{
    while (1) {
        spinlock_acquire(&lk->lock);
        if (!lk->locked) {
            lk->locked = 1;
            proc_t *p = myproc();
            lk->pid = (p != NULL) ? p->pid : -1;
            spinlock_release(&lk->lock);
            break;
        }
        proc_sleep(lk, &lk->lock);
    }
}

// 释放睡眠锁, 唤醒其他等待睡眠锁的进程
void sleeplock_release(sleeplock_t *lk)
{
    spinlock_acquire(&lk->lock);
    lk->locked = 0;
    lk->pid = -1;
    proc_wakeup(lk);
    spinlock_release(&lk->lock);
}
