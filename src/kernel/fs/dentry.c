#include "mod.h"

/*
	出于简化目的的假设:
	如果inode_disk.type == INODE_TYPE_DIR
	那么inode_disk.size <= BLOCKSIZE (只有inode_disk.index[0]有效)
	也就是说, 单个目录最多包含BLOCKSIZE / sizeof(dentry)个目录项

	另外, INODE_TYPE_DATA要求数据之间没有空隙
	但是对于INODE_TYPE_DIR来说是无法做到的(目录项的删除很常见)
	因此, ip->size代表block中已经使用的空间大小
*/


/*----------------dentry的查找、增加、删除操作-----------------*/

/*
	在目录ip中查找是否存在名字为name的目录项
	如果找到了返回目录项中存储的inode_num
	如果没找到返回INVALID_INODE_NUM
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_search(inode_t *ip, char *name)
{
	assert(sleeplock_holding(&ip->slk), "dentry_search: slk!");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_search: not dir!");
	assert(ip->disk_info.size <= BLOCK_SIZE, "dentry_search: dir size");

	if (ip->disk_info.index[0] == 0)
		return INVALID_INODE_NUM;

	buffer_t *buf = buffer_get(ip->disk_info.index[0]);
	dentry_t *de = (dentry_t *)(buf->data);
	uint32 ret = INVALID_INODE_NUM;

	for (uint32 i = 0; i < DENTRY_PER_BLOCK; i++, de++) {
		if (de->name[0] == 0)
			continue;
		if (strncmp(de->name, name, MAXLEN_FILENAME) == 0) {
			ret = de->inode_num;
			break;
		}
	}

	buffer_put(buf);
	return ret;
}

/*
	在目录ip中寻找空闲槽位, 插入新的dentry
	如果成功插入则返回这个目录项的偏移量(还需要更新size)
	如果插入失败(没有空间/发生重名)返回-1
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_create(inode_t *ip, uint32 inode_num, char *name)
{
	assert(sleeplock_holding(&ip->slk), "dentry_create: slk!");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_create: not dir!");
	assert(ip->disk_info.size <= BLOCK_SIZE, "dentry_create: dir size");

	/* 确保目录数据块存在 */
	if (ip->disk_info.index[0] == 0) {
		uint32 block_num = bitmap_alloc_block();
		assert(block_num != (uint32)-1, "dentry_create: alloc fail");
		ip->disk_info.index[0] = block_num;

		buffer_t *b = buffer_get(block_num);
		memset(b->data, 0, BLOCK_SIZE);
		buffer_write(b);
		buffer_put(b);
	}

	buffer_t *buf = buffer_get(ip->disk_info.index[0]);
	dentry_t *free_slot = NULL;
	uint32 free_off = 0;

	for (uint32 i = 0; i < DENTRY_PER_BLOCK; i++) {
		dentry_t *de = (dentry_t *)(buf->data + i * sizeof(dentry_t));
		if (de->name[0] != 0) {
			if (strncmp(de->name, name, MAXLEN_FILENAME) == 0) {
				buffer_put(buf);
				return (uint32)-1; /* 重名 */
			}
			continue;
		}
		if (free_slot == NULL) {
			free_slot = de;
			free_off = i * sizeof(dentry_t);
		}
	}

	if (free_slot == NULL) {
		buffer_put(buf);
		return (uint32)-1;
	}

	memset(free_slot, 0, sizeof(dentry_t));
	memmove(free_slot->name, name, MAXLEN_FILENAME - 1);
	free_slot->name[MAXLEN_FILENAME - 1] = 0;
	free_slot->inode_num = inode_num;

	buffer_write(buf);
	buffer_put(buf);

	uint32 new_size = free_off + sizeof(dentry_t);
	if (new_size > ip->disk_info.size)
		ip->disk_info.size = new_size;
	inode_rw(ip, true);

	return free_off;
}

