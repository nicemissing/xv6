/*
  // RISC-V 有32个通用寄存器：
  // a0-a7: 函数参数/返回值寄存器（前8个参数）
  // t0-t6: 临时寄存器
  // s0-s11: 保存寄存器（跨函数调用保持）
  // ra: 返回地址
  // sp: 栈指针

  // 在swtch函数中：
  // a0 = 第一个参数（old context指针）
  // a1 = 第二个参数（new context指针）
*/

// Saved registers for kernel context switches.
// 保存内核上下文切换所需的寄存器
// 这个结构体专门用于内核线程之间的切换（如进程↔调度器）
struct context {
  uint64 ra; // 返回地址（Return Address） - 关键：控制流的切换点，当swtch()返回时，会跳转到这个地址执行
  uint64 sp; // 栈指针（Stack Pointer） - 关键：栈的切换，每个内核线程有自己的栈空间

  // callee-saved
  // 这些寄存器通常用于存储重要的中间结果
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
// 每个CPU核心的状态信息，主要用于多核调度
struct cpu {
  // 当前在此CPU上运行的进程，或为null（空闲时）
  struct proc *proc;          // The process running on this cpu, or null.
  // 调度器上下文，当swtch()切换到这里时进入scheduler()，每个CPU有自己的调度器上下文
  struct context context;     // swtch() here to enter scheduler().
  // 中断管理相关 - 用于嵌套中断禁用
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

// 全局CPU数组，NCPU=最大CPU核心数（在param.h定义，通常8）
extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
// 陷阱帧（Trap Frame）- 用于用户/内核模式切换时保存和恢复用户寄存器状态
// 位于用户页表中trampoline页下方的一页中（TRAPFRAME虚拟地址）
// 在内核页表中没有特殊映射（但通过恒等映射内核可以直接访问其物理地址）
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  // 锁 - 保护进程状态的并发访问
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  pagetable_t kpagetable; // the kernel table per process 专属内核页
};
