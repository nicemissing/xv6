// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

// 初始化自旋锁
// 输入：lk - 指向要初始化的自旋锁的指针
//       name - 锁的名称（用于调试）
// 输出：将锁的状态初始化为未锁定
void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
// 获取锁（自旋等待直到获得锁）
// 输入：lk - 要获取的自旋锁指针
// 输出：获得锁后返回，执行期间禁用中断
void
acquire(struct spinlock *lk)
{
  // 禁用中断以避免死锁，中断处理函数也有可能获得这个锁
  push_off(); // disable interrupts to avoid deadlock.
  if(holding(lk)) // 检查当前CPU是否已经持有此锁（防止重复获取），保证一个CPU不同时获取
    panic("acquire");

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  // 如果使用普通的软件代码无法做到，参见https://vanjker.github.io/HITsz-OS-labs-2022/site/lab3/part4/
  // __sync_lock_test_and_set特点是无法被其他CPU打断，只有一个CPU在执行，作用是将数据写入内存，然后返回该内存之前的值
  // 因此，这个地方就是先获取锁，然后将1装填进去，然后返回锁之前的值，如果之前没有上锁，则返回0，代码继续执行，如果之前已经上锁，则返回1，卡在循环中，等待锁被释放
  // 保证多个CPU只有一个能获取到锁
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  // 创建内存屏障，确保屏障前后的内存读写绝对不许乱序，确保加锁的内存先完成
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
// 释放锁
// 输入：lk - 要释放的自旋锁指针
// 输出：释放锁，可能恢复中断状态
void
release(struct spinlock *lk)
{
  // 检查当前CPU是否持有此锁
  if(!holding(lk))
    panic("release");

  lk->cpu = 0; // 清除锁的持有者信息

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  // 先确保临界区的临界区里所有写操作在锁被释放之前，对所有 CPU 都可见
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  // 原子地把 0 写进锁变量，本身就是释放操作
  __sync_lock_release(&lk->locked);

  pop_off(); // 可能恢复中断状态
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
// 检查当前CPU是否持有锁
// 必须在中断禁用的情况下调用
// 输入：lk - 要检查的自旋锁指针
// 输出：1表示当前CPU持有锁，0表示不持有
int
holding(struct spinlock *lk)
{
  int r;
  // 锁被锁定并且持有者是当前CPU
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.
// 禁用中断并记录禁用深度
// 输入：无
// 输出：禁用中断，增加当前CPU的noff计数
void
push_off(void)
{
  int old = intr_get(); // 获取当前中断状态（开启或关闭）

  // disable interrupts to prevent an involuntary context
  // switch while using mycpu().
  intr_off();// 禁用中断以防止在使用mycpu()时发生非自愿的上下文切换
  // 如果这是第一次push_off（嵌套深度为0）
  if(mycpu()->noff == 0)
    mycpu()->intena = old; // 保存原始中断使能状态
  mycpu()->noff += 1; // 增加嵌套深度计数器
}
// 恢复中断状态（可能重新启用中断）
// 输入：无
// 输出：减少中断禁用深度，如果深度为0且之前中断是开启的，则重新启用中断
void
pop_off(void)
{
  struct cpu *c = mycpu();
  if(intr_get()) // 检查中断是否被启用（不应该在pop_off时启用）
    panic("pop_off - interruptible");
  if(c->noff < 1) // 检查嵌套深度是否有效
    panic("pop_off");
  c->noff -= 1; // 减少嵌套深度
  // 如果嵌套深度为0且之前中断是开启的，则重新启用中断
  if(c->noff == 0 && c->intena)
    intr_on();
}
