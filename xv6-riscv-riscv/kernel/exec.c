#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
// 将ELF程序头中的权限标志转换为页表项权限位
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1) // 如果可执行标志置位
      perm = PTE_X; // 添加执行权限
    if(flags & 0x2) // 如果可写标志置位
      perm |= PTE_W; // 添加写权限
    return perm; // 返回权限位（默认可读）
}

//
// the implementation of the exec() system call
//
//
// exec()系统调用的实现
// 功能：加载并执行一个新的程序，替换当前进程的地址空间
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf; // ELF文件头
  struct inode *ip; // 可执行文件的inode
  struct proghdr ph; // ELF程序头
  pagetable_t pagetable = 0, oldpagetable; // 新旧页表
  struct proc *p = myproc(); // 当前进程

  begin_op(); // 开始文件系统操作

  // Open the executable file.
  if((ip = namei(path)) == 0){ // 根据路径查找文件
    end_op();
    return -1; // 文件不存在
  }
  ilock(ip); // 锁定inode

  // Read the ELF header.
  // 2. 读取ELF文件头
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad; // 读取失败

  // Is this really an ELF file?
  // 检查是否为有效的ELF文件
  if(elf.magic != ELF_MAGIC) // 检查魔数
    goto bad;
  // 3. 创建新的页表（替代当前进程的页表）
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad; // 创建页表失败

  // Load program into memory.
  // 4. 将程序段加载到内存中
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    // 读取程序头
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD) // 只处理加载类型的程序段
      continue;
    // 验证程序头信息
    if(ph.memsz < ph.filesz) // 内存大小不能小于文件大小
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr) // 检查地址溢出
      goto bad;
    if(ph.vaddr % PGSIZE != 0) // 虚拟地址必须页对齐
      goto bad;
    uint64 sz1;
    // 分配内存空间，程序段，在这里分配用户空间的内存（程序段）
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
    // 加载程序段内容到内存
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip); // 释放inode锁和引用
  end_op(); // 结束文件系统操作
  ip = 0; // 标记ip已处理

  p = myproc(); // 重新获取当前进程（可能被中断切换）
  uint64 oldsz = p->sz; // 保存旧的内存大小

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  // 5. 分配用户栈空间，用户栈段
  sz = PGROUNDUP(sz); // 将sz向上舍入到页边界
  uint64 sz1;
  // 分配栈空间：USERSTACK个页面作为栈，第1个页面作为栈保护页，在这里分配用户空间的内存（栈段）
  if((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  // 清除栈保护页的映射，使其不可访问
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  // 设置栈指针和栈基址
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  // 6. 复制参数到用户栈
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG) // 检查参数数量是否超过限制
      goto bad;
    sp -= strlen(argv[argc]) + 1; // 减去字符串长度加终止符
    sp -= sp % 16; // riscv sp must be 16-byte aligned // RISC-V要求栈指针16字节对齐
    if(sp < stackbase) // 检查栈溢出
      goto bad;
    // 将参数字符串复制到用户栈
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp; // 记录参数地址
  }
  ustack[argc] = 0; // 参数数组以NULL结尾

  // push a copy of ustack[], the array of argv[] pointers.
  // 7. 将参数指针数组压栈
  sp -= (argc+1) * sizeof(uint64); // 为指针数组分配空间
  sp -= sp % 16; // 对齐
  if(sp < stackbase) // 栈溢出检查
    goto bad;
  // 复制参数指针数组到用户栈
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  // 8. 设置trapframe中的参数，参数一般是局部变量，通过压栈压进去
  // a0将包含argc（通过系统调用返回值设置）
  p->trapframe->a1 = sp; // a1寄存器指向argv数组

  // Save program name for debugging.
  // 9. 保存程序名用于调试
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1; // 找到最后一个'/'之后的部分（程序名）
  safestrcpy(p->name, last, sizeof(p->name)); // 复制程序名到进程结构

  uvmunmap(p->kpagetable, 0, PGROUNDUP(oldsz) / PGSIZE, 0);
  // 将新的用户空间的页表内容拷贝到内核页表中
  if(u2kvmcopy(pagetable, p->kpagetable, 0, sz)<0)
    goto bad;
  // Commit to the user image.
  // 10. 提交新的用户镜像（替换旧的地址空间）
  oldpagetable = p->pagetable; // 保存旧页表
  p->pagetable = pagetable; // 设置新页表
  p->sz = sz; // 更新进程内存大小
  // 设置程序入口点和栈指针
  p->trapframe->epc = elf.entry;  // initial program counter = ulib.c:start() // 程序计数器指向ELF入口点
  p->trapframe->sp = sp; // initial stack pointer // 栈指针指向当前栈顶
  proc_freepagetable(oldpagetable, oldsz); // 释放旧的页表和内存空间
  // 返回值进入a0寄存器，作为main函数的argc参数
  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad: // 错误处理
  if(pagetable)
    proc_freepagetable(pagetable, sz); // 释放新创建的页表
  if(ip){
    iunlockput(ip); // 释放文件锁
    end_op(); // 结束文件操作
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
// 加载ELF程序段到页表中指定的虚拟地址
// va必须是页对齐的，并且va到va+sz的页面必须已经映射
// 成功返回0，失败返回-1
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa; // 物理地址
  // 逐页加载程序段
  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i); // 通过页表查找虚拟地址对应的物理地址
    if(pa == 0)
      panic("loadseg: address should exist");
    // 计算本次读取的字节数（最后一页可能不满）
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    // 从文件中读取数据到物理内存
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
