# Lab7：磁盘、Buffer Cache 与文件系统基础

- **A（周屹枫）**：在 Lab6 基础上完成 Lab7 的主要功能实现，包括 VirtIO 磁盘驱动、磁盘中断处理、buffer cache、bitmap、文件系统初始化、系统调用扩展，以及本 README 的撰写。
- **B（严之皓）**：提供 Lab6 内核代码基础与模块接口说明，协助对接内存/Trap/进程模块，参与 VirtIO I/O、中断链路与文件系统的联调与测试。

---

## 一、环境与运行方式

本实验在 Lab6 的内核框架基础上完成，引入 VirtIO Block 磁盘设备，并在 QEMU 虚拟机中通过 `disk.img` 提供磁盘镜像。

编译与运行方式：

```bash
make clean && make run

成功后将启动 QEMU，内核启动完成后自动进入用户态 initcode，并根据测试程序输出对应信息。

在调试阶段，为了验证磁盘 I/O、中断与 buffer 行为，我们在内核中加入了若干调试输出（如 sb_print()、buffer_print_info() 等），最终版本可根据需要保留或删去。

⸻

二、磁盘设备与中断总体设计

2.1 VirtIO Block 设备简介

本实验使用 QEMU 提供的 VirtIO Block 作为磁盘设备，其特点是：
	•	通过 MMIO 寄存器与内核交互
	•	以 block（512B/4096B）为基本读写单位
	•	I/O 完成后通过外部中断通知内核

因此，要实现磁盘读写，内核需要完成三件事：
	1.	在内核页表中映射 VirtIO 设备寄存器（MMIO）
	2.	在 PLIC 中配置磁盘中断
	3.	在 Trap 中正确响应磁盘中断并唤醒等待进程

⸻

2.2 kvm.c：磁盘 MMIO 映射

在 src/kernel/mem/kvm.c 中，我们在原有内核映射基础上，为 VirtIO 设备补充了 MMIO 映射：

// virtio disk mmio
vm_mappages(pt, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

同时，对 vm_getpte() 进行了扩展，使其支持 pgtbl == NULL 的情况：

pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    if (pgtbl == NULL)
        pgtbl = kernel_pagetable;
    ...
}

这样，磁盘驱动在内核态访问 VirtIO 寄存器时，可以直接通过内核页表完成地址翻译。

⸻

2.3 plic.c 与 trap_kernel.c：磁盘中断链路

在 src/kernel/trap/plic.c 中：
	•	为 VirtIO 磁盘 IRQ 设置优先级
	•	在每个 CPU 上 enable 磁盘中断

在 src/kernel/trap/trap_kernel.c 中：
	•	在外部中断分支中识别磁盘 IRQ
	•	调用磁盘中断处理函数
	•	唤醒因磁盘 I/O 而睡眠的进程

至此，磁盘 I/O 的 提交 → 等待 → 中断 → 唤醒 链路完整闭环。

⸻

三、文件系统初始化与 Superblock

3.1 初始化时机选择

文件系统初始化不能过早进行（如 main() 阶段），否则 sleep/wakeup 机制尚未就绪。

因此，我们选择在 proc_return() 第一次执行时 初始化文件系统，并使用 flag + 锁保证只执行一次：

static int fs_inited = 0;

if (!fs_inited) {
    fs_inited = 1;
    fs_init();
    sb_print();
}


⸻

3.2 fs.c：Superblock 的读取与打印

在 src/kernel/fs/fs.c 中：
	•	fs_init() 负责：
	•	初始化 buffer cache
	•	从磁盘固定 block 读取 superblock
	•	superblock 被保存在全局变量 sb 中
	•	提供 sb_print() 输出磁盘布局信息（inode 区、bitmap 区、data 区等）

该输出在 test-2/test-3 中用于验证磁盘结构是否正确。

⸻

四、Block 缓冲系统（Buffer Cache）

4.1 设计目标

buffer cache 作为磁盘与内存之间的中间层，主要目标是：
	•	减少重复磁盘 I/O
	•	管理 block 对应的内存缓存
	•	支持回收不活跃 buffer 释放内存

⸻

4.2 buffer.c 的核心实现

在 src/kernel/fs/buffer.c 中：
	•	每个 buffer 对应一个磁盘 block
	•	使用 active / inactive 双链表 管理缓存状态
	•	cache miss 时从 inactive 链表尾部回收 buffer
	•	data page 按需 pmem_alloc(true) 分配

核心接口包括：

buffer_t *buffer_get(uint32 block_no);
void buffer_put(buffer_t *b);
void buffer_write(buffer_t *b);
int  buffer_freemem(int n);
void buffer_print_info(void);

buffer_print_info() 在 test-3 中用于观察 buffer 状态变化。

⸻

五、Bitmap 管理

5.1 Bitmap 的作用

磁盘上的 inode 与 data block 均通过 bitmap 管理其分配状态：
	•	inode bitmap：记录 inode 是否被使用
	•	data bitmap：记录 data block 是否被使用

⸻

5.2 bitmap.c 的实现

在 src/kernel/fs/bitmap.c 中实现：
	•	bitmap_alloc_inode / bitmap_free_inode
	•	bitmap_alloc_block / bitmap_free_block
	•	bitmap_print

free 操作中会进行严格的范围检查，防止非法 block/inode 号导致磁盘结构损坏。

⸻

六、系统调用扩展（11~21）

6.1 系统调用设计

为方便用户态测试，本实验新增 11~21 号系统调用，覆盖：
	•	inode / block 的 alloc 与 free
	•	buffer 的 get / read / write / put
	•	bitmap / buffer 状态打印
	•	buffer cache flush

⸻

6.2 syscall.c 与 sysfunc.c

在 syscall.c 中：
	•	扩展 syscall dispatch 表，注册 11~21

在 sysfunc.c 中：
	•	实现对应 sys_* 函数
	•	内部调用 bitmap / buffer / fs 接口完成具体逻辑

系统调用号在 kernel 与 user 两侧保持一致。

⸻

七、用户态测试程序

7.1 test-1

验证用户态启动与基本 syscall 路径是否正确。

预期输出：

hello, world!


⸻

7.2 test-2（Bitmap）

验证 inode / data bitmap 的分配与释放：
	•	alloc 后 bitmap 中对应 bit 被置位
	•	free 后 bit 被清除
	•	不出现越界 panic

⸻

7.3 test-3（Buffer Cache + Disk I/O）

验证：
	•	buffer cache 的复用行为
	•	block 读写一致性
	•	flush 后内存是否被释放

⸻

八、调试与问题分析

在实现过程中，主要遇到以下问题：
	1.	文件系统初始化过早导致 bitmap 全为 0
→ 通过延后至 proc_return() 解决
	2.	磁盘 I/O sleep 后未被唤醒
→ 修正中断号与 wakeup 地址
	3.	bitmap free 越界 panic
→ 增加 superblock 范围检查

通过逐步定位并解决上述问题，最终三个测试均能正常通过。

⸻

九、总结

通过 Lab7 的实现，我们完成了从 磁盘设备驱动 → 中断 → buffer cache → bitmap → 文件系统初始化 → 系统调用 → 用户态测试 的完整链路。

本实验为后续 inode 层、目录与文件操作打下了必要的基础，也进一步加深了对操作系统中 设备驱动与文件系统协作关系 的理解。

---

