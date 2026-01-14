// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

// 文件系统实现。分为五层：
//   + 块层：原始磁盘块的分配器。
//   + 日志层：多步更新的崩溃恢复。
//   + 文件层：inode分配器，读写和元数据。
//   + 目录层：特殊内容的inode（其他inode的列表！）
//   + 名称层：像 /usr/rtm/xv6/fs.c 这样的路径，方便命名。
//
// 这个文件包含底层的文件系统操作例程。
// （更高层的）系统调用实现在 sysfile.c 中。

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
// 每个磁盘设备应该有一个超级块，但我们只运行一个设备
struct superblock sb; // 全局超级块变量，存储文件系统元数据

// Read the super block.
// 读取超级块。
// 输入：设备号dev，超级块指针sb（用于存储读取的数据）
// 输出：无，但填充*sb
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp; // 缓冲区指针

  bp = bread(dev, 1); // 读取设备dev的第1块（超级块固定在第1块）
  memmove(sb, bp->data, sizeof(*sb)); // 将缓冲区数据复制到超级块结构
  brelse(bp); // 释放缓冲区
}

// Init fs
// 初始化文件系统。
// 输入：设备号dev
// 输出：无，但初始化全局超级块和日志，并回收孤儿inode
void
fsinit(int dev) { 
  readsb(dev, &sb); // 读取超级块
  if(sb.magic != FSMAGIC) // 检查魔数，验证文件系统有效性
    panic("invalid file system");
  initlog(dev, &sb); // 初始化日志系统
  ireclaim(dev); // 回收孤儿inode（链接数为0但未删除的inode）
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno); // 读取块
  memset(bp->data, 0, BSIZE); // 清零缓冲区数据
  log_write(bp); // 记录写操作到日志（延迟写入）
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// returns 0 if out of disk space.

// 块分配。
// 分配一个清零的磁盘块。
// 输入：设备号dev
// 输出：分配的块号，如果磁盘空间不足则返回0
/*
    b：当前位图块管理的第一个数据块的块号

    bi：在位图块中的位索引（0-8191）

    BPB：Bits Per Block = 1024字节 × 8位/字节 = 8192位

    sb.size：文件系统中数据块的总数

    bp->data：位图块的数据缓冲区（1024字节数组）
*/
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  // 遍历所有块，每次处理一个位图块，一个位图块对应8192个数据块
  for(b = 0; b < sb.size; b += BPB){
    // 读取位图块
    bp = bread(dev, BBLOCK(b, sb));// 前8192个数据块对应第一个位图块，以此类推
    // 在位图块中查找空闲位
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8); // 一个位图块的1024字节，看看当前位是否为0
      if((bp->data[bi/8] & m) == 0){  // Is block free? // 检查位图中当前位是否为0，也就是对应的数据块是否空闲
        bp->data[bi/8] |= m;  // Mark block in use. // 标记数据块为已经使用
        log_write(bp); // 记录位图修改到日志，后续会将其提交到磁盘
        brelse(bp); // 释放位图缓冲区
        bzero(dev, b + bi); // 清零新分配的块
        return b + bi; // 返回块号
      }
    }
    brelse(bp); // 释放位图缓冲区
  }
  printf("balloc: out of blocks\n");
  return 0; // 磁盘空间不足
}

// ===============================================================================
// 函数：bfree
// 作用：释放一个磁盘块（将位图中对应位清零）。
// 输入：dev (设备号), b (要释放的块号)
// 输出：无
// ===============================================================================
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb)); // 读取包含块 b 信息的位图块
  bi = b % BPB; // 计算 b 在该位图块内的位偏移，就是b在对应的位图块上的位偏移
  m = 1 << (bi % 8); // 一个位图块有1024个char类型的数据，计算在对应的char类型的数据上的偏移，并将该位置为1
  if((bp->data[bi/8] & m) == 0) // 检查该位是否已经是 0（如果是，说明在释放一个空闲块，这是错误的）
    panic("freeing free block");
  bp->data[bi/8] &= ~m; // 将位图块中的对应位清零
  log_write(bp); // 记录修改到日志
  brelse(bp); // 释放缓冲区
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

// Inode 表结构，包含一个自旋锁和 inode 数组，这个是内存中的inode备份，谁先读到谁占位置
struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

