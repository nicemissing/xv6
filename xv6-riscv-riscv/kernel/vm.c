#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
/*
  pte包含PPN和标志位，一共是54位
  pa包含56位，是PPN和偏移
  PPN是物理页号基址
*/
/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.// 内核代码结束位置，由链接脚本kernel.ld定义

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  // 这是根目录页表，一个页表可以存储512个页表项，可以把整个虚拟空间存储进去
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc(); // 分配一个物理页，返回这个页的指针
  memset(kpgtbl, 0, PGSIZE); // 将分配的页表清零

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  // 映射内核数据和物理RAM（可读写），内核区除了代码剩下的位置
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  // 这是用户和内核切换的代码，如果和用户位置不同，那么切换到内核后，无法执行下一条指令，因此虚拟地址和用户相同
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  //内核页表(kpgtbl) 中为每个进程建立内核栈映射！
  // proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
// 向内核页表添加映射（仅在内核启动时使用）
// 不刷新TLB，也不启用分页
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
// 初始化内核页表，被所有CPU共享
void
kvminit(void)
{
  kernel_pagetable = kvmmake(); // 创建内核页表
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();// 页表更改的刷新

  w_satp(MAKE_SATP(kernel_pagetable));//将内核页表的物理地址写入 satp 寄存器，使硬件 MMU 开始使用该页表进行地址翻译。

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
// 返回页表中对应虚拟地址va的叶子页表的PTE（页表项）地址，只是一个查找函数和内核态，用户态无关
// 如果alloc!=0，则创建所需的页表页
//
// RISC-V Sv39方案有三层页表
// 一个页表页包含512个64位PTE
// 64位虚拟地址分为五个字段：
//   39..63 -- 必须为零
//   30..38 -- 9位二级索引
//   21..29 -- 9位一级索引
//   12..20 -- 9位零级索引
//    0..11 -- 12位页内字节偏移
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    // PX只保留对应level的9位PTE索引，3级页表有3个level
    pte_t *pte = &pagetable[PX(level, va)]; // 获取当前级别的PTE
    if(*pte & PTE_V) {// 如果PTE有效
      pagetable = (pagetable_t)PTE2PA(*pte); // 进入下一级页表
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)// 需要分配但分配失败，一个物理页4096，存储512个页表项
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
// 查找虚拟地址对应的物理地址页，返回的不是物理地址，是物理地址页的基址，没有加偏移
// 只能用于查找用户页面（必须具有PTE_U标志）
// 返回物理地址，如果未映射则返回0
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(pagetable_t pgtbl, uint64 va)         // kvmpa 将虚拟地址翻译为物理地址（添加第一个参数）
{
    uint64 off = va % PGSIZE;
    pte_t *pte;
    uint64 pa;

    pte = walk(pgtbl, va, 0);			//kernel_pagetable改为参数pgtbl
    if (pte == 0)
        panic("kvmpa");
    if ((*pte & PTE_V) == 0)
        panic("kvmpa");
    pa = PTE2PA(*pte);
    return pa + off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
// 创建从虚拟地址va到物理地址pa的映射，将最后一级的页表和物理地址通过页表项关联起来
// va和size必须页面对齐，pagetable类似于根目录，分配一页用来存储根目录，最高的9位
// 成功返回0，如果walk()无法分配所需的页表页则返回-1
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0) // 检查虚拟地址是否页面对齐，虚拟地址分配的时候要对齐，4KB对齐
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0) // 检查大小是否页面对齐
    panic("mappages: size not aligned");

  if(size == 0) // 检查大小是否为0
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;// 最后一个要映射的地址
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0) // 获取PTE地址，如果需要则分配页表页
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V; // 设置PTE：物理地址|权限|有效位，将物理地址转化为PTE给最后一级页表
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
// 创建一个空的用户页表
// 如果内存不足则返回0
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// lab3 模仿kvminit，分配内核页表映射
void
ukvmmap(pagetable_t kpagetable, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpagetable, va, sz, pa, perm)!=0)
  {
    panic("ukvmmap");
  }
}

// lab3 模仿uvmunmap
pagetable_t
ukvminit()
{
  pagetable_t kpagetable = (pagetable_t)kalloc();
  if(kpagetable == 0)
  {
    return kpagetable;
  }
  memset(kpagetable, 0, PGSIZE);
  ukvmmap(kpagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);
  ukvmmap(kpagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  ukvmmap(kpagetable, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);
  ukvmmap(kpagetable, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);
  ukvmmap(kpagetable, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);
  ukvmmap(kpagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
  return kpagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
// 删除从va开始的npages个页面的映射
// va必须页面对齐。如果映射不存在也没关系
// 如果do_free为真，则释放物理内存
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// 分配PTE和物理内存，使进程大小从oldsz增长到newsz
// oldsz和newsz不需要页面对齐
// 返回新大小，错误时返回0
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc(); // 分配物理页面
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);// 回滚已分配的内存
      return 0;
    }
    memset(mem, 0, PGSIZE);
    // 建立映射：用户可访问，可读写，可能可执行（xperm）
    // 必须循环分配，因为kalloc只能分配一页，kvmap可以是因为内核指定了物理地址，用户无法指定
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
// 释放用户页面，将进程大小从oldsz减小到newsz
// oldsz和newsz不需要页面对齐，newsz也不需要小于oldsz
// oldsz可以大于实际进程大小
// 返回新的进程大小
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
// 递归释放页表页
// 所有叶子映射必须已经移除
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  // 一个页表中有512个PTE
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);// 这个PTE指向一个更低级别的页表（非叶子PTE）
      freewalk((pagetable_t)child);// 递归释放子页表
      pagetable[i] = 0;// 清除PTE
    } else if(pte & PTE_V){// 如果还有有效的叶子PTE，说明有错误
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);//释放当前页表页
}

