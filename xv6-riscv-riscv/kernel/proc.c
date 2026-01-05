#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
// 全局数据结构定义
struct cpu cpus[NCPU]; // 所有CPU核心的状态数组

struct proc proc[NPROC]; // 所有进程的状态数组（进程表）

struct proc *initproc; // 指向init进程的指针（第一个用户进程）

int nextpid = 1; // 下一个可用的进程ID
struct spinlock pid_lock; // 保护nextpid的锁

extern void forkret(void); // fork返回函数
static void freeproc(struct proc *p); // 释放进程内部资源

extern char trampoline[]; // trampoline.S 跳板函数的地址

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock; // 等待锁：保护进程父子关系和wait()系统调用

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
// 为每个进程的内核栈分配页面，每个进程一个内核栈
// 将其映射到高地址内存，后面跟着一个无效的保护页
// 有多少进程，内核页表就有多少个内核栈
void
proc_mapstacks(pagetable_t kpgtbl) // kpgtbl是内核页表
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    // 计算该进程内核栈的虚拟地址（使用KSTACK宏）
    uint64 va = KSTACK((int) (p - proc));
    // 在内核页表中建立映射，只映射一页
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
// 初始化进程表
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");// 初始状态为未使用
      p->state = UNUSED;
      // 设置内核栈的虚拟地址，物理页还没有分配，用户地址上没有内核栈，这里应该只是为了方便寻找内核页表上对应的本进程的内核栈位置
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
// 获取当前CPU的ID（必须在中断禁用时调用）
int
cpuid()
{
  int id = r_tp(); // 读取tp寄存器，在RISC-V中tp存储hartid（CPU核心ID）
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
// 返回当前CPU的cpu结构（必须在中断禁用时调用）
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
// 返回当前进程的proc结构，如果没有返回0
struct proc*
myproc(void)
{
  push_off(); // 禁用中断，防止进程被迁移
  struct cpu *c = mycpu();
  struct proc *p = c->proc; // 从CPU结构获取当前运行的进程
  pop_off();// 恢复中断状态
  return p;
}

// 分配一个新的进程ID（需要持有pid_lock）
int
allocpid()
{
  int pid;
  
  acquire(&pid_lock); // 不然nextpid会乱套
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
// 在进程表中查找一个UNUSED的进程槽位
// 如果找到，初始化运行在内核所需的状态，并返回持有p->lock的进程
// 如果没有空闲进程或内存分配失败，返回0
static struct proc*
allocproc(void)
{
  struct proc *p;
  // 遍历进程表查找未使用的槽位
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) { // 找到空闲槽位
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();// 分配pid
  p->state = USED; // 修改状态

  // Lab4
  p->alarm_handler = 0;
  p->ticks_since_last_alarm = 0;
  p->alarm_period = 0;
  p->inalarm = 0;

  // Allocate a trapframe page.
  // 分配一个trapframe页面，使用物理地址或者内核虚拟地址
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Lab4
  // Allocate a alarmframe page. 为这个备份alarmframe分配1页物理空间
  if((p->alarmframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  // 建立基本的用户页表和trampoline和trapframe映射
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p); // 创建失败，释放资源
    release(&p->lock);
    return 0;
  }

  // 为这个进程分配并初始化一个新的专属内核页
  p->kpagetable = ukvminit();
  if (p->kpagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  char *pa = kalloc();
  if (pa == 0)
  {
    panic("kalloc");
  }
  uint64 va = KSTACK((int)(p-proc));
  ukvmmap(p->kpagetable, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  p->kstack = va;

  // Set up new context to start executing at forkret,
  // which returns to user space.
  // 设置新的上下文，使其从forkret开始执行
  // forkret返回到用户空间
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret; // ra进入这个函数的地址，ra寄存器存储下一条执行的指令
  p->context.sp = p->kstack + PGSIZE;// 栈指针设为内核栈顶部

  return p; // 返回新分配的进程（调用者持有p->lock）
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
// 释放进程结构及其关联的数据，包括用户页面
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe); // 释放trapframe物理页
  p->trapframe = 0;
  if(p->alarmframe)
    kfree((void*)p->alarmframe); // 释放alarmframe物理页
  p->alarmframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);// 释放用户页表和内存
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  // lab3
  if (p->kstack)
  {
    pte_t* pte = walk(p->kpagetable, p->kstack, 0);
    if(pte==0)
    {
      panic("freeproc");
    }
    kfree((void*)PTE2PA(*pte));
  }
  p->kstack = 0;

  if(p->kpagetable)
  {
    proc_freewalk(p->kpagetable);
  }
  p->kpagetable = 0;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
// 为给定进程创建用户页表，没有用户内存
// 只包含trampoline和trapframe页面
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  // 创建空的用户根页表
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  // 映射trampoline代码（用于系统调用返回）
  // 在最高的用户虚拟地址
  // 只有监管者（内核）使用它，在进出用户空间时，所以不设置PTE_U
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  // 进入内核才能修改，所以没有PTE_U
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
// 释放进程的页表及其引用的物理内存
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
// 设置第一个用户进程（init进程）
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // if(u2kvmcopy(p->pagetable, p->kpagetable, 0,p->sz))
  //   panic("userinit: u2kvmcopy");
  
  p->cwd = namei("/"); // 获取根目录的inode

  p->state = RUNNABLE; // 标记为可运行

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
// 增加或减少用户内存n字节
// 成功返回0，失败返回-1
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {// 检查是否会超过TRAPFRAME（trapframe页面）
      return -1;// 内存不足
    }
    uint oldsz = sz;
    // 分配新内存
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
    u2kvmcopy(p->pagetable, p->kpagetable, PGROUNDUP(oldsz), sz);
  } else if(n < 0){ // 收缩内存
    sz = uvmdealloc(p->pagetable, sz, sz + n);
    // 内核页表中的映射同步缩小
    // sz = kama_kvmdealloc(p->kpagetable, sz, sz + n);
  }
  p->sz = sz;  // 更新进程内存大小
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
// 创建新进程，复制父进程
// 设置子进程内核栈，使其看起来像是从fork()系统调用返回的
// fork后的子进程状态是runable
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  // 从父进程复制用户内存到子进程
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz; // 设置子进程内存大小

  // 这里需要把新的子进程的页表，复制到用户的内核页表
  if(u2kvmcopy(np->pagetable,np->kpagetable,0,np->sz)<0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }

  // copy saved user registers.
  // 复制保存的用户寄存器（trapframe），把父进程的寄存器的值复制给子进程
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  // 使fork在子进程中返回0（系统调用约定）
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  // 增加打开文件描述符的引用计数，复制打开的文件，引用计数+1
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd); // 复制当前工作目录
  // 复制进程名
  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid; // 获取子进程PID

  release(&np->lock);
  // 设置父子关系（需要wait_lock）
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);
  // 将子进程标记为可运行
  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
