#include "mod.h"
#include "../mem/method.h"

extern super_block_t sb;

/* 内存中的inode资源集合 */
static inode_t inode_cache[N_INODE];
static spinlock_t lk_inode_cache;

/* inode_cache初始化 */
void inode_init()
{
	spinlock_init(&lk_inode_cache, "inode_cache");
	for (int i = 0; i < N_INODE; i++) {
		inode_t *ip = &inode_cache[i];
		ip->valid_info = false;
		ip->inode_num = 0;
		ip->ref = 0;
		memset(&ip->disk_info, 0, sizeof(inode_disk_t));
		sleeplock_init(&ip->slk, "inode");
	}
} 

/*--------------------关于inode->index的增删查操作-----------------*/

/* 
	供free_data_blocks使用
	递归删除inode->index中的一个元素
	返回删除过程中是否遇到空的block_num (文件末尾)
*/
static __attribute__((unused)) bool __free_data_blocks(uint32 block_num, uint32 level)
{
	if (block_num == 0)
		return true;

	if (level == 0) {
		bitmap_free_block(block_num);
		return false;
	}

	buffer_t *b = buffer_get(block_num);
	uint32 *entry = (uint32 *)b->data;
	bool meet_empty = false;

	for (uint32 i = 0; i < BLOCK_SIZE / sizeof(uint32); i++) {
		meet_empty = __free_data_blocks(entry[i], level - 1);
		if (meet_empty)
			break;
	}

	buffer_put(b);
	bitmap_free_block(block_num);
	return meet_empty;
}

/* 
	释放inode管理的blocks
*/
static __attribute__((unused)) void free_data_blocks(uint32 *inode_index)
{
	unsigned int i;
	bool meet_empty = false;

	/* step-1: 释放直接映射的block */
	for (i = 0; i < INODE_INDEX_1; i++)
	{
		meet_empty = __free_data_blocks(inode_index[i], 0);
		if (meet_empty) return;
	}

	/* step-2: 释放一级间接映射的block */
	for (; i < INODE_INDEX_2; i++)
	{
		meet_empty = __free_data_blocks(inode_index[i], 1);
		if (meet_empty) return;
	}

	/* step-3: 释放二级间接映射的block */
	for (; i < INODE_INDEX_3; i++)
	{
		meet_empty = __free_data_blocks(inode_index[i], 2);
		if (meet_empty) return;		
	}

	panic("free_data_blocks: impossible!");
}