void
proc_freewalk(pagetable_t pagetable)
{
  for(int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if (pte & PTE_V)
    {
      pagetable[i] = 0;
      if((pte & (PTE_R | PTE_W | PTE_X))==0)
      {
        uint64 child = PTE2PA(pte);
        proc_freewalk((pagetable_t)child);
      }
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
// 先释放用户内存页面，然后释放页表页
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
// 给定父进程的页表，将其内存复制到子进程的页表中
// 复制页表和物理内存（写时复制需要修改）
// 成功返回0，失败返回-1
// 失败时释放任何已分配的页面
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;
  // 遍历父进程的每个页面
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)// 获取父进程的PTE
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)// 检查PTE是否有效
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);// 获取物理地址
    flags = PTE_FLAGS(*pte);// 获取权限标志，取后10位
    if((mem = kalloc()) == 0)// 为子进程分配物理页面
      goto err;
    memmove(mem, (char*)pa, PGSIZE);// 复制页面内容
    // 在子进程页表中建立映射
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);// 映射失败，释放页面
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);// 释放已分配的内存
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
// 清除PTE的用户访问权限（设置PTE_U为0）
// 由exec用于用户栈保护页
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
// 从内核复制到用户空间
// 将len字节从src复制到给定页表中虚拟地址dstva处
// src(len) --> dstva
// 成功返回0，错误返回-1
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);// 获取目标虚拟地址所在的页面起始地址
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);// 获取物理地址
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {// 如果没有建立映射，就申请一页映射
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);// 获取PTE
    // forbid copyout over read-only user text pages.
    // 禁止复制到只读区域
    if((*pte & PTE_W) == 0)// 检查是否可写
      return -1;
      
    n = PGSIZE - (dstva - va0);// 计算当前页面中可复制的字节数
    if(n > len)// 如果请求的长度小于可复制长度
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);// 执行复制，操作的物理地址

    len -= n;// 减少剩余长度
    src += n;// 移动源指针
    dstva = va0 + PGSIZE;// 移动到下一个页面
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
// 从用户空间复制到内核
// 将len字节从给定页表中虚拟地址srcva处复制到dst
// 成功返回0，错误返回-1
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva); // 获取srcva的页起始地址
    pa0 = walkaddr(pagetable, va0); // 获取va0的物理地址
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {// 尝试处理缺页
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);// 计算当前页面中可复制的字节数
    if(n > len)// 如果请求的长度小于可复制长度
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
  // return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
// 从用户空间复制一个以空字符结尾的字符串到内核
// 从给定页表中虚拟地址srcva处复制字节到dst，直到遇到'\0'或达到max
// srcva --> dst
// 成功返回0，错误返回-1
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;// 是否遇到空字符

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);// 获取页面起始地址
    pa0 = walkaddr(pagetable, va0); // 获取物理地址
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0); // 计算当前页面中可复制的字节数
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0)); // 计算实际物理地址
    while(n > 0){
      if(*p == '\0'){// 遇到字符串结束符
        *dst = '\0';
        got_null = 1;// 标记已找到结束符
        break;
      } else {
        *dst = *p; // 复制字符
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE; // 移动到下一个页面
  }
  if(got_null){// 成功复制了整个字符串
    return 0;
  } else {// 达到max长度但未找到结束符
    return -1;
  }
  //  return copyinstr_new(pagetable, dst, srcva, max);
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
// 如果进程引用了一个在sys_sbrk()中延迟分配的页面，则分配并映射用户内存
// 如果va无效、已映射或物理内存不足，则返回0；成功则返回物理地址
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)// 检查虚拟地址是否在进程大小范围内
    return 0;
  va = PGROUNDDOWN(va);// 对齐到页面边界
  if(ismapped(pagetable, va)) {// 检查是否已映射
    return 0;
  }
  mem = (uint64) kalloc();// 分配物理页面
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  // 建立映射：可写、用户可访问、可读
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}
// 检查虚拟地址是否已映射
int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

// lab3 物理地址只有128M，内核设备驱动从UART0开始，但是用户是从0虚拟地址开始增，因此无论如何也到不了128M，因此只能用内核页表映射UART0
int 
u2kvmcopy(pagetable_t upgtbl, pagetable_t kpgtbl, uint64 begin, uint64 end)
{
  pte_t* pte;
  uint64 pa,i;
  uint flag;

  for (i = begin; i < end; i += PGSIZE)
  {
    if((pte = walk(upgtbl, i, 0)) == 0)
    {
      panic("uvmmap_copy: pte should exist");
    }
    if ((*pte & PTE_V) == 0)
    {
      panic("uvmmap_copy: page not exist");
    }
    pa = PTE2PA(*pte);
    // 在内核页表中去掉PTE_U
    flag = PTE_FLAGS(*pte) & ~PTE_U;
    if (mappages(kpgtbl, i, PGSIZE, pa, flag)!=0)
    {
      uvmunmap(kpgtbl, 0, i/PGSIZE, 0); // 不释放物理空间
      return -1;
    }
  }
  return 0;
}

// 与 uvmdealloc 功能类似，将程序内存从 oldsz 缩减到 newsz，但不释放实际内存
uint64
kama_kvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
    if (newsz >= oldsz)
        return oldsz;

    if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
        int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        uvmunmap(pagetable, PGROUNDUP(newsz), npages, 0);
    }

    return newsz;
}
