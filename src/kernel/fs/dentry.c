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
	inode_t *ip;
	if (path[0] == '/') {
		ip = inode_get(ROOT_INODE);
	} else {
		proc_t *p = myproc();
		if (p != NULL && p->cwd != NULL)
			ip = inode_dup(p->cwd);
		else
			ip = inode_get(ROOT_INODE);
	}
	inode_lock(ip);

	while (1) {
		path = get_element(path, name);
		if (path == NULL || name[0] == 0) {
			break;
		}

		/* 如果要找父亲，并且已经到达最后一段，则返回当前 inode */
		if (find_parent_inode && path[0] == 0) {
			inode_unlock(ip);
			return ip;
		}

		if (ip->disk_info.type != INODE_TYPE_DIR) {
			inode_unlock(ip);
			inode_put(ip);
			return NULL;
		}

		uint32 inode_num = dentry_search(ip, name);
		if (inode_num == INVALID_INODE_NUM) {
			inode_unlock(ip);
			inode_put(ip);
			return NULL;
		}

		inode_t *next = inode_get(inode_num);
		inode_lock(next);
		inode_unlock(ip);
		inode_put(ip);
		ip = next;

		if (path[0] == 0)
			break;
	}

	inode_unlock(ip);
	return ip;
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

/*
	在目录ip中查找是否存在序号为inode_num的目录项
	如果存在则将它的名字拷贝到name, 返回name_len
	如果不存在则返回-1
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_search_2(inode_t *ip, uint32 inode_num, char *name)
{
	assert(sleeplock_holding(&ip->slk), "dentry_search_2: slk!");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_search_2: not dir!");
	assert(ip->disk_info.size <= BLOCK_SIZE, "dentry_search_2: dir size");

	if (ip->disk_info.index[0] == 0)
		return (uint32)-1;

	buffer_t *buf = buffer_get(ip->disk_info.index[0]);
	dentry_t *de = (dentry_t *)buf->data;
	uint32 ret = (uint32)-1;

	for (uint32 i = 0; i < DENTRY_PER_BLOCK; i++, de++) {
		if (de->name[0] == 0)
			continue;
		if (de->inode_num == inode_num) {
			uint32 len = strlen(de->name);
			memmove(name, de->name, len + 1);
			ret = len;
			break;
		}
	}
	buffer_put(buf);
	return ret;
}

/*
	向缓冲区[dst, dst + len)中填充有效的dentry
	返回成功填充的数据量(字节)
	注意: 调用者需持有ip->slk
*/
uint32 dentry_transmit(inode_t *ip, uint64 dst, uint32 len, bool is_user_dst)
{
	assert(sleeplock_holding(&ip->slk), "dentry_transmit: slk!");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_transmit: not dir!");
	assert(ip->disk_info.size <= BLOCK_SIZE, "dentry_transmit: dir size");

	if (ip->disk_info.index[0] == 0 || len < sizeof(dentry_t))
		return 0;

	buffer_t *buf = buffer_get(ip->disk_info.index[0]);
	dentry_t *de = (dentry_t *)buf->data;
	uint32 write_len = 0;
	proc_t *p = myproc();

	for (uint32 i = 0; i < DENTRY_PER_BLOCK; i++, de++) {
		if (de->name[0] == 0)
			continue;
		if (write_len + sizeof(dentry_t) > len)
			break;
		if (is_user_dst)
			uvm_copyout(p->pgtbl, dst + write_len, (uint64)de, sizeof(dentry_t));
		else
			memmove((void*)(dst + write_len), de, sizeof(dentry_t));
		write_len += sizeof(dentry_t);
	}
	buffer_put(buf);
	return write_len;
}

/*
	将inode对应的完整路径填入path中(缓冲区长度为len)
	成功返回偏移量(从path+offset开始有效), 失败返回-1
*/
uint32 inode_to_path(inode_t *ip, char *path, uint32 len)
{
	if (len == 0)
		return (uint32)-1;

	uint32 offset = len - 1;
	path[offset] = 0;

	inode_t *cur = inode_dup(ip);
	inode_lock(cur);

	while (true) {
		if (cur->inode_num == ROOT_INODE) {
			if (offset == 0) {
				inode_unlock(cur);
				inode_put(cur);
				return (uint32)-1;
			}
			path[--offset] = '/';
			break;
		}

		if (cur->disk_info.index[0] == 0) {
			inode_unlock(cur);
			inode_put(cur);
			return (uint32)-1;
		}

		buffer_t *buf = buffer_get(cur->disk_info.index[0]);
		dentry_t *de = (dentry_t*)buf->data;
		uint32 parent_inum = de[1].inode_num;
		buffer_put(buf);

		inode_unlock(cur);

		inode_t *parent = inode_get(parent_inum);
		inode_lock(parent);

		char name[MAXLEN_FILENAME];
		uint32 name_len = dentry_search_2(parent, cur->inode_num, name);
		if (name_len == (uint32)-1 || name_len + 1 > offset) {
			inode_unlock(parent);
			inode_put(parent);
			inode_put(cur);
			return (uint32)-1;
		}

		offset -= name_len;
		memmove(path + offset, name, name_len);
		path[--offset] = '/';

		inode_put(cur);
		cur = parent;
	}

	inode_unlock(cur);
	inode_put(cur);
	return offset;
}