// 将进程p的孤儿子进程重新分配给init进程
// 调用者必须持有wait_lock，必须在wait_lock内调用
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
// 退出当前进程，不返回
// 退出的进程保持在僵尸状态，直到其父进程调用wait()
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  // 关闭本进程所有打开的文件
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }
  // 释放当前工作目录
  begin_op(); // 开始文件系统操作
  iput(p->cwd); // 减少目录inode引用计数
  end_op(); // 结束文件系统操作
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);// 将本进程的子进程交给init进程收养

  // Parent might be sleeping in wait().
  wakeup(p->parent); // 父进程可能在wait()中睡眠，唤醒它
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE; // 变为僵尸状态

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();// 让出cpu
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// 等待子进程退出并返回其pid
// 如果这个进程没有子进程，返回-1
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc(); // 当前进程

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    // 扫描进程表寻找已退出的子进程
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          // 如果需要，将退出状态复制到用户空间
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);// 释放子进程资源
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    // 如果没有子进程或者本进程被杀死，不需要等待
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    // 等待子进程退出（睡眠在wait_lock上）
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
// 每个CPU的进程调度器
// 每个CPU在设置好自身后调用scheduler()
// 调度器永不返回。它循环执行：
//  - 选择一个进程运行
//  - 切换到该进程开始运行
//  - 最终该进程通过swtch将控制权交回调度器
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu(); // 当前CPU

  c->proc = 0; // 初始时没有进程在运行
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    // 最近运行的进程可能关闭了中断
    // 启用中断以避免所有进程都在等待时的死锁
    // 然后再次关闭中断以避免中断和wfi之间的竞争
    intr_on();
    intr_off();

    int found = 0;// 是否找到可运行进程
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);// 获取进程锁
      if(p->state == RUNNABLE) { // 找到可运行进程
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        // 切换到选择的进程
        p->state = RUNNING; // 状态改为运行中
        c->proc = p; // 设置CPU当前运行的进程
        // 切换到要马上运行的新进程的内核页表
        w_satp(MAKE_SATP(p->kpagetable));
        sfence_vma();
        swtch(&c->context, &p->context);// 切换到进程的上下文,cpu跳转到p进程执行，然后时间片用完再跳转回来
        kvminithart(); // 切换回全局的页表，因为如果不切换有可能访问到上一个进程的内核栈
        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0; // CPU不再运行进程
        found = 1; // 标记找到了进程
      }
      release(&p->lock);
    }
    if(found == 0) {
      // 没有可运行的进程
      // 停止在此核心上运行，直到中断到来
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
// 切换到调度器。必须只持有p->lock
// 并且已经改变了proc->state
// 保存和恢复intena，因为intena是这个内核线程的属性，不是CPU的
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1) // 中断禁用嵌套深度应为1
    panic("sched locks");
  if(p->state == RUNNING) // 运行中的进程不应调用sched
    panic("sched RUNNING");
  if(intr_get()) // 中断应被禁用
    panic("sched interruptible");

  intena = mycpu()->intena; // 保存当前中断状态
  swtch(&p->context, &mycpu()->context);// 切换到调度器上下文
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
// 放弃CPU进行一次调度轮转
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
// fork子进程的第一次调度将由scheduler()切换到forkret
void
forkret(void)
{
  extern char userret[]; // trampoline.S中的userret地址
  static int first = 1; // 是否是第一次调用
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV); // 初始化文件系统

    first = 0; // 标记已初始化
    // ensure other cores see first=0.
    __sync_synchronize(); // 内存屏障

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    // 现在文件系统已初始化，可以调用kexec了
    // 将kexec的返回值（argc）放入a0寄存器
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  // 返回到用户空间，模拟usertrap()的返回
  prepare_return();// 准备返回用户空间的状态
  uint64 satp = MAKE_SATP(p->pagetable);// 用户页表的satp格式
  // TRAMPOLINE是虚拟地址，对应trampoline的入口，userret是trampoline.S中的userret地址，表示从内核态空间返回到用户地址空间
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);// 调用userret，并且把satp传入userret，这是函数指针，相当于上下文的返回地址是如何返回用户态
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
// 在通道chan上睡眠，释放条件锁lk
// 被唤醒时重新获取lk
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  /* sched让出cpu所以主进程不运行了，因此把当前状态保留，
   但是如果还有子进程退出，这时候就会唤醒（父进程状态从sleep->runnable
   ，然后就可以被scheduler调度器执行，这时候sleep继续执行剩下的，相当于释放
   了一个子进程，然后继续到wait里的下一个循环，释放下一个exit的子进程）*/
  sched();

  // Tidy up.
  // 清理（被唤醒后从这里继续）
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
// 唤醒所有在通道chan上睡眠的进程
// 调用者应持有条件锁
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){// 不唤醒自己
      acquire(&p->lock);
      // 进程在sleeping状态，并且进程的睡眠通道与chan相等
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){ // 找到目标进程
      p->killed = 1;// 设置杀死标志
      if(p->state == SLEEPING){
        // Wake process from sleep().
        // 从sleep()中唤醒进程
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}
// 设置进程的killed标志
void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}
// 检查进程是否被杀死
int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
// 复制到用户地址或内核地址，取决于usr_dst
// 成功返回0，错误返回-1
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);// 复制到用户空间
  } else {
    memmove((char *)dst, src, len); // 复制到内核空间
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
// 从用户地址或内核地址复制，取决于usr_src
// 成功返回0，错误返回-1
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);// 从用户空间复制
  } else {
    memmove(dst, (char*)src, len); // 从内核空间复制
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
// 打印进程列表到控制台（调试用）
// 当用户在控制台输入^P时运行
// 不使用锁以避免卡住的机器进一步卡死
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)// 跳过未使用的槽位
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);// 打印PID、状态和名称
    printf("\n");
  }
}
