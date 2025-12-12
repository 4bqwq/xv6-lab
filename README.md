# LAB-5

## 项目运行与环境

通过 `make qemu` 启动系统。

## 一：系统调用流程

### 系统调用处理

在 `src/kernel/trap/trap_user.c` 封装系统调用。当用户程序执行 `ecall` 指令时，会触发 `scause = 8` 的异常：

```c
case 8: { // Environment call from U-mode (ecall)
    tf->user_to_kern_epc += 4;
    syscall();
    break;
}
```

执行系统调用时，需要把程序计数器向前挪4字节，跳过ecall指令，这样从内核返回用户态时就不会重复执行它。具体的系统调用功能则由syscall函数根据a7寄存器里的编号来分发处理。

### 跨地址空间数据传递

用户态和内核态使用不同的页表，需要专门的函数处理数据传递。在 `src/kernel/mem/uvm.c` 中实现了：

```c
static uint64 walk_user_va(pgtbl_t pgtbl, uint64 va)
{
    pte_t *pte = vm_getpte(pgtbl, va, false);
    assert(pte != NULL, "uvm: vm_getpte returned NULL");
    assert((*pte) & PTE_V, "uvm: pte not valid");
    return (uint64)PTE_TO_PA(*pte);
}
```

`walk_user_va` 通过用户页表查找虚拟地址对应的物理页基址。`PTE_TO_PA` 宏从页表项中提取物理地址。

`uvm_copyin` 函数处理跨页数据拷贝：

```c
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    while (len > 0) {
        uint64 va0 = PGROUNDDOWN(src);    // 当前用户页首地址
        uint32 off = (uint32)(src - va0); // 页内偏移
        uint32 npage = PGSIZE - off;      // 当前页还能拷多少字节
        uint32 n = (len < npage) ? len : npage;

        uint64 pa0 = walk_user_va(pgtbl, va0);
        void* k_src = (void*)(pa0 + off);
        void* k_dst = (void*)dst;
        memmove(k_dst, k_src, n);

        dst += n; src += n; len -= n;
    }
}
```

这里使用了 `PGROUNDDOWN` 宏将地址向下对齐到页边界，`PGSIZE` 是页面大小常量(4096字节)。`memmove` 进行实际的内存拷贝。

### 系统调用参数提取

在 `src/kernel/syscall/sysfunc.c` 中，通过 `arg_uint64` 和 `arg_uint32` 从trapframe中提取参数：

```c
uint64 sys_copyin()
{
    proc_t *p = myproc();
    uint64 uaddr;
    uint32 len;
    arg_uint64(0, &uaddr);  // 第0个参数
    arg_uint32(1, &len);    // 第1个参数
    
    int buf[32];
    uvm_copyin(p->pgtbl, (uint64)buf, uaddr, len * sizeof(int));
    // 处理数据...
    return 0;
}
```

`myproc()` 获取当前进程结构体，`p->pgtbl` 是用户页表基址。`arg_uint64` 内部通过 `p->tf->a0` 等寄存器访问参数。

### 测试结果

![](.\pictures\lab5_test-1.png)

这个验证系统调用流程和数据传递是否正常。内核通过 sys_copyout 把数组 [1 2 3 4 5] 成功传到用户空间，sys_copyin 则顺利从用户空间读回数组并打印出来，而 sys_copyinstr 也正确获取了用户提供的字符串 hello, world。

## 二：用户堆空间管理

### 堆扩展实现

`sys_brk` 系统调用处理堆空间的伸缩。堆从低地址向高地址生长，但不能越过 `MMAP_BEGIN`：

```c
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len)
{
    uint64 new_heap_top = cur_heap_top + len;
    if (new_heap_top > MMAP_BEGIN) {
        return (uint64)-1;
    }

    uint64 va_start = PGROUNDUP(cur_heap_top);
    for (uint64 va = va_start; va < new_heap_top; va += PGSIZE) {
        uint64 page = (uint64)pmem_alloc(false);
        vm_mappages(pgtbl, va, page, PGSIZE, PTE_R | PTE_W | PTE_U);
    }
    return new_heap_top;
}
```

