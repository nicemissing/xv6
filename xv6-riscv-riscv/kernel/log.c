/*
  日志层的主要作用是在于文件系统崩溃后的恢复，日志层
  可以使得系统调用的结果要么完全出现，要么完全不出现。
  同时，日志层还具有快速恢复的优点。
  具体理解：
  想象一次 write() 系统调用，它可能要修改多个磁盘块：

    1、数据块（写入文件内容）
    2、inode（更新文件大小、修改时间）
    3、块位图（标记数据块已分配）
    4、inode 位图（如果文件是新创建的）

  如果崩溃发生在第2步和第3步之间，磁盘就处于半写状态：
  inode 记录了文件变大，但数据块没写入或位图没标记，文件系统就损坏了。

  因此，日志层就把对磁盘的所有修改，打包成一个事务（transaction），
  这样就不会有一部分操作完成，一部分操作没有完成。
  要么崩溃没有动文件系统，要么崩溃所有的已经写完了，而且重启后看到“提交记录”，重放日志 → 把日志里的块再写一遍 → 完整应用所有修改

  日志区一般是位于块2-31

  通常情况下：一个系统调用 ≈ 一个事务

  日志层与buffercache打交道不直接接触磁盘，通过buffercache与磁盘交互
*/
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
// 头块的内容，既用于磁盘上的头块，也用于在内存中跟踪提交前的已记录块号。
struct logheader {
  int n; // 本次事务包含的块数，从日志区要提交到数据块区的块数
  int block[LOGBLOCKS]; // 这些块在文件系统中的目标块号，在数据块区的目标块号
};

// 日志结构体，在内存中维护日志状态
struct log {
  struct spinlock lock; // 保护日志结构的自旋锁
  int start; // 日志区域在磁盘上的起始块号
  // 正在执行的文件系统系统调用数量，例如write
  int outstanding; // how many FS sys calls are executing.
  // 是否正在提交（如果为1，则其他操作需等待）
  int committing;  // in commit(), please wait.
  // 日志所在的设备号
  int dev;
  // 内存中的日志头
  struct logheader lh;
};
struct log log; // 全局日志实例

static void recover_from_log(void);
static void commit();

// 初始化日志系统
// 输入：dev - 设备号，sb - 指向超级块的指针（包含日志起始块号）
// 输出：无
void
initlog(int dev, struct superblock *sb)
{
  // 确保日志头大小不超过一个块的大小
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");
  // 初始化日志锁
  initlock(&log.lock, "log");
  // 设置日志起始块（从超级块中获取）
  log.start = sb->logstart;
  log.dev = dev;
  // 从日志中恢复（如果上次崩溃时有未提交的事务，则进行恢复）
  recover_from_log();
}

