#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock; // 保护ticks计数器的自旋锁
uint ticks; // 系统启动以来的时钟中断次数

extern char trampoline[], uservec[]; // 来自trampoline.S的符号地址

// in kernelvec.S, calls kerneltrap().
// 在内核态处理中断/异常的汇编入口（kernelvec.S），调用kerneltrap()
void kernelvec();

extern int devintr();// 设备中断处理函数声明

// 初始化陷阱处理子系统
void
trapinit(void)
{
  initlock(&tickslock, "time"); // 初始化保护ticks的锁
}

// set up to take exceptions and traps while in the kernel.
// 设置内核态的中断/异常处理向量
void
trapinithart(void)
{
  // 一旦发生异常/中断，CPU 会跳过去执行的那条指令的地址，应该在早期被初始化好
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
//
// 处理来自用户空间的中断、异常或系统调用
// 从trampoline.S调用，并返回到trampoline.S
// 返回值是用户页表的satp值，供trampoline.S切换页表使用
/*当CPU执行到usertrap()时，已经完成了：
  从用户态到内核态的切换
  页表切换（到内核页表）
  栈切换（到内核栈）*/
uint64
usertrap(void)
{
  int which_dev = 0; // 记录中断设备类型
  // 检查是否来自用户态（SPP位应为0）
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  // 现在处于内核态，将中断/异常处理设置为kerneltrap
  w_stvec((uint64)kernelvec);  //DOC: kernelvec

  struct proc *p = myproc(); // 获取当前进程
  
  // save user program counter.
  // 将发生trap的代码位置保存进epc方便之后恢复
  p->trapframe->epc = r_sepc(); // 将sepc保存到trapframe
  // 根据scause判断陷阱类型
  if(r_scause() == 8){
    // system call

    if(killed(p))// 如果进程已被杀死，则退出
      kexit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    // 如果系统调用sepc指向ecall指令，但我们要返回到下一条指令
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    // 没有保存完成现场不能开启中断
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // 设备中断（devintr返回非0表示已处理）
    // ok
  } else if((r_scause() == 15 || r_scause() == 13) &&
            vmfault(p->pagetable, r_stval(), (r_scause() == 13)? 1 : 0) != 0) {
    // page fault on lazily-allocated page
    // 页面错误（scause=13读缺页, 15写缺页）
    // vmfault尝试处理惰性分配的页面
  } else {
    // 未知的陷阱原因，打印信息并杀死进程
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    kexit(-1);

  // give up the CPU if this is a timer interrupt.
  // 如果是定时器中断，让出CPU（调度）
  if(which_dev == 2)
    yield();

  prepare_return();// 准备返回到用户空间

  // the user page table to switch to, for trampoline.S
  uint64 satp = MAKE_SATP(p->pagetable);// 用户页表的satp值，供trampoline.S切换页表

  // return to trampoline.S; satp value in a0.
  return satp;// 返回trampoline.S；satp值在a0寄存器中
}

//
// set up trapframe and control registers for a return to user space
//
//
// 设置trapframe和控制寄存器，为返回到用户空间做准备
// 输入：无
// 输出：设置stvec、trapframe和sstatus等寄存器
//
void
prepare_return(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(). because a trap from kernel
  // code to usertrap would be a disaster, turn off interrupts.
  // 即将把陷阱目标从kerneltrap切换到usertrap
  // 因为从内核代码触发usertrap将是灾难性的，所以关闭中断
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  // 将系统调用、中断和异常发送到trampoline.S中的uservec，所以trap发生时才能自动处理usertrap
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  // 还在内核中
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  // 清除SPP位（设置为0表示用户模式）
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  // 在用户模式下启用中断
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  // 可能中途被修改，sepc寄存器的作用是保存异常发生时用户态的pc，当sret返回时将sepc复制到pc中
  w_sepc(p->trapframe->epc);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
// 来自内核代码的中断和异常通过kernelvec来到这里，
// 使用当前的内核栈，在跳板函数中已通过汇编切换了内核栈指针
// 输入：无（通过寄存器获取陷阱信息），主要是时钟中断处理
// 输出：处理内核态中断/异常
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  // 检查是否来自监管者模式（内核态）
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  // 检查中断是否被禁用（内核陷阱处理期间应禁用中断）
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");
  // 处理设备中断
  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    // 未知来源的中断或陷阱
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  // 如果是定时器中断且有进程在运行，让出CPU
  if(which_dev == 2 && myproc() != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  // yield()可能引起一些陷阱，所以恢复陷阱寄存器供kernelvec.S的sepc指令使用
  w_sepc(sepc);
  w_sstatus(sstatus);
}
// 时钟中断处理函数
// 输入：无
// 输出：更新ticks计数器，设置下一次定时器中断
void
clockintr()
{// 只让CPU 0更新全局ticks计数器
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks); // 唤醒等待ticks的进程
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);// 0.1s一次
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
// 检查是外部中断还是软件中断，并处理它
// 输入：无（通过scause寄存器判断）
// 输出：返回2如果是定时器中断，1如果是其他设备中断，0如果不识别
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

