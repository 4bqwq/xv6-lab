#include "mod.h"

super_block_t sb; /* 超级块 */

file_t file_table[N_FILE]; // 文件资源池
spinlock_t lk_file_table;  // 保护file_table的锁

static void file_free(file_t *f)
{
    f->ip = NULL;
    f->readable = false;
    f->writbale = false;
    f->offset = 0;
    f->ref = 0;
}

/* 初始化file_table */
void file_init()
{
    spinlock_init(&lk_file_table, "file_table");
    for (int i = 0; i < N_FILE; i++)
        file_free(&file_table[i]);
}

/* 从file_table中获取1个空闲file */
file_t* file_alloc()
{
    spinlock_acquire(&lk_file_table);
    for (int i = 0; i < N_FILE; i++) {
        if (file_table[i].ref == 0) {
            file_table[i].ref = 1;
            spinlock_release(&lk_file_table);
            return &file_table[i];
        }
    }
    spinlock_release(&lk_file_table);
    return NULL;
}

/*
    根据路径打开文件 (指定打开模式)
    成功返回file, 失败返回NULL
*/
file_t* file_open(char *path, uint32 open_mode)
{
    inode_t *ip;

    if (open_mode & FILE_OPEN_CREATE) {
        ip = path_create_inode(path, INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    } else {
        ip = path_to_inode(path);
    }

    if (ip == NULL)
        return NULL;

    inode_lock(ip);
    if (ip->disk_info.type == INODE_TYPE_DIVICE) {
        inode_unlock(ip);
        if (!device_open_check(ip->disk_info.major, open_mode)) {
            inode_put(ip);
            return NULL;
        }
    } else {
        inode_unlock(ip);
    }

    file_t *f = file_alloc();
    if (f == NULL) {
        inode_put(ip);
        return NULL;
    }

    f->ip = ip;
    f->readable = open_mode & FILE_OPEN_READ;
    f->writbale = open_mode & FILE_OPEN_WRITE;
    f->offset = 0;

    return f;
}

/* 关闭文件 */
void file_close(file_t *file)
{
    spinlock_acquire(&lk_file_table);
    if (file->ref > 0)
        file->ref--;
    bool free_it = (file->ref == 0);
    spinlock_release(&lk_file_table);

    if (free_it) {
        inode_put(file->ip);
        file_free(file);
    }
}

/* 读取文件内容, 返回读到的字节数量 */
uint32 file_read(file_t* file, uint32 len, uint64 dst, bool is_user_dst)
{
    if (file == NULL || !file->readable)
        return (uint32)-1;

    inode_t *ip = file->ip;
    uint32 n = 0;

    switch (ip->disk_info.type) {
    case INODE_TYPE_DATA:
        inode_lock(ip);
        n = inode_read_data(ip, file->offset, len, (void*)dst, is_user_dst);
        file->offset += n;
        inode_unlock(ip);
        break;
    case INODE_TYPE_DIR:
        inode_lock(ip);
        n = dentry_transmit(ip, dst, len, is_user_dst);
        inode_unlock(ip);
        break;
    case INODE_TYPE_DIVICE:
        n = device_read_data(ip->disk_info.major, len, dst, is_user_dst);
        break;
    default:
        break;
    }
    return n;
}

/* 读取文件内容, 返回读到的字节数量 */
uint32 file_write(file_t* file, uint32 len, uint64 src, bool is_user_src)
{
    if (file == NULL || !file->writbale)
        return (uint32)-1;

    inode_t *ip = file->ip;
    uint32 n = 0;

    switch (ip->disk_info.type) {
    case INODE_TYPE_DATA:
        inode_lock(ip);
        n = inode_write_data(ip, file->offset, len, (void*)src, is_user_src);
        file->offset += n;
        inode_unlock(ip);
        break;
    case INODE_TYPE_DIR:
        n = 0;
        break;
    case INODE_TYPE_DIVICE:
        n = device_write_data(ip->disk_info.major, len, src, is_user_src);
        break;
    default:
        break;
    }
    return n;
}

/* 
    读/写指针的移动
    对于不合理的lseek_offset, 只做尽力而为的移动
    返回新的file->offset
*/
uint32 file_lseek(file_t *file, uint32 lseek_offset, uint32 lseek_flag)
{
    if (file == NULL)
        return (uint32)-1;

    uint32 new_off = file->offset;
    if (lseek_flag == FILE_LSEEK_SET) {
        new_off = lseek_offset;
    } else if (lseek_flag == FILE_LSEEK_ADD) {
        new_off += lseek_offset;
    } else if (lseek_flag == FILE_LSEEK_SUB) {
        new_off = (lseek_offset > new_off) ? 0 : new_off - lseek_offset;
    }
    file->offset = new_off;
    return file->offset;
}

/* file->ref++ with lock protect */
file_t* file_dup(file_t* file)
{
    spinlock_acquire(&lk_file_table);
    file->ref++;
    spinlock_release(&lk_file_table);
    return file;
}

/* 获取文件参数, 成功返回0, 失败返回-1 */
uint32 file_get_stat(file_t* file, uint64 user_dst)
{
    if (file == NULL)
        return (uint32)-1;

    proc_t *p = myproc();
    file_stat_t st;

    inode_t *ip = file->ip;
    inode_lock(ip);
    st.type = ip->disk_info.type;
    st.nlink = ip->disk_info.nlink;
    st.size = ip->disk_info.size;
    st.inode_num = ip->inode_num;
    inode_unlock(ip);
    st.offset = file->offset;

    uvm_copyout(p->pgtbl, user_dst, (uint64)&st, sizeof(st));
    return 0;
}

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
    printf("fs_init: begin\n");
    // 1) buffer 系统
    buffer_init();

    // 2) 初始化inode_cache
    printf("fs_init: inode_init\n");
    inode_init();

    // 3) 读入超级块
    printf("fs_init: read superblock\n");
    buffer_t *buf = buffer_get(FS_SB_BLOCK);
    assert(buf && buf->data, "fs_init: failed to read superblock");
    super_block_t *disk_sb = (super_block_t *)buf->data;
    sb = *disk_sb;
    buffer_put(buf);

    assert(sb.magic_num == FS_MAGIC, "fs_init: bad FS magic");
    assert(sb.block_size == BLOCK_SIZE, "fs_init: unexpected block size");

    file_init();
    device_init();
    printf("fs_init: end\n");
}
