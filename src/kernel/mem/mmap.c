#include "mod.h"
#include "../lib/type.h"

// mmap_region_node_t 仓库(单向链表) + 链表头节点(不可分配) + 保护仓库的自旋锁
static mmap_region_node_t node_list[N_MMAP];
static mmap_region_node_t list_head;
static spinlock_t list_lk;

// 初始化上述三个数据结构
void mmap_init()
{
    // 初始化自旋锁
    spinlock_init(&list_lk, "mmap_list");
    
    // 初始化链表头节点
    list_head.next = &(node_list[0]);
    
    // 将所有节点链接成链表
    for(int i = 0; i < N_MMAP - 1; i++) {
        node_list[i].next = &(node_list[i + 1]);
    }
    
    // 最后一个节点指向NULL
    node_list[N_MMAP - 1].next = 0;
}

// 从仓库申请一个 mmap_region_t
// 若仓库空了则 panic
mmap_region_t *mmap_region_alloc()
{
    spinlock_acquire(&list_lk);
    
    // 检查是否有可用节点
    if(list_head.next == 0) {
        spinlock_release(&list_lk);
        panic("mmap_region_alloc: out of nodes");
    }
    
    // 获取第一个可用节点
    mmap_region_node_t *node = list_head.next;
    list_head.next = node->next;  // 从链表中移除该节点
    
    spinlock_release(&list_lk);
    
    // 返回mmap_region部分
    return &(node->mmap);
}

// 向仓库归还一个 mmap_region_t
void mmap_region_free(mmap_region_t *mmap)
{
    // 通过地址计算出对应的mmap_region_node_t
    // mmap_region_t 是 mmap_region_node_t 中的第一个成员，偏移量为0
    mmap_region_node_t *node = (mmap_region_node_t *)mmap;
    
    spinlock_acquire(&list_lk);
    
    // 将节点插入到链表头部
    node->next = list_head.next;
    list_head.next = node;
    
    spinlock_release(&list_lk);
}

// 输出可用的 mmap_region_node_t 链
// for debug
void mmap_show_nodelist()
{
    spinlock_acquire(&list_lk);

    mmap_region_node_t *tmp = list_head.next;
    int node = 0, index = 0;
    while (tmp)
    {
        index = tmp - &(node_list[0]);
        printf("node %d index = %d\n", node++, index);
        tmp = tmp->next;
    }

    spinlock_release(&list_lk);
}