`PGROUNDUP` 宏将地址向上对齐到页边界。`pmem_alloc(false)` 从用户内存池分配物理页，`vm_mappages` 建立虚拟地址到物理地址的映射，权限位 `PTE_R | PTE_W | PTE_U` 表示可读可写的用户页面。

### 栈自动扩展

栈扩展通过页面错误处理实现。在 `trap_user_handler` 中处理 `scause = 15` 的页面错误：

```c
case 15: { // Store/AMO page fault
    uint64 fault_addr = stval;
    uint64 current_ustack_bottom = TRAPFRAME - p->ustack_npage * PGSIZE;
    
    if (fault_addr < current_ustack_bottom && fault_addr >= TRAPFRAME - PGSIZE * 10) {
        uint64 new_ustack_npage = uvm_ustack_grow(p->pgtbl, p->ustack_npage, fault_addr);
        if (new_ustack_npage != (uint64)-1) {
            p->ustack_npage = new_ustack_npage;
            break;
        }
    }
}
```

`stval` 寄存器保存触发页面错误的地址。`TRAPFRAME` 是trapframe页面的地址，栈在其下方生长。`uvm_ustack_grow` 计算需要扩展的页数并分配映射：

```c
uint64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_ustack_npage, uint64 fault_addr)
{
    uint64 current_stack_bottom = TRAPFRAME - old_ustack_npage * PGSIZE;
    uint64 target_stack_bottom = PGROUNDDOWN(fault_addr);
    
    if (target_stack_bottom < current_stack_bottom) {
        uint64 pages_to_add = (current_stack_bottom - target_stack_bottom) / PGSIZE;
        for (uint64 i = 0; i < pages_to_add; i++) {
            uint64 new_page_va = current_stack_bottom - (i + 1) * PGSIZE;
            uint64 page = (uint64)pmem_alloc(false);
            memset((void*)page, 0, PGSIZE);
            vm_mappages(pgtbl, new_page_va, page, PGSIZE, PTE_R | PTE_W | PTE_U);
        }
        return old_ustack_npage + pages_to_add;
    }
    return -1;
}
```

### 测试结果

![](.\pictures\lab5_test-2.png)

在 look 事件，堆顶为 0x2000，页表中只映射了 physical page 1。
在 grow 事件中，堆顶增长到 0xb000，页表中新增了 physical page 2 到 10，共 9 个物理页。
在 equal 事件中，堆顶保持不变，页表内容与 grow 阶段一致，没有发生变化。
在 ungrow 事件中，堆顶缩小到 0x6000，physical page 6 到 10 不再出现，说明对应的物理页已被释放。

![](.\pictures\lab5_test-3.png)

第一次发生页面错误时，ustack_npage 从1增加到2，成功读取了字符串hello。随后第二次页面错误触发，ustack_npage 进一步扩展到5，顺利获取了字符串world。

## 三：mmap_region_node

### 资源池初始化

在 `src/kernel/mem/mmap.c` 中实现了固定大小的资源池：

```c
void mmap_init()
{
    spinlock_init(&list_lk, "mmap_list");
    list_head.next = &(node_list[0]);
    
    for(int i = 0; i < N_MMAP - 1; i++) {
        node_list[i].next = &(node_list[i + 1]);
    }
    node_list[N_MMAP - 1].next = 0;
}
```

`spinlock_init` 初始化自旋锁，`list_head` 是链表头，`node_list` 是预分配的节点数组。`N_MMAP` 是最大节点数常量。

### 资源分配

```c
mmap_region_t *mmap_region_alloc()
{
    spinlock_acquire(&list_lk);
    
    if(list_head.next == 0) {
        spinlock_release(&list_lk);
        panic("mmap_region_alloc: out of nodes");
    }
    
    mmap_region_node_t *node = list_head.next;
    list_head.next = node->next;
    
    spinlock_release(&list_lk);
    
    return &(node->mmap);
}
```

`spinlock_acquire` 和 `spinlock_release` 保护链表操作。通过 `container_of` 宏的逆向操作，将 `mmap_region_t*` 转换回 `mmap_region_node_t*` 进行释放。

