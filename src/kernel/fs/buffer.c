#include "mod.h"
#include "../mem/method.h"
// virtio.c 在同一模块里实现；若你的工程已在头文件中声明过它，保留这行也不会有副作用。
extern void virtio_disk_rw(buffer_t *b, bool write);

static buffer_node_t buf_cache[N_BUFFER];
static buffer_node_t buf_head_active, buf_head_inactive;
static spinlock_t lk_buf_cache;

/* 
	将一个节点拿出来并插入
	1. 活跃链表的头部 buf_head_active->next
	2. 活跃链表的尾部 buf_head_active->prev
	3. 不活跃链表的头部 buf_head_inactive->next
	4. 不活跃链表的尾部 buf_head_inactive->prev
*/
static void insert_node(buffer_node_t *node, bool insert_active, bool insert_next)
{
	/* 如果有需要, 让node先离开当前位置 */
	if (node->next != NULL && node->prev != NULL) {
		node->next->prev = node->prev;
		node->prev->next = node->next;
	}

	/* 选择目标双向循环链表 */
	buffer_node_t *head = &buf_head_inactive;
	if (insert_active)
		head = &buf_head_active;

	/* 然后将node插入head->next or head->prev */	
	if (insert_next) {
		node->next = head->next;
		node->next->prev = node;
		node->prev = head;
		head->next = node;
	} else {
		node->prev = head->prev;
		node->prev->next = node;
		node->next = head;
		head->prev = node;
	}
}

/* 
	buffer系统初始化：
	1. 初始化全局的lk_buf_cache + buf_head_active + buf_head_inactive
	2. 初始化buf_cache中的所有node, 并将他们放在不活跃链表中
*/
void buffer_init()
{
	spinlock_init(&lk_buf_cache, "buf_cache");

	// 初始化两个带头节点的双向循环链表
	buf_head_active.next = &buf_head_active;
	buf_head_active.prev = &buf_head_active;
	buf_head_inactive.next = &buf_head_inactive;
	buf_head_inactive.prev = &buf_head_inactive;

	// 初始化所有 buffer，并放入不活跃链表。
	// 目标：buf_cache[0] 最终位于 buf_head_inactive.next
	for (int i = (int)N_BUFFER - 1; i >= 0; i--) {
		buffer_node_t *node = &buf_cache[i];
		node->next = NULL;
		node->prev = NULL;

		node->buf.block_num = BLOCK_NUM_UNUSED;
		node->buf.ref = 0;
		node->buf.data = NULL;
		node->buf.disk = false;
		sleeplock_init(&node->buf.slk, "buffer");

		insert_node(node, false, true);
	}
}

/* 磁盘读取: block -> buf */
static void buffer_read(buffer_t *buf)
{
	assert(buf != NULL, "buffer_read: null");
	assert(buf->data != NULL, "buffer_read: data is null");
	assert(buf->block_num != BLOCK_NUM_UNUSED, "buffer_read: invalid block");
	assert(sleeplock_holding(&buf->slk), "buffer_read: slk not held");

	virtio_disk_rw(buf, false);
}

/* 磁盘写入: buf -> block */
void buffer_write(buffer_t *buf)
{
	assert(buf != NULL, "buffer_write: null");
	assert(buf->data != NULL, "buffer_write: data is null");
	assert(buf->block_num != BLOCK_NUM_UNUSED, "buffer_write: invalid block");
	assert(sleeplock_holding(&buf->slk), "buffer_write: slk not held");

	virtio_disk_rw(buf, true);
}