// ===============================================================================
// 函数：iinit
// 作用：初始化 inode 表的锁。
// 输入：无
// 输出：无
// ===============================================================================
void
iinit()
{
  int i = 0;
  
  initlock(&itable.lock, "itable");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&itable.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum); // 前向声明

// ===============================================================================
// 函数：ialloc
// 作用：在磁盘上分配一个新的 inode。
// 输入：dev (设备号), type (文件类型，如 T_FILE, T_DIR)
// 输出：成功返回内存中的 inode 指针，失败返回 0
// ===============================================================================
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;
  // 遍历所有 inode 编号（从 1 开始）
  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb)); // 读取包含该 inode 的磁盘块
    // (struct dinode*)bp->data 是获取包含该inode的buf中的数组首地址，然后加上偏移就是该inode在块内的地址
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // type 为 0 表示该 inode 空闲
      memset(dip, 0, sizeof(*dip));// 清空 inode 的内容
      dip->type = type; // 设置文件类型，这同时也标记了该 inode 已被分配
      log_write(bp); // 记录修改到日志
      brelse(bp); // 释放缓冲区
      return iget(dev, inum); // 返回对应的内存 inode（iget 会加载它）
    }
    brelse(bp); // 当前 inode 被占用，释放缓冲区，继续寻找
  }
  printf("ialloc: no inodes\n"); // inode 耗尽
  return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
// ===============================================================================
// 函数：iupdate
// 作用：将内存 inode 的元数据（大小、链接数等）同步写入到磁盘。
// 输入：ip (内存 inode 指针)
// 输出：无
// 注意：调用者必须持有 ip->lock
// ===============================================================================
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb)); // 读取 inode 所在的磁盘块
  dip = (struct dinode*)bp->data + ip->inum%IPB; // 获取该 inode 在该磁盘块中的地址
  // 将内存 inode 的字段复制到磁盘 inode 结构中
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs)); // 复制数据块地址列表
  log_write(bp); // 写入日志
  brelse(bp); // 释放缓冲区
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
// ===============================================================================
// 函数：iget
// 作用：获取指定 inode 的内存副本（如果已在缓存中则增加引用，否则加载）。
// 输入：dev (设备号), inum (inode 编号)
// 输出：返回内存 inode 指针
// ===============================================================================
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&itable.lock); // 获取 inode 表全局锁

  // Is the inode already in the table?
  // 1. 检查 inode 是否已经在内存缓存中
  empty = 0;
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){ // 找到了匹配项
      ip->ref++; // 增加引用计数
      release(&itable.lock); // 释放全局锁
      return ip; // 返回找到的 inode
    }
    if(empty == 0 && ip->ref == 0) // 记录遇到的第一个空闲槽位
      empty = ip;
  }

  // Recycle an inode entry.
  // 2. 如果不在缓存中，分配一个新的槽位
  if(empty == 0)
    panic("iget: no inodes"); // 缓存表满

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1; // 设置引用计数为 1
  ip->valid = 0; // 标记数据无效（需要通过 ilock 从磁盘读取）
  release(&itable.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
// ===============================================================================
// 函数：idup
// 作用：复制 inode 指针（实际上只是增加引用计数）。
// 输入：ip (inode 指针)
// 输出：返回传入的 ip
// ===============================================================================
struct inode*
idup(struct inode *ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
// ===============================================================================
// 函数：ilock
// 作用：锁定 inode。如果 inode 数据尚未从磁盘读入，则读取它。
// 输入：ip (inode 指针)
// 输出：无
// ===============================================================================
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock); // 获取该 inode 的独占睡眠锁
  // 如果内存中的数据无效（刚通过 iget 获取，还没读盘），则从磁盘读取
  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb)); // 读取磁盘块
    dip = (struct dinode*)bp->data + ip->inum%IPB; // 定位
    // 复制数据到内存 inode
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp); // 释放缓冲区
    ip->valid = 1; // 标记数据有效
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
// ===============================================================================
// 函数：iunlock
// 作用：解锁 inode。
// 输入：ip (inode 指针)
// 输出：无
// ===============================================================================
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock); // 释放睡眠锁
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
// ===============================================================================
// 函数：iput
// 作用：释放 inode 的引用。如果是最后一个引用且链接数为 0，则真正删除文件。
// 输入：ip (inode 指针)
// 输出：无
// ===============================================================================
void
iput(struct inode *ip)
{
  acquire(&itable.lock); // 获取全局锁以检查 ref

  // 如果引用计数为 1（只有当前调用者持有），且数据有效，且磁盘链接数为 0
  // 这意味着文件被删除了，且没有进程打开它
  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock); // 获取睡眠锁，这不会阻塞，因为 ref=1 意味着没别人在用

    release(&itable.lock); // 获取了 inode 锁后，可以暂时释放全局锁

    itrunc(ip); // 截断文件，释放所有数据块
    ip->type = 0; // 标记 inode 类型为 0（空闲）
    iupdate(ip); // 将状态写回磁盘
    ip->valid = 0; // 内存数据失效

    releasesleep(&ip->lock); // 释放 inode 锁

    acquire(&itable.lock); // 重新获取全局锁以减少 ref
  }

  ip->ref--; // 减少引用计数
  release(&itable.lock); // 释放全局锁
}

