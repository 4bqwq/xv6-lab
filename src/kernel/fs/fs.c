#include "mod.h"

super_block_t sb; /* 超级块 */

// 若 fs/method.h 已声明过这些函数，重复声明不会冲突（前提是签名一致）。
extern void buffer_init(void);
extern buffer_t *buffer_get(uint32 block_num);
extern void buffer_put(buffer_t *buf);

/* 基于superblock输出磁盘布局信息 (for debug) */
void sb_print()
{
    printf("\ndisk layout information:\n");
    printf("1. super block:  block[0]\n");
    printf("2. inode bitmap: block[%d - %d]\n", sb.inode_bitmap_firstblock,
        sb.inode_bitmap_firstblock + sb.inode_bitmap_blocks - 1);
    printf("3. inode region: block[%d - %d]\n", sb.inode_firstblock,
        sb.inode_firstblock + sb.inode_blocks - 1);
    printf("4. data bitmap:  block[%d - %d]\n", sb.data_bitmap_firstblock,
        sb.data_bitmap_firstblock + sb.data_bitmap_blocks - 1);
    printf("5. data region:  block[%d - %d]\n", sb.data_firstblock,
        sb.data_firstblock + sb.data_blocks - 1);
    printf("block size = %d Byte, total size = %d MB, total inode = %d\n\n", sb.block_size,
        (int)((unsigned long long)(sb.total_blocks) * sb.block_size / 1024 / 1024), sb.total_inodes);
}

/* 文件系统初始化 */
void fs_init()
{
    // 1) 先把 buffer 系统拉起来
    buffer_init();

	// 2) 初始化inode_cache
	inode_init();

    // 3) 读入超级块（block 0）到全局 sb
    buffer_t *buf = buffer_get(FS_SB_BLOCK);
    assert(buf && buf->data, "fs_init: failed to read superblock");

    super_block_t *disk_sb = (super_block_t *)buf->data;
    // super_block_t 只有基础字段，直接结构体赋值即可。
    sb = *disk_sb;

    buffer_put(buf);

    // 4) 做最基本的合法性检查，避免你之后 debug 到怀疑人生
    assert(sb.magic_num == FS_MAGIC, "fs_init: bad FS magic");
    assert(sb.block_size == BLOCK_SIZE, "fs_init: unexpected block size");
    assert(sb.total_blocks > 0, "fs_init: total_blocks invalid");

	/* fs_init in fs.c */

	printf("============= test begin =============\n\n");

	inode_t *rooti, *ip_1, *ip_2;
	
	rooti = inode_get(ROOT_INODE);
	inode_lock(rooti);
	inode_print(rooti, "root");
	inode_unlock(rooti);

	/* 第一次查看bitmap */
	bitmap_print(false);

	ip_1 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
	ip_2 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
	inode_lock(ip_1);
	inode_lock(ip_2);
	inode_dup(ip_2);

	inode_print(ip_1, "dir");
	inode_print(ip_2, "data");
	
	/* 第二次查看bitmap */
	bitmap_print(false);

	ip_1->disk_info.nlink = 0;
	ip_2->disk_info.nlink = 0;
	inode_unlock(ip_1);
	inode_unlock(ip_2);
	inode_put(ip_1);
	inode_put(ip_2);

	/* 第三次查看bitmap */
	bitmap_print(false);

	inode_put(ip_2);
	
	/* 第四次查看bitmap */
	bitmap_print(false);

	printf("============= test end =============\n\n");

	while(1);
}