/*
	在目录ip下删除名称为name的dentry, 返回它的inode_num
	如果匹配失败或者遇到非法情况返回INVALID_INODE_NUM
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_delete(inode_t *ip, char *name)
{
	assert(sleeplock_holding(&ip->slk), "dentry_delete: slk!");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_delete: not dir!");
	assert(ip->disk_info.size <= BLOCK_SIZE, "dentry_delete: dir size");

	if (ip->disk_info.index[0] == 0)
		return INVALID_INODE_NUM;

	buffer_t *buf = buffer_get(ip->disk_info.index[0]);
	dentry_t *base = (dentry_t *)buf->data;
	dentry_t *target = NULL;
	uint32 ret = INVALID_INODE_NUM;

	for (uint32 i = 0; i < DENTRY_PER_BLOCK; i++) {
		dentry_t *de = base + i;
		if (de->name[0] == 0)
			continue;
		if (strncmp(de->name, name, MAXLEN_FILENAME) == 0) {
			target = de;
			ret = de->inode_num;
			break;
		}
	}

	if (target != NULL) {
		memset(target, 0, sizeof(dentry_t));
		buffer_write(buf);

		uint32 new_size = ip->disk_info.size;
		while (new_size > 0) {
			uint32 off = new_size - sizeof(dentry_t);
			dentry_t *de = base + off / sizeof(dentry_t);
			if (de->name[0] != 0)
				break;
			new_size -= sizeof(dentry_t);
		}
		ip->disk_info.size = new_size;
		inode_rw(ip, true);
	}

	buffer_put(buf);
	return ret;
}

/* 输出目录中所有有效目录项的信息 (for debug) */
void dentry_print(inode_t *ip)
{
	assert(sleeplock_holding(&ip->slk), "dentry_print: slk!");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_print: not dir!");

	dentry_t *de;
	buffer_t *buf;

	if (ip->disk_info.index[0] == 0)
		panic("dentry_print: invalid index[0]!");
	
	printf("inode_num = %d, dentries:\n", ip->inode_num);

	buf = buffer_get(ip->disk_info.index[0]);
	for (de = (dentry_t*)(buf->data); de < (dentry_t*)(buf->data + BLOCK_SIZE); de++)
	{
		if (de->name[0] != 0) {
			printf("dentry: offset = %d, inode_num = %d, name = %s\n",
				(uint32)((uint8*)de - buf->data), de->inode_num, de->name);
		}
	}
	buffer_put(buf);

	printf("\n");
}

/*------------------从文件名到文件路径-----------------*/

/*
	Examples:
	get_element("a/bb/c", name) = "bb/c" + name = "a"
	get_element("///aa//bb", name) = "bb" + name = "aa"
	get_element("aaa", name) = "" + name = "aaa"
	get_element("", name) = NULL + name = ""
	get_element("//", name) = NULL + name = ""
*/
static __attribute__((unused)) char* get_element(char *path, char *name)
{
	/* 跳过前置的'/' */
    while (*path == '/')
		path++;

	/* 如果遇到末尾了则返回 */
    if (*path == 0) {
		name[0] = 0;
		return NULL;
	}

	/* 记录起点位置 */
    char *start = path;
    
	/* 推进path直到遇到'/'或者到达末尾 */
	while (*path != '/' && *path != 0)
        path++;

	/* 提取到的name的长度 */
    int len = path - start;
	len = MIN(len, MAXLEN_FILENAME-1);
	
	/* 设置name */
	memmove(name, start, len);
	name[len] = 0;

	/* 跳过后置的'/' */
    while (*path == '/') path++;

    return path;
}
/*
	根据文件路径(/A/B/C)查找对应inode(inode_B or inode_C)
	如果find_parent_inode == true, 返回父节点inode, name为下一级子节点的名字
	如果find_parent_inode == false, 返回子节点inode, name无意义
	如果失败返回NULL
*/
static inode_t* __path_to_inode(char *path, char *name, bool find_parent_inode)
{
	(void)path;
	(void)name;
	(void)find_parent_inode;
	return NULL;
}

/*
	基于path寻找inode
	失败返回NULL
*/
inode_t* path_to_inode(char *path)
{
	char name[MAXLEN_FILENAME];
	return __path_to_inode(path, name, false);
}

/* 
	基于path寻找inode->parent, 将inode->name放入name
	失败返回NULL, 同时name无效
*/
inode_t* path_to_parent_inode(char *path, char *name)
{
	return __path_to_inode(path, name, true);
}