/*
	基于path创建新的inode
	成功返回inode, 失败返回NULL
*/
inode_t* path_create_inode(char *path, uint16 type, uint16 major, uint16 minor)
{
	char name[MAXLEN_FILENAME];
	inode_t *parent = path_to_parent_inode(path, name);
	if (parent == NULL || name[0] == 0)
		return NULL;

	inode_lock(parent);
	if (dentry_search(parent, name) != INVALID_INODE_NUM) {
		inode_unlock(parent);
		inode_put(parent);
		return NULL;
	}

	inode_t *ip = inode_create(type, major, minor);
	inode_lock(ip);

	if (type == INODE_TYPE_DIR) {
		if (dentry_create(ip, ip->inode_num, ".") == (uint32)-1 ||
			dentry_create(ip, parent->inode_num, "..") == (uint32)-1) {
			inode_unlock(ip);
			inode_put(ip);
			inode_unlock(parent);
			inode_put(parent);
			return NULL;
		}
		parent->disk_info.nlink++;
		inode_rw(parent, true);
	}

	if (dentry_create(parent, ip->inode_num, name) == (uint32)-1) {
		if (type == INODE_TYPE_DIR) {
			parent->disk_info.nlink--;
			inode_rw(parent, true);
		}
		inode_unlock(ip);
		inode_put(ip);
		inode_unlock(parent);
		inode_put(parent);
		return NULL;
	}

	inode_rw(ip, true);
	inode_unlock(ip);
	inode_unlock(parent);
	inode_put(parent);
	return ip;
}

/*
	构建文件硬链接 (new_path 指向 old_path 指向的 inode)
	核心操作包括 nlink++ 和 dentry_create()
	注意: old_path指向的inode不能是目录类型的
	成功返回0, 失败返回-1
*/
uint32 path_link(char *old_path, char *new_path)
{
	inode_t *old = path_to_inode(old_path);
	if (old == NULL)
		return (uint32)-1;

	inode_lock(old);
	if (old->disk_info.type == INODE_TYPE_DIR) {
		inode_unlock(old);
		inode_put(old);
		return (uint32)-1;
	}
	old->disk_info.nlink++;
	inode_rw(old, true);
	inode_unlock(old);

	char name[MAXLEN_FILENAME];
	inode_t *parent = path_to_parent_inode(new_path, name);
	if (parent == NULL || name[0] == 0) {
		inode_lock(old);
		old->disk_info.nlink--;
		inode_rw(old, true);
		inode_unlock(old);
		inode_put(old);
		return (uint32)-1;
	}

	inode_lock(parent);
	uint32 ret = 0;
	if (dentry_create(parent, old->inode_num, name) == (uint32)-1) {
		inode_lock(old);
		old->disk_info.nlink--;
		inode_rw(old, true);
		inode_unlock(old);
		ret = (uint32)-1;
	}
	inode_rw(parent, true);
	inode_unlock(parent);
	inode_put(parent);
	inode_put(old);
	return ret;
}

/*
	解除文件硬链接
	成功返回0, 失败返回-1
*/
uint32 path_unlink(char *path)
{
	char name[MAXLEN_FILENAME];
	inode_t *parent = path_to_parent_inode(path, name);
	if (parent == NULL || name[0] == 0)
		return (uint32)-1;

	if (strncmp(name, ".", MAXLEN_FILENAME) == 0 || strncmp(name, "..", MAXLEN_FILENAME) == 0) {
		inode_put(parent);
		return (uint32)-1;
	}

	inode_lock(parent);
	uint32 inum = dentry_search(parent, name);
	if (inum == INVALID_INODE_NUM) {
		inode_unlock(parent);
		inode_put(parent);
		return (uint32)-1;
	}

	inode_t *ip = inode_get(inum);
	inode_lock(ip);

	if (ip->disk_info.type == INODE_TYPE_DIR && ip->disk_info.size > sizeof(dentry_t) * 2) {
		inode_unlock(ip);
		inode_put(ip);
		inode_unlock(parent);
		inode_put(parent);
		return (uint32)-1;
	}

	if (dentry_delete(parent, name) == INVALID_INODE_NUM) {
		inode_unlock(ip);
		inode_put(ip);
		inode_unlock(parent);
		inode_put(parent);
		return (uint32)-1;
	}

	if (ip->disk_info.type == INODE_TYPE_DIR) {
		parent->disk_info.nlink--;
		inode_rw(parent, true);
	}

	ip->disk_info.nlink--;
	inode_rw(ip, true);

	inode_unlock(ip);
	inode_put(ip);
	inode_unlock(parent);
	inode_put(parent);
	return 0;
}