/*
	获取inode第logical_block_num个block的物理序号block_num
	调用者保证输入的logical_block_num只有两种情况:
	1. 属于已经分配的区域 (返回block_num)
	2. 将已经分配出去的区域往外扩展1个block (申请block并返回block_num) 
	成功返回block_num, 失败返回-1
*/
static __attribute__((unused)) uint32 locate_or_add_block(uint32 *inode_index, uint32 logical_block_num)
{
	if (logical_block_num >= INODE_BLOCK_INDEX_3)
		return (uint32)-1;

	/* 直接映射 */
	if (logical_block_num < INODE_BLOCK_INDEX_1) {
		uint32 *p = &inode_index[logical_block_num];
		if (*p == 0) {
			uint32 bnum = bitmap_alloc_block();
			if (bnum == (uint32)-1)
				return (uint32)-1;
			*p = bnum;
		}
		return *p;
	}

	/* 一级间接映射 */
	if (logical_block_num < INODE_BLOCK_INDEX_2) {
		uint32 inner = logical_block_num - INODE_BLOCK_INDEX_1;
		uint32 idx_block_idx = inner / 1024;
		uint32 idx_off = inner % 1024;

		uint32 *p_index_block = &inode_index[INODE_INDEX_1 + idx_block_idx];
		if (*p_index_block == 0) {
			uint32 new_block = bitmap_alloc_block();
			if (new_block == (uint32)-1)
				return (uint32)-1;
			*p_index_block = new_block;
			buffer_t *b = buffer_get(new_block);
			memset(b->data, 0, BLOCK_SIZE);
			buffer_write(b);
			buffer_put(b);
		}

		buffer_t *b = buffer_get(*p_index_block);
		uint32 *entry = (uint32 *)b->data + idx_off;
		if (*entry == 0) {
			uint32 new_block = bitmap_alloc_block();
			if (new_block == (uint32)-1) {
				buffer_put(b);
				return (uint32)-1;
			}
			*entry = new_block;
			buffer_write(b);
		}
		uint32 ret = *entry;
		buffer_put(b);
		return ret;
	}

	/* 二级间接映射 */
	uint32 inner = logical_block_num - INODE_BLOCK_INDEX_2;
	uint32 idx_idx = inner / 1024;
	uint32 idx_off = inner % 1024;

	if (inode_index[INODE_INDEX_2] == 0) {
		uint32 new_block = bitmap_alloc_block();
		if (new_block == (uint32)-1)
			return (uint32)-1;
		inode_index[INODE_INDEX_2] = new_block;
		buffer_t *b = buffer_get(new_block);
		memset(b->data, 0, BLOCK_SIZE);
		buffer_write(b);
		buffer_put(b);
	}

	buffer_t *b_idx_idx = buffer_get(inode_index[INODE_INDEX_2]);
	uint32 *idx_idx_base = (uint32 *)b_idx_idx->data;
	if (idx_idx_base[idx_idx] == 0) {
		uint32 new_index_block = bitmap_alloc_block();
		if (new_index_block == (uint32)-1) {
			buffer_put(b_idx_idx);
			return (uint32)-1;
		}
		idx_idx_base[idx_idx] = new_index_block;
		buffer_write(b_idx_idx);

		buffer_t *b_new = buffer_get(new_index_block);
		memset(b_new->data, 0, BLOCK_SIZE);
		buffer_write(b_new);
		buffer_put(b_new);
	}

	buffer_t *b_idx = buffer_get(idx_idx_base[idx_idx]);
	uint32 *entry = (uint32 *)b_idx->data + idx_off;
	if (*entry == 0) {
		uint32 new_block = bitmap_alloc_block();
		if (new_block == (uint32)-1) {
			buffer_put(b_idx);
			buffer_put(b_idx_idx);
			return (uint32)-1;
		}
		*entry = new_block;
		buffer_write(b_idx);
	}
	uint32 ret = *entry;
	buffer_put(b_idx);
	buffer_put(b_idx_idx);
	return ret;
}

/*---------------------关于inode的管理: get dup lock unlock put----------------------*/

/* 
	磁盘里的inode <-> 内存里的inode
	调用者需要持有ip->slk并设置合理的inode_num
*/
void inode_rw(inode_t *ip, bool write)
{
	assert(sleeplock_holding(&ip->slk), "inode_rw: slk");

	uint32 block_num = sb.inode_firstblock + ip->inode_num / INODE_PER_BLOCK;
	uint32 byte_offset = (ip->inode_num % INODE_PER_BLOCK) * sizeof(inode_disk_t);

	buffer_t *buf = buffer_get(block_num);
	if (write) {
		memmove(buf->data + byte_offset, &ip->disk_info, sizeof(inode_disk_t));
		buffer_write(buf);
	} else {
		memmove(&ip->disk_info, buf->data + byte_offset, sizeof(inode_disk_t));
	}
	buffer_put(buf);
}

/*
	尝试在inode_cache里寻找是否存在目标inode
	如果不存在则申请一个空闲的inode
	如果没有空闲位置直接panic
	核心逻辑: ref++
*/
inode_t *inode_get(uint32 inode_num)
{
	inode_t *ip = NULL;

	spinlock_acquire(&lk_inode_cache);

	for (int i = 0; i < N_INODE; i++) {
		if (inode_cache[i].ref > 0 && inode_cache[i].inode_num == inode_num) {
			ip = &inode_cache[i];
			ip->ref++;
			spinlock_release(&lk_inode_cache);
			return ip;
		}
	}

	for (int i = 0; i < N_INODE; i++) {
		if (inode_cache[i].ref == 0) {
			ip = &inode_cache[i];
			ip->inode_num = inode_num;
			ip->valid_info = false;
			ip->ref = 1;
			spinlock_release(&lk_inode_cache);
			return ip;
		}
	}

	spinlock_release(&lk_inode_cache);
	panic("inode_get: no free inode");
	return NULL;
}

