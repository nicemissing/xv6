// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

// （物理内存分配器，用于用户进程、内核栈、页表页和管道缓冲区。
// 分配整个4096字节的页面。）

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
                   // 在kernel.ld中定义，内核链接之后的地址，kalloc从这里开始分配

// ★ 空闲页链表节点结构 ★
struct run {
  struct run *next; // 指向下一个空闲页
};

// ★ 全局空闲内存管理结构 ★
struct {
  struct spinlock lock; // 保护空闲链表的锁
  struct run *freelist; // 空闲页链表头指针
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

// ★ 释放一个连续范围的物理页到空闲链表 ★
// 参数：pa_start - 起始物理地址，pa_end - 结束物理地址
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  // 将起始地址向上对齐到页边界（4096字节对齐）
  p = (char*)PGROUNDUP((uint64)pa_start);
  // 循环释放每一页
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// ★ 释放一个物理页 ★
// 参数：pa - 要释放的物理页的起始地址
// 注意：pa必须是由kalloc分配的，除了初始化时
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // ★ 将页面加入空闲链表（需加锁） ★
  acquire(&kmem.lock);
  // 将当前的空闲链表头设置为当前节点，然后将当前节点设置为原链表头，这样原来的链表头前面就加入了一个新的释放节点
  r->next = kmem.freelist;// 新节点指向原链表头
  kmem.freelist = r;// 更新链表头为新节点
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// 最大到PHYSTOP
// ★ 分配一个物理页 ★
// 返回值：内核可以使用的指针（物理地址，也是内核虚拟地址）
//         如果内存不足，返回0
// 注意：分配的范围从end到PHYSTOP
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist; // freelist是一个链表
  if(r)
    kmem.freelist = r->next; // 相当于freelist向后移动一位
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
