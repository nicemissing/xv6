// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

/*
  睡眠锁的locked和pid字段需要原子访问：
  防止多个CPU同时修改锁状态
  确保acquiresleep()中的检查和设置是原子的
  确保holdingsleep()中的检查是原子的
  内部自旋锁只在操作睡眠锁状态时短暂持有，不会导致长时间忙等待。
*/

void
initsleeplock(struct sleeplock *lk, char *name)
{
  initlock(&lk->lk, "sleep lock");// 初始化内部保护自旋锁
  lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
}
// 获取睡眠锁（如果锁已被占用，则让当前进程睡眠等待）
// 输入：lk - 要获取的睡眠锁指针
// 输出：获得锁后返回，可能让进程睡眠等待
void
acquiresleep(struct sleeplock *lk)
{
  // 获取内部保护自旋锁，确保对locked和pid的访问是原子的
  acquire(&lk->lk);
  // 如果锁已被占用，让当前进程在睡眠锁上睡眠等待
  while (lk->locked) {
    sleep(lk, &lk->lk); // sleep让出CPU，并等待信号
                        // 同时释放内部自旋锁（lk.lk），允许其他进程操作
  }
  lk->locked = 1; // 标记锁为已持有
  lk->pid = myproc()->pid; // 记录持有锁的进程ID
  release(&lk->lk); // 释放内部保护自旋锁
}

// 释放睡眠锁，并唤醒等待此锁的所有进程
// 输入：lk - 要释放的睡眠锁指针
// 输出：释放锁，唤醒等待的进程
void
releasesleep(struct sleeplock *lk)
{
  acquire(&lk->lk); // 获取内部保护自旋锁
  lk->locked = 0; // 标记锁为未持有
  lk->pid = 0; // 清除持有锁的进程ID
  wakeup(lk); // 唤醒所有在这个睡眠锁上等待的进程
  release(&lk->lk); // 释放内部保护自旋锁
}
// 检查当前进程是否持有睡眠锁
// 输入：lk - 要检查的睡眠锁指针
// 输出：1表示当前进程持有锁，0表示不持有
int
holdingsleep(struct sleeplock *lk)
{
  int r;
  
  acquire(&lk->lk);// 获取内部保护自旋锁，确保读取locked和pid是原子的
  // 检查锁是否被锁定并且持有者是当前进程
  r = lk->locked && (lk->pid == myproc()->pid);
  release(&lk->lk);// 释放内部保护自旋锁
  return r;
}