// Common idiom: unlock, then put.
// ===============================================================================
// 函数：iunlockput
// 作用：常用的组合操作：先解锁，再释放引用。
// 输入：ip (inode 指针)
// ===============================================================================
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// ===============================================================================
// 函数：ireclaim
// 作用：启动时回收孤儿 inode。孤儿是指：已分配(type!=0)但链接数(nlink)为0的 inode。
//       这种情况通常发生在崩溃前文件被删除但尚未完成清理时。
// 输入：dev (设备号)
// iget是获取指定dev和inum的inode区在内存上的备份，在内存中找到一个位置，先不填充内容
// ilock是读取指定dev和inum的inode区的内容，将iget的内存备份填充数据，如果不读取就不知道磁盘上的inode区域对应的addr的数据块的内容，就很容易把inode区域释放但是对应的数据块区还没有释放
// iput是释放引用，将inode的状态写入磁盘，并且把数据块清零
// ===============================================================================
void
ireclaim(int dev)
{
  for (int inum = 1; inum < sb.ninodes; inum++) {
    struct inode *ip = 0;
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode *)bp->data + inum % IPB;
    // 检查是否是孤儿 inode
    if (dip->type != 0 && dip->nlink == 0) {  // is an orphaned inode
      printf("ireclaim: orphaned inode %d\n", inum);
      ip = iget(dev, inum); // 获取内存 inode，就是获取对应的dev和inum的inode在内存上的备份的，操作内存上的数据
    }
    brelse(bp);
    if (ip) {
      // 触发清理流程
      begin_op(); // 开始事务
      ilock(ip); // 锁定并读取（使得 valid=1），这个函数是把对应的dev和inum的磁盘上的inode读入内存的备份
      iunlock(ip); // 解锁
      iput(ip); // 释放引用。由于 nlink=0，iput 会调用 itrunc 并清理磁盘 inode。这个函数是释放引用，当引用计数为0，则触发清理流程，把内存的inode清零然后通过iupdate写入磁盘inode区域
      end_op(); // 结束事务
    }
  }
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// returns 0 if out of disk space.
// ===============================================================================
// 函数：bmap
// 作用：映射 inode 的逻辑块号 (bn) 到磁盘物理块号。如果对应块不存在，则分配它。
// 输入：ip (inode 指针), bn (文件的第几个块，从 0 开始)
// 输出：磁盘物理块号 (address)，如果分配失败返回 0
// ip->addrs是逻辑地址，前12个区域存放直接数据块块号，第13个区域存放间接块的地址，这个地址指向一个磁盘块，存放256个块号，用来大文件寻址
// ===============================================================================
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  // 1. 处理直接块 (Direct Blocks)
  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){ // 如果该位置还没分配块
      addr = balloc(ip->dev); // 分配新块
      if(addr == 0)
        return 0; // 分配失败
      ip->addrs[bn] = addr; // 记录块号到 inode
    }
    return addr;
  }
  bn -= NDIRECT; // 减去直接块的数量，计算在间接块中的偏移

  // 2. 处理间接块 (Indirect Blocks)
  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0){ // 检查间接索引块是否存在
      addr = balloc(ip->dev); // 分配间接索引块
      if(addr == 0)
        return 0;
      ip->addrs[NDIRECT] = addr; // 记录间接索引块地址
    }
    bp = bread(ip->dev, addr); // 读取间接索引块
    a = (uint*)bp->data; // 获取块内容（这是一个块号数组）
    if((addr = a[bn]) == 0){ // 如果间接块中对应的条目为空
      addr = balloc(ip->dev); // 分配实际数据块
      if(addr){
        a[bn] = addr; // 记录数据块号到间接索引块中
        log_write(bp); // 记录间接块的修改
      }
    }
    brelse(bp); // 释放间接索引块缓冲区
    return addr; // 返回实际的物理块号
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
// ===============================================================================
// 函数：itrunc
// 作用：截断 inode（丢弃文件所有内容）。释放所有关联的数据块。
// 输入：ip (inode 指针)
// 输出：无
// ===============================================================================
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  // 1. 释放所有直接块
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]); // 释放块
      ip->addrs[i] = 0; // 指针清零
    }
  }

  // 2. 释放间接块及其指向的数据块
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]); // 读取间接索引块
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]); // 释放索引块指向的每一个数据块
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]); // 释放间接索引块本身
    ip->addrs[NDIRECT] = 0; // 指针清零
  }

  ip->size = 0; // 文件大小置 0
  iupdate(ip); // 更新 inode 到磁盘
}

