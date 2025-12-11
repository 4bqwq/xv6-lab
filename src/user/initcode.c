#include "sys.h"

int main()
{
    int L[5];
    char* s = "hello, world";

    // 内核把 [1 2 3 4 5] 写入用户数组 L
    syscall(SYS_copyout, L);

    // 用户把 L 传给内核，内核再打印出来
    syscall(SYS_copyin, L, 5);

    // 用户把字符串 s 传给内核，内核打印出来
    syscall(SYS_copyinstr, s);

    while (1)
        ;

    return 0;
}