### 测试结果

![](.\pictures\lab5_test-4(1).png)

![](.\pictures\lab5_test-4(2).png)

在多核并发申请 `mmap_region_node` 的测试中，输出的 `index` 序列出现了非连续的跳跃现象，说明多个 CPU 的分配操作在时间上发生了交错执行。

## 四：mmap 与 munmap 实现

### 地址分配算法

`uvm_mmap_find` 函数实现首次适应算法寻找合适的位置，在地址空间中查找第一个足够容纳请求大小的空闲区域。

```c
static uint64 uvm_mmap_find(mmap_region_t *head_mmap, uint64 len,
                            mmap_region_t **p_last_mmap, mmap_region_t **p_tmp_mmap)
{
    mmap_region_t *prev = NULL;
    mmap_region_t *curr = head_mmap;
    uint64 candidate = MMAP_BEGIN;

    while (curr != NULL) {
        if (candidate + len <= curr->begin) {
            *p_last_mmap = prev;
            *p_tmp_mmap = curr;
            return candidate;
        }
        candidate = curr->begin + (uint64)curr->npages * PGSIZE;
        prev = curr;
        curr = curr->next;
    }
    
    if (candidate + len <= MMAP_END) {
        *p_last_mmap = prev;
        *p_tmp_mmap = NULL;
        return candidate;
    }
    return 0;
}
```

`MMAP_BEGIN` 和 `MMAP_END` 定义了mmap区域的地址范围。函数返回找到的起始地址，并通过输出参数返回前驱和后继节点。

### 相邻区域合并

```c
static void mmap_merge(mmap_region_t *mmap_1, mmap_region_t *mmap_2, bool keep_mmap_1)
{
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin,
           "mmap_merge: regions not adjacent");

    if (keep_mmap_1) {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    } else {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}
```

合并前检查两个区域是否相邻，然后根据 `keep_mmap_1` 参数决定保留哪个节点。

### 部分释放处理

`uvm_munmap` 支持部分释放，可能需要分割现有区域：

```c
bool uvm_munmap(uint64 begin, uint32 npages)
{
    // 先扫描一遍确认区间被完全覆盖，防止释放了一半才发现不合法
    uint64 probe = begin;
    mmap_region_t *check = curr;
    while (check && begin >= check->begin + (uint64)check->npages * PGSIZE) {
        prev = check;
        check = check->next;
    }
    while (probe < end) {
        if (check == NULL) return false;
        uint64 region_start = check->begin;
        uint64 region_end = region_start + (uint64)check->npages * PGSIZE;
        if (probe < region_start || probe >= region_end) return false;
        uint64 chunk_end = (end < region_end) ? end : region_end;
        probe = chunk_end;
        if (probe < end) check = check->next;
    }

    uint64 cursor = begin;
    while (cursor < end) {
        uint64 region_start = curr->begin;
        uint64 region_end = region_start + (uint64)curr->npages * PGSIZE;
        uint64 chunk_end = (end < region_end) ? end : region_end;
        
        vm_unmappages(p->pgtbl, cursor, chunk_len, true);
        
        if (cursor == region_start && chunk_end == region_end) {
            // 完全释放
            mmap_region_t *next = curr->next;
            if (prev) prev->next = next; else p->mmap = next;
            mmap_region_free(curr);
            curr = next;
        } else if (cursor == region_start) {
            // 头部释放
            curr->begin = chunk_end;
            curr->npages -= chunk_pages;
            prev = curr;
        } else if (chunk_end == region_end) {
            // 尾部释放
            curr->npages -= chunk_pages;
            prev = curr;
            curr = curr->next;
        } else {
            // 中间分割
            uint32 left_pages = (uint32)((cursor - region_start) / PGSIZE);
            uint32 right_pages = (uint32)((region_end - chunk_end) / PGSIZE);
            mmap_region_t *tail = mmap_region_alloc();
            tail->begin = chunk_end;
            tail->npages = right_pages;
            tail->next = curr->next;
            curr->npages = left_pages;
            curr->next = tail;
            prev = curr;
            curr = tail;
        }
        cursor = chunk_end;
    }
    return true;
}
```