// Copy stat information from inode.
// Caller must hold ip->lock.
// ===============================================================================
// 函数：stati
// 作用：从 inode 复制信息到 stat 结构体（用于 fstat 系统调用）。
// 输入：ip (inode 指针), st (输出的 stat 结构体)
// ===============================================================================
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
// ===============================================================================
// 函数：readi
// 作用：从 inode 读取数据。
// 输入：ip (inode), user_dst (目标是否为用户空间), dst (目标地址), off (文件偏移), n (读取字节数)
// 输出：实际读取的字节数
// ===============================================================================
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off) // 检查读取范围是否越界
    return 0;
  if(off + n > ip->size) // 如果读取超过文件末尾，截断读取长度
    n = ip->size - off;

  // 循环读取数据，tot 是已读取字节数
  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    uint addr = bmap(ip, off/BSIZE); // 获取当前偏移对应的物理块号
    if(addr == 0) // 空洞文件（未分配块），通常返回 0
      break;
    bp = bread(ip->dev, addr); // 读取块
    m = min(n - tot, BSIZE - off%BSIZE); // 计算本次复制的字节数：取 (剩余需求 n-tot) 和 (当前块剩余空间) 的较小值
    // 将数据从缓冲区复制到目标地址 (dst)
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1; // 复制失败（如用户地址非法）
      break;
    }
    brelse(bp); // 释放缓冲区
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
// ===============================================================================
// 函数：writei
// 作用：向指定inode文件的off偏移开始处写入 n 个字节。
// 输入：ip (inode), user_src (源是否为用户空间), src (源地址), off (偏移), n (字节数)
// 输出：实际写入的字节数，失败返回 -1
// 循环写入是因为物理块不连续，每次最多写入一个块
// ===============================================================================
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off) // 检查偏移量是否合法，以及是否超过最大文件大小
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  // 循环写入数据
  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    uint addr = bmap(ip, off/BSIZE); // 获取物理块号（若不存在 bmap 会分配）
    if(addr == 0)
      break; // 分配失败（磁盘满）
    bp = bread(ip->dev, addr); // 读取该块（为了写入部分内容，必须先读整个块）
    m = min(n - tot, BSIZE - off%BSIZE); // 计算本次写入长度
    // 将数据从源地址 (src) 复制到缓冲区
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp); // 记录修改到日志
    brelse(bp); // 释放缓冲区
  }

  if(off > ip->size) // 如果写入使得文件变大，更新文件大小
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip); // 即使大小没变，也需要 iupdate，因为 bmap 可能分配了新块导致 ip->addrs 改变

  return tot;
}

// Directories 目录操作

// ===============================================================================
// 函数：namecmp
// 作用：比较两个文件名（限制长度 DIRSIZ）。
// ===============================================================================
int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
// ===============================================================================
// 函数：dirlookup，目录inode的addr中存放的数据是struct dirent类型的数据
// 作用：在目录 inode dp 中查找名为 name 的条目。
// 输入：dp (目录 inode), name (文件名), poff (输出参数，若不为0，存储找到条目的偏移量)
// 输出：找到的文件的 inode 指针，未找到返回 0
// 在一个指定的目录（dp）中，查找名为 name 的文件。如果找到了，返回该文件的 Inode 指针；如果没找到，返回 0。
// ===============================================================================
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de; // 目录项结构

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  // 遍历目录内容，每次读取一个 dirent 大小
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) // 每次把off加sizeof(de)的数据读取到de中
      panic("dirlookup read");
    if(de.inum == 0) // inum 为 0 表示该目录项是空的
      continue;
    if(namecmp(name, de.name) == 0){ // 名字匹配
      // entry matches path element
      if(poff)
        *poff = off; // 如果调用者需要，记录偏移量
      inum = de.inum; 
      return iget(dp->dev, inum); // 获取该文件的 inode 并返回
    }
  }

  return 0; // 未找到
}

