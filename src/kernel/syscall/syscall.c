#include "mod.h"

// 系统调用号 -> 内核服务函数
static uint64 (*syscalls[])(void) = {
    [SYS_brk] sys_brk,
    [SYS_mmap] sys_mmap,
    [SYS_munmap] sys_munmap,
    [SYS_print_str] sys_print_str,
    [SYS_print_int] sys_print_int,
    [SYS_getpid] sys_getpid,
    [SYS_fork] sys_fork,
    [SYS_wait] sys_wait,
    [SYS_exit] sys_exit,
    [SYS_sleep] sys_sleep,

    // lab7: 文件系统相关测试系统调用 (11~21)
    [SYS_alloc_block] sys_alloc_block,
    [SYS_free_block] sys_free_block,
    [SYS_alloc_inode] sys_alloc_inode,
    [SYS_free_inode] sys_free_inode,
    [SYS_show_bitmap] sys_show_bitmap,
    [SYS_get_block] sys_get_block,
    [SYS_read_block] sys_read_block,
    [SYS_write_block] sys_write_block,
    [SYS_put_block] sys_put_block,
    [SYS_show_buffer] sys_show_buffer,
    [SYS_flush_buffer] sys_flush_buffer,
    
    //lab9
    [SYS_helloworld] sys_helloworld,
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

uint64 sys_helloworld(void)
{
    printf("hello world!\n");
    return 0;
}