/* 从buf_cache中获取一个buf */
buffer_t* buffer_get(uint32 block_num)
{
	buffer_node_t *node;
	bool need_read = false;

	spinlock_acquire(&lk_buf_cache);

	// 1) 先在活跃链表中寻找
	for (node = buf_head_active.next; node != &buf_head_active; node = node->next) {
		if (node->buf.block_num == block_num) {
			node->buf.ref++;
			insert_node(node, true, true); // move to most-active position
			spinlock_release(&lk_buf_cache);
			sleeplock_acquire(&node->buf.slk);
			return &node->buf;
		}
	}

	// 2) 再在不活跃链表中寻找
	for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next) {
		if (node->buf.block_num == block_num) {
			node->buf.ref++;
			// 从不活跃 -> 活跃，移动到最活跃位置
			insert_node(node, true, true);

			// 自动申请物理内存（只在从不活跃链表取出时做）
			if (node->buf.data == NULL) {
				node->buf.data = (uint8 *)pmem_alloc(true);
				assert(node->buf.data != NULL, "buffer_get: no mem for buffer page");
				memset(node->buf.data, 0, BLOCK_SIZE);
				need_read = true;
			}
			spinlock_release(&lk_buf_cache);

			sleeplock_acquire(&node->buf.slk);
			if (need_read)
				buffer_read(&node->buf);
			return &node->buf;
		}
	}

	// 3) 缓存失败：挑选系统中最不活跃的 buffer（不活跃链表尾部）
	node = buf_head_inactive.prev;
	assert(node != &buf_head_inactive, "buffer_get: no free buffer");

	// 复用该 buffer
	node->buf.block_num = block_num;
	node->buf.ref = 1;
	node->buf.disk = false;

	// miss 时插入活跃链表的尾部（最不活跃）
	insert_node(node, true, false);

	if (node->buf.data == NULL) {
		node->buf.data = (uint8 *)pmem_alloc(true);
		assert(node->buf.data != NULL, "buffer_get: no mem for buffer page");
		memset(node->buf.data, 0, BLOCK_SIZE);
	}
	need_read = true;

	spinlock_release(&lk_buf_cache);

	// 上锁 + 读盘
	sleeplock_acquire(&node->buf.slk);
	if (need_read)
		buffer_read(&node->buf);
	return &node->buf;
}

/* 向buf_cache归还一个buf */
void buffer_put(buffer_t *buf)
{
	assert(buf != NULL, "buffer_put: null");
	assert(sleeplock_holding(&buf->slk), "buffer_put: slk not held");

	// 先放开睡眠锁，避免与 lk_buf_cache 的锁顺序形成死锁
	sleeplock_release(&buf->slk);

	buffer_node_t *node = (buffer_node_t *)buf;

	spinlock_acquire(&lk_buf_cache);
	assert(buf->ref > 0, "buffer_put: ref underflow");
	buf->ref--;
	if (buf->ref == 0) {
		// 回到不活跃链表的最活跃位置
		insert_node(node, false, true);
	}
	spinlock_release(&lk_buf_cache);
}

/*
	从后向前遍历非活跃链表, 尝试释放buffer_count个buffer持有的物理内存(data)
	返回成功释放资源的buffer数量
*/
uint32 buffer_freemem(uint32 buffer_count)
{
	uint32 freed = 0;
	buffer_node_t *node;

	spinlock_acquire(&lk_buf_cache);

	// 从不活跃链表尾部开始（最不活跃）向前扫描
	for (node = buf_head_inactive.prev;
	     node != &buf_head_inactive && freed < buffer_count;
	     node = node->prev) {

		buffer_t *b = &node->buf;
		if (b->data == NULL)
			continue;
		assert(b->ref == 0, "buffer_freemem: nonzero ref in inactive list");

		// 不活跃 buffer 理论上没人持有 slk；这里拿一下，保证 data 的一致性
		sleeplock_acquire(&b->slk);
		if (b->data != NULL) {
			pmem_free((uint64)b->data, true);
			b->data = NULL;
			b->disk = false;
			freed++;
		}
		sleeplock_release(&b->slk);
	}

	spinlock_release(&lk_buf_cache);
	return freed;
}

/* 输出buffer_cache的信息 (for test) */
void buffer_print_info()
{
	buffer_node_t *node;

	assert(N_BUFFER == N_BUFFER_TEST, "buffer_print_info: invalid N_BUFFER");

	spinlock_acquire(&lk_buf_cache);

	printf("buffer_cache information:\n");
	
	printf("1.active list:\n");
	for (node = buf_head_active.next; node != &buf_head_active; node = node->next) {
		printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
			(int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
	}
	printf("over!\n");

	printf("2.inactive list:\n");
	for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next) {
		printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
			(int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
	}
	printf("over!\n");

	spinlock_release(&lk_buf_cache);
}