// Write a new directory entry (name, inum) into the directory dp.
// Returns 0 on success, -1 on failure (e.g. out of disk blocks).
// ===============================================================================
// 函数：dirlink
// 作用：在目录 dp 中添加一个新的目录项 (name, inum)。
// 输入：dp (目录 inode), name (文件名), inum (inode 编号)
// 输出：成功 0，失败 -1
// ===============================================================================
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){ // 检查名字是否已存在
    iput(ip); // 如果找到了，说明重名了，释放引用并报错
    return -1;
  }

  // Look for an empty dirent.
  // 寻找空的目录项槽位
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0) // 找到空位
      break;
  }
  // 准备新的目录项数据
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  // 写入目录项到目录文件中
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;

  return 0;
}

// 如何从path到inode呢？
// 主要就是因为inode区域的目录文件的数据存放的是struct dirent类型数据，其中name字段存放的是文件名，inum字段存放的是inode编号。
// 这里的name字段就是各层目录的名字或者目录下面文件的名字，而inum字段存放的是该文件或者目录文件对应的inode编号，这就可以将path和inode关联起来。

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
// ===============================================================================
// 函数：skipelem
// 作用：解析路径中的下一个元素。
// 输入：path (当前路径字符串), name (输出缓冲区，存放解析出的文件名)
// 输出：指向路径中下一个元素的指针
// 示例：skipelem("a/bb/c", name) -> name="a", 返回 "bb/c"
// ===============================================================================
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/') // 跳过前导斜杠
    path++;
  if(*path == 0) // 路径结束
    return 0;
  s = path; // 记录元素开始位置
  while(*path != '/' && *path != 0) // 找到下一个斜杠或结尾
    path++;
  len = path - s; // 计算当前元素长度
  // 复制名字到 name 缓冲区
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/') // 跳过后续的斜杠
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
// ===============================================================================
// 函数：namex
// 作用：路径名解析的核心逻辑。
// 输入：path (路径), nameiparent (是否只解析到父目录), name (若nameiparent为真，存最后的文件名)
// 输出：目标 inode，失败返回 0
// ===============================================================================
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;
  // 决定起始目录：绝对路径从根目录开始，相对路径从当前目录开始
  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO); 
  else
    ip = idup(myproc()->cwd);

  // 逻辑就是逐渐拆解，我把第一层目录的name存起来，然后通过dirlookup找到下一个节点，一层层向下找
  while((path = skipelem(path, name)) != 0){
    ilock(ip); // 锁定当前目录 inode 以读取内容
    if(ip->type != T_DIR){ // 如果中间某个节点不是目录，则路径非法
      iunlockput(ip);
      return 0;
    }
    // 如果 nameiparent 为真，且 path 为空（已到最后一个元素）
    // 说明我们需要返回的是父目录（当前 ip），而不是最后一层的 inode
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip); // 提前停止
      return ip;
    }
    // 在当前目录查找下一级，name在skipelem返回为当前的目录
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip); // 没找到
      return 0;
    }
    iunlockput(ip); // 释放当前目录锁和引用
    ip = next; // 移动到下一级
  }
  // 如果要求解析父目录，但循环正常结束了（说明解析到了文件本身而不是父目录），这通常不应发生
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

// ===============================================================================
// 函数：namei
// 作用：解析路径，返回对应的 inode。
// 输入：path (路径字符串)
// 输出：inode 指针
// ===============================================================================
struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

// ===============================================================================
// 函数：nameiparent
// 作用：解析路径，返回其父目录的 inode，并将最后一个元素名复制到 name。
//       例如 path="/a/b/c"，返回 "/a/b" 的 inode，name 为 "c"。
// 输入：path (路径), name (输出缓冲)
// 输出：父目录 inode 指针
// ===============================================================================
struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
