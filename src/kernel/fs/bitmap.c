#include "mod.h"

extern super_block_t sb;

// 若 fs/method.h 已声明过这些函数，重复声明不会冲突（前提是签名一致）。
extern buffer_t *buffer_get(uint32 block_num);
extern void buffer_put(buffer_t *buf);
extern void buffer_write(buffer_t *buf);

/*
    查询一个block中的所有bit, 找到空闲bit, 设置1并返回
    如果没有空闲bit, 返回-1
*/
static uint32 bitmap_search_and_set(uint32 bitmap_block_num, uint32 valid_count)
{
    assert(valid_count <= BIT_PER_BLOCK, "bitmap_search_and_set: valid_count overflow");

    buffer_t *buf = buffer_get(bitmap_block_num);
    assert(buf && buf->data, "bitmap_search_and_set: bad buffer");
    assert(sleeplock_holding(&buf->slk), "bitmap_search_and_set: buffer not locked");

    uint32 idx = 0;
    for (uint32 byte = 0; byte < BLOCK_SIZE && idx < valid_count; byte++) {
        uint8 v = buf->data[byte];

        // 仅当这一整字节都在有效范围内且全为1，才可以快速跳过。
        uint32 remain = valid_count - idx;
        if (remain >= BIT_PER_BYTE && v == 0xFF) {
            idx += BIT_PER_BYTE;
            continue;
        }

        for (uint32 bit = 0; bit < BIT_PER_BYTE && idx < valid_count; bit++, idx++) {
            uint8 mask = (uint8)(1U << bit);
            if ((v & mask) == 0) {
                // 找到空闲bit：置1并写回磁盘
                buf->data[byte] = (uint8)(v | mask);
                buffer_write(buf);
                buffer_put(buf);
                return idx;
            }
        }
    }

    buffer_put(buf);
    return (uint32)-1;
}

/*
    将block中第index个bit设为0
*/
static void bitmap_clear(uint32 bitmap_block_num, uint32 index)
{
    assert(index < BIT_PER_BLOCK, "bitmap_clear: index overflow");

    buffer_t *buf = buffer_get(bitmap_block_num);
    assert(buf && buf->data, "bitmap_clear: bad buffer");
    assert(sleeplock_holding(&buf->slk), "bitmap_clear: buffer not locked");

    uint32 byte = index / BIT_PER_BYTE;
    uint32 bit = index % BIT_PER_BYTE;
    uint8 mask = (uint8)(1U << bit);
    buf->data[byte] = (uint8)(buf->data[byte] & ~mask);

    buffer_write(buf);
    buffer_put(buf);
}

/*
    获取一个空闲block, 将data_bitmap对应bit设为1
    返回这个block的全局序号
*/
uint32 bitmap_alloc_block()
{
    uint32 total_bits = sb.data_blocks;
    uint32 bit_base = 0;

    for (uint32 blk = 0; blk < sb.data_bitmap_blocks && bit_base < total_bits; blk++) {
        uint32 bitmap_block_num = sb.data_bitmap_firstblock + blk;
        uint32 valid = BIT_PER_BLOCK;
        if (bit_base + BIT_PER_BLOCK > total_bits)
            valid = total_bits - bit_base;

        uint32 idx = bitmap_search_and_set(bitmap_block_num, valid);
        if (idx != (uint32)-1)
            return sb.data_firstblock + bit_base + idx;

        bit_base += BIT_PER_BLOCK;
    }

    return (uint32)-1;
}

/*
    获取一个空闲inode, 将inode_bitmap对应bit设为1
    返回这个inode的全局序号
*/
uint32 bitmap_alloc_inode()
{
    uint32 total_bits = sb.total_inodes;
    uint32 bit_base = 0;

    for (uint32 blk = 0; blk < sb.inode_bitmap_blocks && bit_base < total_bits; blk++) {
        uint32 bitmap_block_num = sb.inode_bitmap_firstblock + blk;
        uint32 valid = BIT_PER_BLOCK;
        if (bit_base + BIT_PER_BLOCK > total_bits)
            valid = total_bits - bit_base;

        uint32 idx = bitmap_search_and_set(bitmap_block_num, valid);
        if (idx != (uint32)-1)
            return bit_base + idx;

        bit_base += BIT_PER_BLOCK;
    }

    return (uint32)-1;
}

/* 释放一个block, 将data_bitmap对应bit设为0 */
void bitmap_free_block(uint32 block_num)
{
    // block_num 是磁盘全局块号，必须落在 data region 范围内
    assert(block_num >= sb.data_firstblock, "bitmap_free_block: below data region");
    uint32 idx = block_num - sb.data_firstblock;
    assert(idx < sb.data_blocks, "bitmap_free_block: out of data range");

    uint32 bitmap_block_num = sb.data_bitmap_firstblock + (idx / BIT_PER_BLOCK);
    uint32 off = idx % BIT_PER_BLOCK;
    bitmap_clear(bitmap_block_num, off);
}

/* 释放一个inode, 将inode_bitmap对应bit设为0 */
void bitmap_free_inode(uint32 inode_num)
{
    assert(inode_num < sb.total_inodes, "bitmap_free_inode: out of inode range");

    uint32 bitmap_block_num = sb.inode_bitmap_firstblock + (inode_num / BIT_PER_BLOCK);
    uint32 off = inode_num % BIT_PER_BLOCK;
    bitmap_clear(bitmap_block_num, off);
}

/* 打印某个bitmap中所有分配出去的bit */
void bitmap_print(bool print_data_bitmap)
{
    uint32 first_block, bitmap_blocks, total_bits;
    uint32 global_base, current_bit = 0;

    if (print_data_bitmap) {
        printf("data bitmap alloced bits:\n");
        first_block = sb.data_bitmap_firstblock;
        bitmap_blocks = sb.data_bitmap_blocks;
        total_bits = sb.data_blocks;
        global_base = sb.data_firstblock;
    } else {
        printf("inode bitmap alloced bits:\n");
        first_block = sb.inode_bitmap_firstblock;
        bitmap_blocks = sb.inode_bitmap_blocks;
        total_bits = sb.total_inodes;
        global_base = 0;
    }

    for (uint32 block = 0; block < bitmap_blocks; block++)
    {
        uint32 bitmap_block_num = first_block + block;
        uint32 bits_in_this_block = BIT_PER_BLOCK;

        // 最后一个 block 可能不满
        if (current_bit + BIT_PER_BLOCK > total_bits)
            bits_in_this_block = total_bits - current_bit;

        buffer_t *buf = buffer_get(bitmap_block_num);

        // 遍历该 block 中的有效 bit
        for (uint32 byte = 0; byte < bits_in_this_block / BIT_PER_BYTE; byte++)
        {
            for (uint32 shift = 0; shift < BIT_PER_BYTE; shift++)
            {
                if (current_bit >= total_bits)
                    break;

                uint8 mask = (uint8)(1U << shift);
                if (buf->data[byte] & mask)
                    printf("%d ", global_base + current_bit);
                current_bit++;
            }
        }
        buffer_put(buf);
    }
    printf("over!\n\n");
}

