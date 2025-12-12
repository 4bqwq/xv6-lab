#include "sys.h"

#define VA_MAX       (1ul << 38)
#define PGSIZE       4096
#define MMAP_END     (VA_MAX - (16 * 256 + 2) * PGSIZE)
#define MMAP_BEGIN   (MMAP_END - 64 * 256 * PGSIZE)

int main()
{
    syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, 3 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 10 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 2 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 12 * PGSIZE, 1 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 7 * PGSIZE, 3 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN, 2 * PGSIZE);
    syscall(SYS_mmap, 0, 10 * PGSIZE);

    syscall(SYS_munmap, MMAP_BEGIN + 10 * PGSIZE, 5 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN, 10 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 17 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 15 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 19 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 22 * PGSIZE, 1 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 21 * PGSIZE, 1 * PGSIZE);

    while (1)
        ;

    return 0;
}
