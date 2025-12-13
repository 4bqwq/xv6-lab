#include "sys.h"

int main()
{
    syscall(SYS_print_str, "level-1!\n");
    syscall(SYS_fork);
    syscall(SYS_print_str, "level-2!\n");
    syscall(SYS_fork);
    syscall(SYS_print_str, "level-3!\n");
    while(1);
}