/*
	在磁盘里创建1个新的inode
	1. 查询和修改inode_bitmap
	2. 填充inode_region对应位置的inode
	注意: 返回的inode未上锁
*/
inode_t *inode_create(uint16 type, uint16 major, uint16 minor)
{
	uint32 inode_num = bitmap_alloc_inode();
	assert(inode_num != (uint32)-1, "inode_create: alloc fail");

	inode_t *ip = inode_get(inode_num);
	inode_lock(ip);

	ip->disk_info.type = type;
	ip->disk_info.major = major;
	ip->disk_info.minor = minor;
	ip->disk_info.nlink = 1;
	ip->disk_info.size = 0;
	for (int i = 0; i < INODE_INDEX_3; i++)
		ip->disk_info.index[i] = 0;
	ip->valid_info = true;

	inode_rw(ip, true);
	inode_unlock(ip);

	return ip;
}

/*
	ip->ref++ with lock proctect
*/
inode_t* inode_dup(inode_t* ip)
{
	spinlock_acquire(&lk_inode_cache);
	ip->ref++;
	spinlock_release(&lk_inode_cache);
	return ip;
}

/*
	锁住inode
	如果inode->disk_info无效则更新一波
*/
void inode_lock(inode_t* ip)
{
	sleeplock_acquire(&ip->slk);
	if (!ip->valid_info) {
		inode_rw(ip, false);
		ip->valid_info = true;
	}
}

/*
	解锁inode
*/
void inode_unlock(inode_t *ip)
{
	sleeplock_release(&ip->slk);
}

/*
	与inode_get相对应, 调用者释放inode资源
	如果达成某些条件, 可能触发彻底删除
*/
void inode_put(inode_t* ip)
{
	spinlock_acquire(&lk_inode_cache);
	if (ip->ref < 1)
		panic("inode_put: ref < 1");

	if (ip->ref == 1 && ip->valid_info && ip->disk_info.nlink == 0) {
		spinlock_release(&lk_inode_cache);

		inode_lock(ip);
		/* 删除inode并释放资源 */
		inode_delete(ip);
		inode_unlock(ip);

		spinlock_acquire(&lk_inode_cache);
		ip->ref = 0;
		ip->valid_info = false;
		ip->inode_num = 0;
		spinlock_release(&lk_inode_cache);
		return;
	}

	ip->ref--;
	spinlock_release(&lk_inode_cache);
}

/*
	在磁盘里删除1个inode
	1. 修改inode_bitmap释放inode_region资源
	2. 修改block_bitmap释放block_region资源
	注意: 调用者需要持有ip->slk
*/
void inode_delete(inode_t *ip)
{
	assert(sleeplock_holding(&ip->slk), "inode_delete: slk");

	/* 释放数据块资源 */
	free_data_blocks(ip->disk_info.index);

	/* 释放inode位并清空磁盘内容 */
	bitmap_free_inode(ip->inode_num);
	memset(&ip->disk_info, 0, sizeof(inode_disk_t));
	ip->valid_info = false;
	inode_rw(ip, true);
}

/*----------------------基于inode的数据读写操作--------------------*/

