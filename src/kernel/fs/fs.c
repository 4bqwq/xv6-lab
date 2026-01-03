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

	inode_t *rooti, *ip_1, *ip_2, *ip_3, *ip_4, *ip_5;
	
	/* 准备测试环境 */

	rooti = inode_get(ROOT_INODE);
	ip_1 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
	ip_2 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
	ip_3 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
	
	inode_lock(rooti);
	inode_lock(ip_1);
	inode_lock(ip_2);
	inode_lock(ip_3);

	if (dentry_create(rooti, ip_1->inode_num, "AABBC") == -1)
		panic("dentry_create fail 1!");
	if (dentry_create(ip_1, ip_2->inode_num, "aaabb") == -1)
		panic("dentry_create fail 2!");
	if (dentry_create(ip_2, ip_3->inode_num, "file.txt") == -1)
		panic("dentry_create fail 3!");

	char tmp1[] = "This is file context!";
	char tmp2[32];
	inode_write_data(ip_3, 0, sizeof(tmp1), tmp1, false);

	inode_rw(rooti, true);
	inode_rw(ip_1, true);
	inode_rw(ip_2, true);

	inode_unlock(rooti);
	inode_unlock(ip_1);
	inode_unlock(ip_2);
	inode_unlock(ip_3);
	inode_put(rooti);
	inode_put(ip_1);
	inode_put(ip_2);
	inode_put(ip_3);

	char *path = "///AABBC///aaabb/file.txt";
	char name[MAXLEN_FILENAME];

	ip_4 = path_to_inode(path);
	if (ip_4 == NULL)
		panic("invalid ip_4");

	ip_5 = path_to_parent_inode(path, name);
	if (ip_5 == NULL)
		panic("invalid ip_5");
	
	printf("get a name = %s\n\n", name);

	inode_lock(ip_4);
	inode_lock(ip_5);

	inode_print(ip_4, "file.txt");
	inode_print(ip_5, "aaabb");

	inode_read_data(ip_4, 0, 32, tmp2, false);
	printf("read data: %s\n\n", tmp2);

	inode_unlock(ip_4);
	inode_unlock(ip_5);
	inode_put(ip_4);
	inode_put(ip_5);

	printf("============= test end =============\n");

	while(1);
}
