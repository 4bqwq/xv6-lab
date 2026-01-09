#pragma once
#include "../arch/type.h"

// 系统调用号
#define SYS_brk 1           // 调整堆边界
#define SYS_mmap 2          // 创建内存映射
#define SYS_munmap 3        // 解除内存映射
#define SYS_print_str 4     // 打印字符串
#define SYS_print_int 5     // 打印32位整数
#define SYS_getpid 6        // 获取当前进程的ID
#define SYS_fork 7          // 进程复制
#define SYS_wait 8          // 等待子进程退出
#define SYS_exit 9          // 进程退出
#define SYS_sleep 10        // 进程睡眠一段时间
#define SYS_helloworld 11   // 新增的系统调用，打印 hello world

void syscall(void);

void arg_uint32(int n, uint32 *ip);
void arg_uint64(int n, uint64 *ip);
void arg_str(int n, char *buf, int maxlen);

uint64 sys_brk();
uint64 sys_mmap();
uint64 sys_munmap();
uint64 sys_print_str();
uint64 sys_print_int();
uint64 sys_fork();
uint64 sys_wait();
uint64 sys_exit();
uint64 sys_sleep();
uint64 sys_getpid();

uint64 sys_helloworld();

uint64 sys_alloc_block();
uint64 sys_free_block();
uint64 sys_alloc_inode();
uint64 sys_free_inode();
uint64 sys_show_bitmap();
uint64 sys_get_block();
uint64 sys_put_block();
uint64 sys_read_block();
uint64 sys_write_block();
uint64 sys_show_buffer();
uint64 sys_flush_buffer();
