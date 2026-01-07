// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

// 缓冲区缓存（Buffer cache）实现
// 
// 缓冲区缓存是一个由buf结构体组成的链表，用于缓存磁盘块的内容。
// 在内存中缓存磁盘块可以减少磁盘读取次数，同时也为多个进程使用的磁盘块提供同步点。
// 
// 接口：
// * 要获取特定磁盘块的缓冲区，调用bread。
// * 修改缓冲区数据后，调用bwrite将其写入磁盘。
// * 使用完缓冲区后，调用brelse。
// * 调用brelse后不要使用缓冲区。
// * 一次只有一个进程可以使用一个缓冲区，因此不要占用超过必要的时间。

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// bcache结构体：管理所有缓冲区缓存的全局数据结构
struct {
  // 保护bcache结构的自旋锁（用于保护整个缓冲区缓存）
  struct spinlock lock; 
  // 缓冲区数组，NBUF为缓冲区数量（定义在param.h中）
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // 所有缓冲区的双向链表，通过prev/next链接。
  // 按缓冲区最近使用时间排序。
  // head.next指向最近使用的缓冲区，head.prev指向最近最少使用的缓冲区。
  struct buf head; // 链表的头节点（哨兵节点，不存储实际数据）
} bcache; // buffercache

// binit函数：初始化缓冲区缓存
// 输入：无
// 输出：无
// 作用：初始化bcache锁和缓冲区链表，将所有缓冲区链接成一个双向循环链表
void
binit(void)
{
  struct buf *b;
  // 初始化bcache的自旋锁
  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  // 创建双向循环链表：初始化头节点，使其指向自己
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  // 遍历所有缓冲区，将它们插入到链表头部（即头节点之后），双向链表的插入
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next; // 1.新节点的next指向原头节点的下一个
    b->prev = &bcache.head; // 2.新节点的prev指向原头节点
    initsleeplock(&b->lock, "buffer"); // 初始化锁
    bcache.head.next->prev = b; // 3.原头节点的下一个（原第一个）节点的prev指向新节点
    bcache.head.next = b; // 4.头节点的next指向新节点
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
// bget函数：根据设备号和块号查找缓冲区，如果不存在则分配一个
// 输入：dev - 设备号，blockno - 块号
// 输出：一个已锁定（睡眠锁）的缓冲区指针
// 作用：在缓冲区缓存中查找指定块，如果找到则增加引用计数并返回；
//       如果没找到，则使用LRU算法找到一个未使用的缓冲区，将其分配给新的块，并返回。
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  // 获取bcache锁，保护整个缓冲区缓存buffercache数据结构
  acquire(&bcache.lock);

  // Is the block already cached?
  // 在缓存中查找是否已经存在指定设备号和块号的缓冲区
  // 遍历双向链表（从最近使用的开始）
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    // 如果找到匹配的缓冲区
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++; // 增加引用计数
      release(&bcache.lock); // 释放bcache锁
      // 获取该缓冲区的睡眠锁（保证互斥访问），因为两个进程不能同时操作同一个块，如果同时写入磁盘会造成错乱
      // 之所以使用睡眠锁，是因为磁盘io操作是耗时的，其余进程如果也等待这buf，没必要自旋，需要睡眠让出cpu节省资源
      acquiresleep(&b->lock); 
      return b; // 返回找到的缓冲区
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  // 没有在缓存中找到，需要分配一个新的缓冲区（实际上是从缓存中回收一个未使用的缓冲区）
  // 从最近最少使用的缓冲区开始查找（即从链表尾部向前查找）
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    // 如果该缓冲区引用计数为0，说明没有被使用，可以回收
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0; // 标记缓冲区数据无效（因为还没有从磁盘读取）
      b->refcnt = 1; // 引用计数设为1
      release(&bcache.lock); // 释放bcache锁
      acquiresleep(&b->lock); // 获取该缓冲区的睡眠锁
      return b; // 返回分配的缓冲区
    }
  }
  // 如果没有找到引用计数为0的缓冲区，说明所有缓冲区都在使用中，系统出现错误
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
// bread函数：读取指定设备号和块号的磁盘块，返回一个包含数据的缓冲区
// 输入：dev - 设备号，blockno - 块号
// 输出：一个已锁定（睡眠锁）且包含有效数据的缓冲区指针
// 作用：调用bget获取缓冲区，如果缓冲区数据无效（未缓存），则从磁盘读取数据
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;
  // 获取缓冲区（可能从缓存中获取，也可能分配新的）
  b = bget(dev, blockno);
  if(!b->valid) {// 如果缓冲区数据无效，则从磁盘读取
    virtio_disk_rw(b, 0); // 0表示读操作，将磁盘块读入缓冲区
    b->valid = 1; // 标记缓冲区数据有效
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
// bwrite函数：将缓冲区的内容写入磁盘
// 输入：b - 要写入的缓冲区指针（必须已锁定）
// 输出：无
// 作用：将缓冲区数据写入磁盘，调用磁盘驱动程序的写操作
void
bwrite(struct buf *b)
{
  // 确保调用者持有该缓冲区的睡眠锁
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  // 调用磁盘写操作，1表示写操作
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
// brelse函数：释放一个锁定的缓冲区
// 输入：b - 要释放的缓冲区指针（必须已锁定）
// 输出：无
// 作用：释放缓冲区的睡眠锁，减少引用计数；如果引用计数为0，则将缓冲区移动到LRU链表头部（表示最近使用过）
void
brelse(struct buf *b)
{
  // 确保调用者持有该缓冲区的睡眠锁
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock); // 释放缓冲区的睡眠锁
  // 获取bcache锁，因为要修改缓冲区的引用计数和链表结构
  acquire(&bcache.lock);
  b->refcnt--; // 减少缓冲区的引用计数
  // 如果引用计数变为0，说明没有进程在使用这个缓冲区，可以将其移动到LRU链表头部（最近使用）
  if (b->refcnt == 0) {
    // no one is waiting for it.
    // 将缓冲区从当前位置移除
    b->next->prev = b->prev;
    b->prev->next = b->next;
    // 将缓冲区插入到链表头部（head之后）
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  // 释放bcache锁
  release(&bcache.lock);
}
// bpin函数：增加缓冲区的引用计数（固定缓冲区，防止被回收）
// 输入：b - 缓冲区指针
// 输出：无
// 作用：增加缓冲区的引用计数，通常在需要长期持有缓冲区时调用
void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}
// bunpin函数：减少缓冲区的引用计数（解除固定）
// 输入：b - 缓冲区指针
// 输出：无
// 作用：减少缓冲区的引用计数，与bpin配对使用
void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


