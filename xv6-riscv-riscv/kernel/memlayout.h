// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0 
// 10001000 -- virtio disk 
// 80000000 -- qemu's boot ROM loads the kernel here,
//             then jumps here.
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio interface
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
/*
    KERNBASE (0x80000000) ──┐
                            │ 内核代码和数据
                            │（entry.S、text、data、bss）
    end ────────────────────┘
                            │ 内核页分配区域
                            │（用于分配物理页）
    PHYSTOP ────────────────┘

    物理内存布局（实际硬件）：
    0x00000000: ┌─────────────────┐
                │ 未使用/设备内存 │
    0x80000000: ├─────────────────┤ ← KERNBASE
                │ 内核代码        │
                │ 内核数据        │
        end:   ├─────────────────┤ ← 内核静态部分结束
                │ 内核页分配区域  │ ← 动态管理
                │   - 用户进程页  │
                │   - 内核栈      │
                │   - trapframe页 │
                │   - 页表页      │
                │   - 缓冲缓存    │
                │   - 空闲页面    │
    PHYSTOP:   └─────────────────┘ ← 内核管理结束 (128MB)
    内核总共使用128MB物理内存
*/
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
/*
 KSTACK是每个进程在内核模式下执行时使用的栈。当用户进程通过系统调用或中断进入内核时，CPU会切换到内核栈
 当用户程序执行系统调用时：
    用户态 ecall → 切换到KSTACK → 执行内核代码 → 返回用户态
*/
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