/*
	基于inode的数据读取
	inode管理的数据空间逻辑上是一个连续的数组data
	需要拷贝data[offset,offset+len)到dst(用户态地址/内核态地址)
	返回读取的数据量(字节)
*/
uint32 inode_read_data(inode_t *ip, uint32 offset, uint32 len, void *dst, bool is_user_dst)
{
	assert(sleeplock_holding(&ip->slk), "inode_read_data: slk");
	if (ip->disk_info.type == INODE_TYPE_DIR)
		assert(ip->disk_info.size <= BLOCK_SIZE, "inode_read_data: dir size");

	if (offset >= ip->disk_info.size)
		return 0;

	if (offset + len > ip->disk_info.size)
		len = ip->disk_info.size - offset;

	uint32 done = 0;
	while (done < len) {
		uint32 cur_off = offset + done;
		uint32 logical_block = cur_off / BLOCK_SIZE;
		uint32 block_off = cur_off % BLOCK_SIZE;
		uint32 cut = MIN(len - done, BLOCK_SIZE - block_off);

		uint32 block_num = locate_or_add_block(ip->disk_info.index, logical_block);
		assert(block_num != (uint32)-1, "inode_read_data: invalid block");

		buffer_t *b = buffer_get(block_num);
		if (is_user_dst) {
			uvm_copyout(myproc()->pgtbl, (uint64)dst + done, (uint64)(b->data + block_off), cut);
		} else {
			memmove((uint8 *)dst + done, b->data + block_off, cut);
		}
		buffer_put(b);

		done += cut;
	}
	return done;
}

/*
	基于inode的数据写入
	inode管理的数据空间逻辑上是一个连续的数组data
	需要拷贝src(用户态地址/内核态地址)到data[offset,offset+len)
	返回写入的数据量(字节)
*/
uint32 inode_write_data(inode_t *ip, uint32 offset, uint32 len, void *src, bool is_user_src)
{
	assert(sleeplock_holding(&ip->slk), "inode_write_data: slk");
	if (len == 0)
		return 0;

	uint64 max_size = (uint64)INODE_BLOCK_INDEX_3 * BLOCK_SIZE;
	if (offset >= max_size)
		return 0;
	if ((uint64)offset + len > max_size)
		len = (uint32)(max_size - offset);
	if ((uint64)offset + len > INODE_MAX_SIZE)
		len = (uint32)(INODE_MAX_SIZE - offset);

	uint32 done = 0;
	while (done < len) {
		uint32 cur_off = offset + done;
		uint32 logical_block = cur_off / BLOCK_SIZE;
		uint32 block_off = cur_off % BLOCK_SIZE;
		uint32 cut = MIN(len - done, BLOCK_SIZE - block_off);

		uint32 block_num = locate_or_add_block(ip->disk_info.index, logical_block);
		if (block_num == (uint32)-1)
			break;

		buffer_t *b = buffer_get(block_num);
		if (is_user_src) {
			uvm_copyin(myproc()->pgtbl, (uint64)(b->data + block_off), (uint64)src + done, cut);
		} else {
			memmove(b->data + block_off, (uint8 *)src + done, cut);
		}
		buffer_write(b);
		buffer_put(b);

		done += cut;
	}

	uint32 new_size = offset + done;
	if (new_size > ip->disk_info.size)
		ip->disk_info.size = new_size;
	inode_rw(ip, true);

	return done;
}

static char *inode_type_list[] = {"DATA", "DIR", "DEVICE"};

/* 输出inode信息(for debug) */
void inode_print(inode_t *ip, char* name)
{
	assert(sleeplock_holding(&ip->slk), "inode_print: slk");

	spinlock_acquire(&lk_inode_cache);

	printf("inode %s:\n", name);
	printf("ref = %d, inode_num = %d, valid_info = %d\n", ip->ref, ip->inode_num, ip->valid_info);
	printf("type = %s, major = %d, minor = %d, nlink = %d, size = %d\n", inode_type_list[ip->disk_info.type],
		ip->disk_info.major, ip->disk_info.minor, ip->disk_info.nlink, ip->disk_info.size);

	printf("index_list = [ ");
	for (int i = 0; i < INODE_INDEX_1; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("] [ ");
	for (int i = INODE_INDEX_1; i < INODE_INDEX_2; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("] [ ");
	for (int i = INODE_INDEX_2; i < INODE_INDEX_3; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("]\n\n");

	spinlock_release(&lk_inode_cache);
}
