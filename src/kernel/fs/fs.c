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

	inode_t *ip_1, *ip_2;
	uint32 len, cut_len;

	/* 小批量读写测试 */

	int small_src[10], small_dst[10];
	for (int i = 0; i < 10; i++)
		small_src[i] = i;
	
	ip_1 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
	inode_lock(ip_1);
	inode_print(ip_1, "small_data");

	printf("writing data...\n\n");
	cut_len = 10 * sizeof(int);
	for (uint32 offset = 0; offset < 400 * cut_len; offset += cut_len) {
		len = inode_write_data(ip_1, offset, cut_len, small_src, false);
		assert(len == cut_len, "write fail 1!");
	}
	inode_print(ip_1, "small_data");

	len = inode_read_data(ip_1, 120 * cut_len + 4, cut_len, small_dst, false);
	assert(len == cut_len, "read fail 1!");
	printf("read data:");
	for (int i = 0; i < 10; i++)
		printf(" %d", small_dst[i]);
	printf("\n\n");

	ip_1->disk_info.nlink = 0;
	inode_unlock(ip_1);
	inode_put(ip_1);

	/* 大批量读写测试 */

	char *big_src, big_dst[9];
	big_dst[8] = 0;

	/* 申请五个连续物理页面 (初始化阶段, 通常来说能拿到连续的) */
	big_src = pmem_alloc(true);
	assert(pmem_alloc(true) == big_src + PGSIZE, "contiguous fail!");
	assert(pmem_alloc(true) == big_src + PGSIZE * 2, "contiguous fail!");
	assert(pmem_alloc(true) == big_src + PGSIZE * 3, "contiguous fail!");
	assert(pmem_alloc(true) == big_src + PGSIZE * 4, "contiguous fail!");

	for (uint32 i = 0; i < 5 * (PGSIZE / 8); i++)
		for (uint32 j = 0; j < 8; j++)
			big_src[i * 8 + j] = 'A' + j;

	ip_2 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
	inode_lock(ip_2);
	inode_print(ip_2, "big_data");

	printf("writing data...\n\n");
	cut_len = PGSIZE * 4 + 1110;
	for (uint32 offset = 0; offset < cut_len * 150; offset += cut_len)
	{
		len = inode_write_data(ip_2, offset, cut_len, big_src, false);
		assert(len == cut_len, "write fail 2!");
	}
	inode_print(ip_2, "big_data");

	len = inode_read_data(ip_1, cut_len * 150 - 8, 8, big_dst, false);
	assert(len == 8, "read fail 2");
	printf("read data: %s\n", big_dst);


	ip_2->disk_info.nlink = 0;
	inode_unlock(ip_2);
	inode_put(ip_2);

	pmem_free((uint64)big_src, true);
	pmem_free((uint64)big_src + PGSIZE, true);
	pmem_free((uint64)big_src + PGSIZE * 2, true);
	pmem_free((uint64)big_src + PGSIZE * 3, true);
	pmem_free((uint64)big_src + PGSIZE * 4, true);


	printf("============= test end =============\n");

	while(1);
}