// Copy committed blocks from log to their home location
// 将已提交的块从日志复制到它们的原始位置（家位置）
// 输入：recovering - 是否处于恢复模式（1表示恢复，0表示正常提交）
// 输出：无
static void
install_trans(int recovering)
{
  int tail;
  // 遍历日志中的所有块
  for (tail = 0; tail < log.lh.n; tail++) {
    // 如果是恢复模式，打印恢复信息
    if(recovering) {
      printf("recovering tail %d dst %d\n", tail, log.lh.block[tail]);
    }
    // 读取日志块（日志区域从start+1开始，因为start是头块）
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block
    // 读取目标块（原始位置）
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    // 将数据从日志块复制到目标块
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    // 将目标块写回磁盘（确保数据持久化）
    bwrite(dbuf);  // write dst to disk
    // 如果不是恢复模式，则取消固定目标块（减少引用计数）
    if(recovering == 0)
      bunpin(dbuf);
    // 释放缓冲区
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
// 从磁盘读取日志头到内存中的日志头
// 输入：无
// 输出：无（但会更新内存中的log.lh）
static void
read_head(void)
{
  // 读取日志头块（位于log.start）
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  // 复制块数
  log.lh.n = lh->n;
  // 把lh的内容复制到log.lh的内存中
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
// 将内存中的日志头写入磁盘，写入磁盘的顺序应该先根据设备号和块号得到buffercache中的指针，然后再将该指针对应的内容装填写入磁盘
// 这是当前事务真正提交的时刻。
// 输入：无
// 输出：无
static void
write_head(void)
{
  // 读取日志头块（准备写入）
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  // 将内存中的日志头信息写入磁盘缓冲区
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  // 将头块写入磁盘（这是提交点）
  bwrite(buf);
  brelse(buf);
}
// 从日志中恢复（在初始化时调用）
// 输入：无
// 输出：无
static void
recover_from_log(void)
{
  // 读取磁盘上的日志头
  read_head();
  // 安装事务（如果已提交，则从日志复制到磁盘），把日志区的旧的数据写入磁盘数据区
  install_trans(1); // if committed, copy from log to disk
  // 清除内存中的日志头（事务已处理）
  log.lh.n = 0;
  // 将空日志头写回磁盘，清除日志，这里维护一个指针n，表示没有有用的日志数据
  write_head(); // clear the log
}

// called at the start of each FS system call.
// 在每个文件系统系统调用开始时调用。
// 输入：无
// 输出：无（但可能休眠直到可以安全开始操作）
void
begin_op(void)
{
  // 获取日志锁
  acquire(&log.lock);
  // 循环直到可以开始操作
  while(1){
    // 如果日志正在提交，则等待
    if(log.committing){
      sleep(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGBLOCKS){
      // 这个操作可能会耗尽日志空间；等待提交。
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {
      // 增加正在进行的系统调用计数，每有一次fs系统调用增加1
      log.outstanding += 1;
      release(&log.lock);
      break; // 可以开始操作
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
// 在每个文件系统系统调用结束时调用。
// 如果这是最后一个未完成的操作，则提交事务。
// 输入：无
// 输出：无
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1; // fs系统调用结束
  // 如果日志正在提交，不应该发生（因为提交时没有未完成的操作）
  if(log.committing)
    panic("log.committing");
  // 如果所有操作都已完成，则开始提交，这是组提交，将多个事务合并为1个一起提交提高吞吐量
  if(log.outstanding == 0){
    do_commit = 1;
    log.committing = 1; // 提交开始，其余系统调用卡在begin_op()
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    // begin_op() 可能正在等待日志空间，
    // 减少log.outstanding会减少保留的空间。
    // 唤醒可能等待的进程
    wakeup(&log);
  }
  release(&log.lock);
  // 如果需要提交，则调用commit（注意：不持有锁，因为commit可能会睡眠）
  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    // 调用commit时不持有锁，因为不允许在持有锁时睡眠。
    commit();
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log); // 唤醒可能等待提交完成的进程
    release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
// 将修改的块从缓存复制到日志区域。
// 输入：无
// 输出：无
static void
write_log(void)
{
  int tail;
  // 遍历日志中的所有块
  for (tail = 0; tail < log.lh.n; tail++) {
    // 分配日志块（日志区域从start+1开始）
    struct buf *to = bread(log.dev, log.start+tail+1); // log block
    // 读取缓存中的块（数据已修改但未写入磁盘）
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    // 将数据从缓存块复制到日志块
    memmove(to->data, from->data, BSIZE);
    // 将日志块写入磁盘（日志区域）
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    // 将修改的块从缓存写入日志区域
    write_log();     // Write modified blocks from cache to log
    // 将日志头写入磁盘——真正的提交点
    write_head();    // Write header to disk -- the real commit
    // 现在将写入安装到原始位置（非恢复模式）
    install_trans(0); // Now install writes to home locations
    // 清除日志
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
// 本次事务需要修改块 b->blockno，记录以下要修改哪个块的块号
// log_write和write_log配合使用，先记录要修改的块号，等commit的时候一起再写入磁盘
void
log_write(struct buf *b)
{
  int i;

  acquire(&log.lock);
  // 检查日志是否已满
  if (log.lh.n >= LOGBLOCKS)
    panic("too big a transaction");
  // 确保在事务中调用（即begin_op之后）
  if (log.outstanding < 1)
    panic("log_write outside of trans");
  // 检查这个块是否已经在当前事务的日志中（日志吸收）
  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorption
      break;
  }
  // 记录块号（如果已存在，则覆盖同一位置），记录进内存的log的日志头中
  log.lh.block[i] = b->blockno;
  // 如果是新块，则固定缓冲区并增加日志计数
  if (i == log.lh.n) {  // Add new block to log?
    bpin(b); // 增加缓冲区的引用计数，防止被换出
    log.lh.n++; // 事务块数 +1
  }
  release(&log.lock);
}

