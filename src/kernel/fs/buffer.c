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
    printf("buffer_init begin\n");
    // 初始化全局的lk_buf_cache + buf_head_active + buf_head_inactive
    spinlock_init(&lk_buf_cache, "buf_cache");

    // 初始化两个带头节点的双向循环链表
    buf_head_active.next = &buf_head_active;
    buf_head_active.prev = &buf_head_active;
    buf_head_inactive.next = &buf_head_inactive;
    buf_head_inactive.prev = &buf_head_inactive;

    // 初始化buf_cache中的所有node, 并将他们放在不活跃链表中
    for (int i = 0; i < N_BUFFER; i++)
    {
        buffer_node_t *node = &buf_cache[i];

        node->next = buf_head_inactive.next;
        node->prev = &buf_head_inactive;
        buf_head_inactive.next->prev = node;
        buf_head_inactive.next = node;

        node->buf.block_num = BLOCK_NUM_UNUSED;
        node->buf.ref = 0;
        node->buf.data = NULL;
        node->buf.disk = false;
        sleeplock_init(&node->buf.slk, "buffer");
    }
    printf("buffer_init end\n");
}

/* 磁盘读取: block -> buf */
static void buffer_read(buffer_t *buf)
{
    virtio_disk_rw(buf, false);
}

/* 磁盘写入: buf -> block */
void buffer_write(buffer_t *buf)
{
    virtio_disk_rw(buf, true);
}

/* 从buf_cache中获取一个buf */
buffer_t* buffer_get(uint32 block_num)
{
    buffer_node_t *node = NULL, *target = NULL;
    bool need_read = false;

    spinlock_acquire(&lk_buf_cache);

    // 在活跃链表中寻找
    for (node = buf_head_active.next; node != &buf_head_active; node = node->next) {
        if (node->buf.block_num == block_num) {
            target = node;
            insert_node(target, true, true);
            target->buf.ref++;
            need_read = !target->buf.disk;
            break;
        }
    }

    // 在不活跃链表中寻找
    if (target == NULL) {
        for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next) {
            if (node->buf.block_num == block_num) {
                target = node;
                insert_node(target, true, true);
                target->buf.ref++;
                need_read = !target->buf.disk;
                break;
            }
        }
    }

    // 如果没找到，将最不活跃的不活跃节点移动到活跃链表尾部
    if (target == NULL) {
        target = buf_head_inactive.prev;
        insert_node(target, true, false);

        target->buf.block_num = block_num;
        target->buf.ref = 1;
        target->buf.disk = false;
        need_read = true;
    }

    // 物理内存
    if (target->buf.data == NULL) {
        target->buf.data = (uint8 *)pmem_alloc(true);
        memset(target->buf.data, 0, BLOCK_SIZE);
    }

    // 释放自旋锁，获取睡眠锁
    spinlock_release(&lk_buf_cache);
    sleeplock_acquire(&target->buf.slk);

    //  miss（来自不活跃链表）读盘
    if (need_read) {
        buffer_read(&target->buf);
        target->buf.disk = true;
    }

    return &target->buf;
}

// 检查指针是否合法
bool is_valid_buffer(buffer_t *b) {
    if (b == NULL) return false;
    uint64 start = (uint64)&buf_cache[0];
    uint64 end = (uint64)&buf_cache[N_BUFFER];

    uint64 ptr = (uint64)b;

    // 检查是否在地址范围内
    if (ptr < start || ptr >= end) return false;

    // 检查是否对齐
    if ((ptr - start) % sizeof(buffer_node_t) != 0) return false;

    return true;
}

/* 向buf_cache归还一个buf */
void buffer_put(buffer_t *buf)
{
    buffer_node_t *target = (buffer_node_t *)buf;

    sleeplock_release(&buf->slk);

    spinlock_acquire(&lk_buf_cache);

    if (buf->ref > 0) {
        buf->ref--;

        // 如果引用为0，放到不活跃链表头部
        if (buf->ref == 0) {
            insert_node(target, false, true);
        }
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
    buffer_node_t *node = NULL;

    spinlock_acquire(&lk_buf_cache);

    // 从后向前遍历非活跃链表
    for (node = buf_head_inactive.prev;
        node != &buf_head_inactive && freed < buffer_count;
        node = node->prev) {

        buffer_t *b = &node->buf;

        // 申请过物理页，且没人持有过，才能释放
        if (b->ref == 0 && b->data != NULL) {
            pmem_free((uint64)b->data, true);
            b->data = NULL;
            
            b->block_num = BLOCK_NUM_UNUSED; 
            b->disk = false;

            freed++;
        }
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
            (int)(node - buf_cache), node->buf.ref, node->buf.data, node->buf.block_num);
    }
    printf("over!\n");

    printf("2.inactive list:\n");
    for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next) {
        printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
            (int)(node - buf_cache), node->buf.ref, node->buf.data, node->buf.block_num);
    }
    printf("over!\n");

    spinlock_release(&lk_buf_cache);
}
