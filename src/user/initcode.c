#include "sys.h"

int main()
{
    int pid = syscall(SYS_fork);
    if (pid == 0) {
        syscall(SYS_print_str, "Ready to sleep!\n");
        syscall(SYS_sleep, 30);
        syscall(SYS_print_str, "Ready to exit!\n");
        syscall(SYS_exit, 0);
    } else {
        syscall(SYS_wait, 0);
        syscall(SYS_print_str, "Child exit!\n");
    }
    syscall(SYS_exit, 0);
    return 0;
}
