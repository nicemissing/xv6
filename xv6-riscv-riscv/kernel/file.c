//
// Support functions for system calls that involve file descriptors.
//
//
// Support functions for system calls that involve file descriptors.
// 支持涉及文件描述符的系统调用的辅助函数。
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

// 全局设备开关表。
// 映射主设备号 (major device number) 到具体的设备读写函数。
struct devsw devsw[NDEV];

// 全局打开文件表 (Global Open File Table)
// 系统中所有进程打开的文件都记录在这里。
struct {
  struct spinlock lock; // 保护 file 数组的自旋锁
  struct file file[NFILE]; // 全局文件结构体数组，NFILE 是最大同时打开文件数
} ftable;

// ===============================================================================
// 函数：fileinit
// 作用：初始化文件表锁。在系统启动时（main.c）被调用一次。
// 输入：无
// 输出：无
// ===============================================================================
void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
// ===============================================================================
// 函数：filealloc
// 作用：分配一个空闲的文件结构体 (struct file)。
// 输入：无
// 输出：成功返回指向 struct file 的指针，失败（表满）返回 0。
// ===============================================================================
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock); // 获取全局锁，因为要修改 ftable 数组状态
  // 遍历全局文件表，寻找引用计数 (ref) 为 0 的空闲槽位
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){ // 找到空闲位
      f->ref = 1; // 将引用计数初始化为 1，标记为已分配
      release(&ftable.lock); // 释放锁
      return f; // 返回分配的文件指针
    }
  }
  release(&ftable.lock); // 如果循环结束还没找到，释放锁
  return 0; // 返回 0 表示系统打开文件数已达上限
}

// Increment ref count for file f.
// ===============================================================================
// 函数：filedup
// 作用：增加文件结构的引用计数。通常用于 dup() 系统调用或 fork() 时复制文件描述符。
// 输入：f (要复制的文件指针)
// 输出：返回传入的文件指针 f
// ===============================================================================
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock); // 修改引用计数需要持有全局锁
  if(f->ref < 1) 
    panic("filedup");
  f->ref++; // 引用计数 +1
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
// ===============================================================================
// 函数：fileclose
// 作用：关闭文件。减少引用计数，如果计数降为 0，则释放资源。
// 输入：f (要关闭的文件指针)
// 输出：无
// ===============================================================================
void
fileclose(struct file *f)
{
  struct file ff; // 用于临时保存文件信息，以便在释放锁之后处理清理工作

  acquire(&ftable.lock); // 获取全局锁
  if(f->ref < 1)
    panic("fileclose");
  // 减少引用计数。如果还有其他引用（如 dup 产生的），则仅减计数并返回
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  // 代码执行到这里，说明 ref 降为 0，需要真正回收该结构体
  ff = *f;
  f->ref = 0; // 标记全局表中的该槽位为空闲
  f->type = FD_NONE; // 清除类型
  release(&ftable.lock); // 释放全局锁

  // 根据文件类型执行具体的清理操作
  if(ff.type == FD_PIPE){
    // 如果是管道，调用管道关闭函数
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op(); // 开始文件系统事务（因为 iput 可能会写磁盘）
    iput(ff.ip); // 释放 inode 的引用计数
    end_op(); // 结束事务
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
// ===============================================================================
// 函数：filestat
// 作用：获取文件的元数据（如大小、类型、链接数）。对应 fstat 系统调用。
// 输入：f (文件指针), addr (用户空间地址，用于存储 struct stat)
// 输出：成功返回 0，失败返回 -1
// ===============================================================================
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc(); // 获取当前进程
  struct stat st; // 栈上的 stat 结构体，用于临时存储内核态获取的信息
  
  // 只有普通文件(INODE)和设备(DEVICE)支持 stat 操作，管道不支持
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip); // 锁定 inode，以便读取一致的数据
    stati(f->ip, &st); // 调用 fs.c 中的 stati 函数，将 inode 信息填入 st
    iunlock(f->ip); // 解锁 inode
    // 将内核中的 st 结构体复制到用户空间的地址 addr
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
// ===============================================================================
// 函数：fileread
// 作用：从文件 f 中读取 n 字节数据到用户地址 addr。
// 输入：f (文件指针), addr (用户目标地址), n (读取字节数)
// 输出：实际读取的字节数，失败返回 -1
// ===============================================================================
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0) // 存储实际读取的字节数
    return -1;

  if(f->type == FD_PIPE){
    // 情况 1: 管道。调用 piperead
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    // 情况 2: 设备文件（如 console）。
    // 检查主设备号合法性以及该设备是否有读函数
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    // 调用设备特定的读函数。参数 1 表示 user_dst 为真（目标在用户空间）
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    // 情况 3: 普通文件。
    ilock(f->ip);
    // 调用 fs.c 的 readi 读取数据
    // 参数含义：ip, user_dst=1(目标是用户空间), dst=addr, off=f->off(当前偏移), n
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r; // 如果读取成功，推进文件偏移量
    iunlock(f->ip); // 解锁
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
// ===============================================================================
// 函数：filewrite
// 作用：向文件 f 写入 n 字节数据，源数据在用户地址 addr。
// 输入：f (文件指针), addr (用户源地址), n (写入字节数)
// 输出：实际写入的字节数，失败返回 -1
// ===============================================================================
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0) // 检查文件是否以“写”模式打开
    return -1;

  if(f->type == FD_PIPE){
    // 情况 1: 管道。调用 pipewrite
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    // 情况 2: 设备文件。
    // 检查主设备号和写函数是否存在
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.

    // 情况 3: 普通文件。
    
    // 计算单次写操作允许的最大字节数。
    // 为了防止单次写入的数据量过大，导致日志系统（logging system）的事务溢出。
    // 解释 max 的计算：
    // MAXOPBLOCKS = 10 (单次事务允许修改的最大块数)
    // -1: 可能修改 inode 块
    // -1: 可能修改 bitmap 块
    // -2: 即使对齐写入，首尾也可能跨越两个数据块
    // /2: 为了保险起见（可能涉及间接块修改等），再除以 2
    // * BSIZE: 转化为字节数
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0; // i 记录已写入的总字节数
    while(i < n){ // 循环分批写入，直到写完 n 字节
      int n1 = n - i; // 本次循环计划写入的字节数
      if(n1 > max)
        n1 = max; // 如果剩余量超过 max，则限制为 max

      begin_op(); // 开启文件系统事务（因为 writei 会修改磁盘数据）
      ilock(f->ip); // 锁定 inode
      // 调用 fs.c 的 writei 写入数据
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r; // 写入成功，推进偏移量
      iunlock(f->ip); // 解锁 inode
      end_op(); // 结束事务（这会触发日志提交）

      if(r != n1){
        // 如果实际写入字节数不等于请求数（例如磁盘满了），则停止循环
        // error from writei
        break;
      }
      i += r; // 累加已写入字节数
    }
    ret = (i == n ? n : -1); // 如果所有数据都写入成功，返回 n，否则返回 -1 (xv6 的简化处理)
  } else {
    panic("filewrite");
  }

  return ret;
}

