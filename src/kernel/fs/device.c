#include "mod.h"

device_t device_table[N_DEVICE];

// 标准输入设备
static uint32 device_stdin_read(uint32 len, uint64 dst, bool is_user_dst) {
    return cons_read(len, dst, is_user_dst);  // 从控制台读取数据
}

// 标准输出设备
static uint32 device_stdout_write(uint32 len, uint64 src, bool is_user_src) {
    return cons_write(len, src, is_user_src);  // 向控制台写数据
}

// 标准错误输出设备
static uint32 device_stderr_write(uint32 len, uint64 src, bool is_user_src) {
    printf("ERROR: ");
    return cons_write(len, src, is_user_src);  // 向控制台写错误信息
}

// 无限0流
static uint32 device_zero_read(uint32 len, uint64 dst, bool is_user_dst) {
    // 永远读取0字节
    return 0;
}

// 初始化设备文件
void device_init() {
    device_table[0].read = device_stdin_read;
    device_table[0].write = device_stdout_write;
    device_table[1].write = device_stderr_write;
    device_table[2].read = device_zero_read;  // /dev/zero
    // 可以继续初始化其他设备文件
}

// 设备文件操作逻辑
uint32 device_read(uint16 major, uint32 len, uint64 dst, bool is_user_dst) {
    if (major < N_DEVICE) {
        return device_table[major].read(len, dst, is_user_dst);
    }
    return 0;
}

uint32 device_write(uint16 major, uint32 len, uint64 src, bool is_user_src) {
    if (major < N_DEVICE) {
        return device_table[major].write(len, src, is_user_src);
    }
    return 0;
}
