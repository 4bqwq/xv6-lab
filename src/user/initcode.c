#include "sys.h"

#define VA_MAX       (1ul << 38)
#define PGSIZE       4096
#define MMAP_END     (VA_MAX - (16 * 256 + 2) * PGSIZE)
#define MMAP_BEGIN   (MMAP_END - 64 * 256 * PGSIZE)

int main()
{
    // merge gap both sides
    syscall(SYS_mmap, MMAP_BEGIN, 2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 2 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN, 6 * PGSIZE);

    // first fit picks inner hole
    syscall(SYS_mmap, MMAP_BEGIN, 8 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 2 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_mmap, 0, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN, 8 * PGSIZE);

    // reject overlap and gap unmap
    syscall(SYS_mmap, MMAP_BEGIN, 2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 1 * PGSIZE, 2 * PGSIZE);  // overlap expect fail
    syscall(SYS_mmap, MMAP_END, 1 * PGSIZE);                // boundary expect fail
    syscall(SYS_munmap, MMAP_BEGIN, 4 * PGSIZE);            // crosses hole expect fail
    syscall(SYS_mmap, MMAP_BEGIN + 8 * PGSIZE, 1 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 4 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 8 * PGSIZE, 1 * PGSIZE);

    // oversize len reject
    syscall(SYS_mmap, 0, (MMAP_END - MMAP_BEGIN) + PGSIZE);

    // split and trim one region
    syscall(SYS_mmap, MMAP_BEGIN + 12 * PGSIZE, 6 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 14 * PGSIZE, 2 * PGSIZE); // split middle
    syscall(SYS_munmap, MMAP_BEGIN + 16 * PGSIZE, 1 * PGSIZE); // trim head of tail
    syscall(SYS_munmap, MMAP_BEGIN + 17 * PGSIZE, 1 * PGSIZE); // remove rest of tail
    syscall(SYS_munmap, MMAP_BEGIN + 12 * PGSIZE, 2 * PGSIZE); // drop left part
    syscall(SYS_munmap, MMAP_BEGIN + 18 * PGSIZE, 2 * PGSIZE); // final cleanup, should be empty

    while (1)
        ;

    return 0;
}
