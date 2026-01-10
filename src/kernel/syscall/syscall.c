#include "mod.h"

// 系统调用号 -> 内核服务函数
static uint64 (*syscalls[])(void) = {
    [SYS_brk] sys_brk,
    [SYS_mmap] sys_mmap,
    [SYS_munmap] sys_munmap,
    [SYS_fork] sys_fork,
    [SYS_wait] sys_wait,
    [SYS_exit] sys_exit,
    [SYS_sleep] sys_sleep,
    [SYS_getpid] sys_getpid,
    [SYS_exec] sys_exec,
    [SYS_open] sys_open,
    [SYS_close] sys_close,
    [SYS_read] sys_read,
    [SYS_write] sys_write,
    [SYS_lseek] sys_lseek,
    [SYS_dup] sys_dup,
    [SYS_fstat] sys_fstat,
    [SYS_get_dentries] sys_get_dentries,
    [SYS_mkdir] sys_mkdir,
    [SYS_chdir] sys_chdir,
    [SYS_print_cwd] sys_print_cwd,
    [SYS_link] sys_link,
    [SYS_unlink] sys_unlink,
};

// 基于系统调用表的请求跳转
void syscall()
{
    proc_t *p = myproc();
    int sys_num = p->tf->a7;

    int nsys = (int)(sizeof(syscalls) / sizeof(syscalls[0]));
    if (sys_num < 0 || sys_num >= nsys || syscalls[sys_num] == NULL) {
        printf("unknown syscall %d from pid = %d\n", sys_num, p->pid);
        panic("syscall");
    }

    p->tf->a0 = syscalls[sys_num]();
}

/* ---------------- 参数读取工具 ---------------- */

static uint64 arg_raw(int n)
{
    proc_t *proc = myproc();

    switch (n) {
    case 0: return proc->tf->a0;
    case 1: return proc->tf->a1;
    case 2: return proc->tf->a2;
    case 3: return proc->tf->a3;
    case 4: return proc->tf->a4;
    case 5: return proc->tf->a5;
    default:
        panic("arg_raw: illegal arg num");
        return (uint64)-1;
    }
}

void arg_uint32(int n, uint32 *ip) { *ip = (uint32)arg_raw(n); }
void arg_uint64(int n, uint64 *ip) { *ip = (uint64)arg_raw(n); }

void arg_str(int n, char *buf, int maxlen)
{
    proc_t *p = myproc();
    uint64 addr;
    arg_uint64(n, &addr);
    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, maxlen);
}