`vm_unmappages` 函数取消页表映射并释放物理页，第三个参数 `true` 表示同时释放物理页。

### 测试结果

![](.\pictures\lab5_test-5.png)


![img](file://wsl.localhost/Ubuntu/home/ubuntu/Programs/OS/xv6-lab/pictures/lab5_test-5.png?lastModify=1765688012)

第一次 mmap 在地址 fb002000 分配了 3 页内存，映射顺利。在 fb008000 又分配了 2 页，用来验证非连续分配是否可行。后续几次 mmap 操作混合使用了系统自动选择地址和手动指定地址的方式。有调用 munmap，分别测试了部分释放、完整释放，以及映射区域被分割后的处理情况。

补充了一轮更细的 mmap/munmap 覆盖测试

```c
// begin=0 时 first-fit 掉进中间空洞，先挖洞再回填
syscall(SYS_mmap, MMAP_BEGIN, 8 * PGSIZE);
syscall(SYS_munmap, MMAP_BEGIN + 2 * PGSIZE, 2 * PGSIZE);
syscall(SYS_mmap, 0, 2 * PGSIZE);
syscall(SYS_munmap, MMAP_BEGIN, 8 * PGSIZE);

// 同时与左右相邻块合并，随后整段回收验证链表清理
syscall(SYS_mmap, MMAP_BEGIN, 2 * PGSIZE);
syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, 2 * PGSIZE);
syscall(SYS_mmap, MMAP_BEGIN + 2 * PGSIZE, 2 * PGSIZE);
syscall(SYS_munmap, MMAP_BEGIN, 6 * PGSIZE);

// 非法输入路径：重叠、越界、跨空洞 munmap 都应该被拒绝
syscall(SYS_mmap, MMAP_BEGIN, 2 * PGSIZE);
syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, 2 * PGSIZE);
syscall(SYS_mmap, MMAP_BEGIN + 1 * PGSIZE, 2 * PGSIZE); // 预期失败
syscall(SYS_mmap, MMAP_END, 1 * PGSIZE);                // 预期失败
syscall(SYS_munmap, MMAP_BEGIN, 4 * PGSIZE);            // 预期失败

// 拆分场景：中间切开再分步收尾，覆盖头部、尾部、整段释放三条分支
syscall(SYS_mmap, MMAP_BEGIN + 12 * PGSIZE, 6 * PGSIZE);
syscall(SYS_munmap, MMAP_BEGIN + 14 * PGSIZE, 2 * PGSIZE); // 中间拆出一块
syscall(SYS_munmap, MMAP_BEGIN + 16 * PGSIZE, 1 * PGSIZE); // 剪掉右半块的头
syscall(SYS_munmap, MMAP_BEGIN + 17 * PGSIZE, 1 * PGSIZE); // 把剩余右半块收掉
syscall(SYS_munmap, MMAP_BEGIN + 12 * PGSIZE, 2 * PGSIZE); // 左半块释放
syscall(SYS_munmap, MMAP_BEGIN + 18 * PGSIZE, 2 * PGSIZE); // 全部清空

// 兜底失败用例：过大长度、未映射地址都应该直接失败不动链表
syscall(SYS_mmap, 0, (MMAP_END - MMAP_BEGIN) + PGSIZE);    // 预期失败
syscall(SYS_mmap, MMAP_BEGIN + 0x1000 * PGSIZE, 2 * PGSIZE);
syscall(SYS_munmap, MMAP_BEGIN + 0x1001 * PGSIZE, 2 * PGSIZE); // 未映射，预期失败
```

为了更加直观，补充了几个print语句，sys_mmap/sys_munmap 打印 start/len/结果。

```
/*
    增加一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节,需检查是否是page-aligned)
    成功返回映射空间的起始地址, 失败返回-1
*/
uint64 sys_mmap()
{
    proc_t *p = myproc();

    uint64 start;
    uint64 len;
    arg_uint64(0, &start);
    arg_uint64(1, &len);

    int seq = ++mmap_call_seq;
    printf("sys_mmap[%d] start %p len %p\n", seq, start, len);

    if (!mmap_len_valid(len) || !mmap_start_valid(start))
    {
        printf("sys_mmap[%d] reject invalid args\n", seq);
        return (uint64)-1;
    }

    if (start != 0 && len > MMAP_END - start)
    {
        printf("sys_mmap[%d] reject overflow range\n", seq);
        return (uint64)-1;
    }

    uint32 npages = (uint32)(len / PGSIZE);
    // uvm_mmap 负责分配物理页并调用 vm_mappages 建立映射
    uint64 mapped = uvm_mmap(start, npages, PTE_R | PTE_W);
    if (mapped == 0)
    {
        printf("sys_mmap[%d] alloc fail\n", seq);
        return (uint64)-1;
    }

    printf("sys_mmap[%d] mapped at %p npages %d\n", seq, mapped, npages);
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

    return mapped;
}

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节, 需检查是否是page-aligned)
    成功返回0 失败返回-1
*/
uint64 sys_munmap()
{
    proc_t *p = myproc();

    uint64 start;
    uint64 len;
    arg_uint64(0, &start);
    arg_uint64(1, &len);

    int seq = ++munmap_call_seq;
    printf("sys_munmap[%d] start %p len %p\n", seq, start, len);

    if (!mmap_len_valid(len) || start == 0 || (start % PGSIZE) != 0)
    {
        printf("sys_munmap[%d] reject invalid args\n", seq);
        return (uint64)-1;
    }

    if (start < MMAP_BEGIN || start >= MMAP_END)
    {
        printf("sys_munmap[%d] reject out of range\n", seq);
        return (uint64)-1;
    }
    if (len > MMAP_END - start)
    {
        printf("sys_munmap[%d] reject overflow range\n", seq);
        return (uint64)-1;
    }

    uint32 npages = (uint32)(len / PGSIZE);
    // uvm_munmap 会在页表里逐段卸载映射并回收节点
    if (!uvm_munmap(start, npages))
    {
        printf("sys_munmap[%d] fail in unmap\n", seq);
        return (uint64)-1;
    }

    printf("sys_munmap[%d] ok npages %d\n", seq, npages);
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

    return 0;
}
```

 ![](.\pictures\lab5_test-5(2).png)

如图所示, `sys_mmap[5] start 0 len 2000` 实际返回地址为 `fb000000`, 表明在 `begin=0` 的情况下映射优先落入已存在的空洞, 验证了 first-fit 分配策略; `sys_mmap[8] alloc fail`、`sys_mmap[9] reject invalid args` 以及 `sys_munmap[4] fail in unmap` 对应重叠、越界和跨空洞等非法请求, 且后续映射仍能正常建立与回收, 说明失败路径不会破坏链表状态; `sys_mmap[12]` 之后连续出现的 `sys_munmap[8..11] ok` 覆盖了中间拆分、剪除头尾及整段回收等多种 `munmap` 分支, 最后的 `sys_munmap[12] fail in unmap` 表明对未映射地址的回收请求被正确拒绝。

## 五：页表复制与销毁

### 递归页表销毁

```c
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    for (int i = 0; i < PGSIZE / (int)sizeof(pte_t); i++) {
        pte_t pte = pgtbl[i];
        if ((pte & PTE_V) == 0) continue;

        if (PTE_CHECK(pte) && level > 1) {
            destroy_pgtbl((pgtbl_t)PTE_TO_PA(pte), level - 1);
        } else if (pte & PTE_U) {
            pmem_free((uint64)PTE_TO_PA(pte), false);
        }
        pgtbl[i] = 0;
    }
    pmem_free((uint64)pgtbl, true);
}
```

`PTE_CHECK` 宏判断页表项是否指向下一级页表。用户页通过 `pmem_free(..., false)` 释放，页表页通过 `pmem_free(..., true)` 释放。

### 选择性页表复制

```c
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 start = PGROUNDDOWN(begin);
    uint64 stop = PGROUNDUP(end);

    for (uint64 va = start; va < stop; va += PGSIZE) {
        pte_t *pte = vm_getpte(old, va, false);
        if (pte == NULL || (*pte & PTE_V) == 0 || PTE_CHECK(*pte)) continue;
        if (((*pte) & PTE_U) == 0) continue;

        uint64 pa = (uint64)PTE_TO_PA(*pte);
        int flags = (int)PTE_FLAGS(*pte);
        uint64 page = (uint64)pmem_alloc(false);
        memmove((void *)page, (const void *)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}
```

`PTE_FLAGS` 宏提取页表项的权限位。只复制用户页面，跳过内核恒等映射和页表页。

### 完整性验证机制

在 `src/kernel/proc/proc.c` 中实现了验证流程：

```c
static bool verify_page_equal(void *pa_a, void *pa_b)
{
    uint8 *lhs = (uint8 *)pa_a;
    uint8 *rhs = (uint8 *)pa_b;
    for (uint32 i = 0; i < PGSIZE; i++) {
        if (lhs[i] != rhs[i]) return false;
    }
    return true;
}

static bool verify_range_equal(pgtbl_t a, pgtbl_t b, uint64 begin, uint64 end)
{
    for (uint64 va = start; va < stop; va += PGSIZE) {
        pte_t *pte_a = vm_getpte(a, va, false);
        pte_t *pte_b = vm_getpte(b, va, false);
        
        bool valid_a = pte_a && (*pte_a & PTE_V) && !PTE_CHECK(*pte_a) && ((*pte_a) & PTE_U);
        bool valid_b = pte_b && (*pte_b & PTE_V) && !PTE_CHECK(*pte_b) && ((*pte_b) & PTE_U);
        
        if (valid_a != valid_b) return false;
        if (valid_a && !verify_page_equal((void *)PTE_TO_PA(*pte_a), (void *)PTE_TO_PA(*pte_b))) return false;
    }
    return true;
}
```

验证包括内容一致性和写时复制隔离性测试，确保页表复制的正确性。

### 测试结果

实验要求自己设计测试用例，我写了代码测试。这里是做了四方面，复制出来的页表内容是不是和原版一模一样；改了副本会不会动到原来的页表；各种地址边界情况能不能处理好；还有物理页面有没有被正确分配和释放。

验证代码：

```c
// 在 main.c 中的验证流程
printf("[pgtbl-check] begin\n");

// 1. 堆区域复制测试
uvm_copy_range(p->pgtbl, new_pgtbl, p->heap_top, p->heap_top + PGSIZE);
assert(verify_range_equal(p->pgtbl, new_pgtbl, p->heap_top, p->heap_top + PGSIZE), "heap copy failed");
printf("[pgtbl-check] heap copy ok\n");

// 2. 栈区域复制测试  
uvm_copy_range(p->pgtbl, new_pgtbl, TRAPFRAME - p->ustack_npage * PGSIZE, TRAPFRAME);
assert(verify_range_equal(p->pgtbl, new_pgtbl, TRAPFRAME - p->ustack_npage * PGSIZE, TRAPFRAME), "stack copy failed");
printf("[pgtbl-check] stack copy ok\n");

// 3. mmap区域复制测试
mmap_region_t *mmap = p->mmap;
while (mmap != NULL) {
    uint64 begin = mmap->begin;
    uint64 end = begin + (uint64)mmap->npages * PGSIZE;
    uvm_copy_range(p->pgtbl, new_pgtbl, begin, end);
    assert(verify_range_equal(p->pgtbl, new_pgtbl, begin, end), "mmap copy failed");
    mmap = mmap->next;
}
printf("[pgtbl-check] mmap copy ok\n");

// 4. 隔离性测试
char *test_addr = (char *)p->heap_top;
*test_addr = 'X';
assert(!verify_range_equal(p->pgtbl, new_pgtbl, p->heap_top, p->heap_top + PGSIZE), "isolation test failed");
printf("[pgtbl-check] copy isolation ok\n");

// 5. 销毁测试
uvm_destroy_pgtbl(new_pgtbl, 3);
printf("[pgtbl-check] destroy empty ok\n");

printf("[pgtbl-check] end\n");
```

![](.\pictures\lab5_test-6.png)
