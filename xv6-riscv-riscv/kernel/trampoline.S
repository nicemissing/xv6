        #
        # low-level code to handle traps from user space into
        # the kernel, and returns from kernel to user.
        #
        # the kernel maps the page holding this code
        # at the same virtual address (TRAMPOLINE)
        # in user and kernel space so that it continues
        # to work when it switches page tables.
        # kernel.ld causes this code to start at 
        # a page boundary.
        #
        # (这段代码用于处理从用户空间陷入内核以及从内核返回用户空间
        # 内核将此代码所在的页面映射到相同的虚拟地址(TRAMPOLINE)
        # 无论是在用户空间还是内核空间，这样在切换页表时它仍然可以工作
        # kernel.ld链接器脚本确保这段代码从页边界开始)

#include "riscv.h"
#include "memlayout.h"

.section trampsec # 将代码放入特殊的"trampsec"段
.globl trampoline  # 全局符号，使其他文件可以引用
.globl usertrap
trampoline: # 跳板代码起始地址
.align 4 # 4字节对齐，确保指令正确对齐
.globl uservec # 用户态陷阱入口点，全局可见
uservec:    
	#
        # trap.c sets stvec to point here, so
        # traps from user space start here,
        # in supervisor mode, but with a
        # user page table.
        #
        # ========================================================
        # 用户态陷阱处理入口点
        # 当用户程序发生陷阱时，CPU会跳转到这里
        # 此时处于监管者模式，但使用的是用户页表
        # ========================================================

        # save user a0 in sscratch so
        # a0 can be used to get at TRAPFRAME.
        csrw sscratch, a0 # 将a0的值保存到sscratch特殊寄存器

        # each process has a separate p->trapframe memory area,
        # but it's mapped to the same virtual address
        # (TRAPFRAME) in every process's user page table.
        li a0, TRAPFRAME # 将TRAPFRAME虚拟地址加载到a0寄存器
        
        # save the user registers in TRAPFRAME
        # --------------------------------------------------------
        # 保存用户寄存器到陷阱帧(TRAPFRAME)
        # 注意：a0已经在sscratch中保存，后面会从sscratch恢复
        # --------------------------------------------------------
        sd ra, 40(a0) # 保存返回地址
        sd sp, 48(a0) # 保存栈指针，应该是用户栈
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)
 	# save the user a0 in p->trapframe->a0
        csrr t0, sscratch # 从sscratch读取之前保存的a0值到t0
        sd t0, 112(a0) # 将原始a0值保存到陷阱帧的a0位置
        
        # --------------------------------------------------------
        # 切换到内核上下文
        # --------------------------------------------------------

        # initialize kernel stack pointer, from p->trapframe->kernel_sp
        ld sp, 8(a0)  # 加载内核栈指针（从陷阱帧的kernel_sp字段），将内核栈指针加载到sp寄存器

        # make tp hold the current hartid, from p->trapframe->kernel_hartid
        ld tp, 32(a0) # 加载当前硬件线程ID（hartid）（从陷阱帧的kernel_hartid字段）

        # load the address of usertrap(), from p->trapframe->kernel_trap
        ld t0, 16(a0) # 加载usertrap()函数地址（从陷阱帧的kernel_trap字段）

        # fetch the kernel page table address, from p->trapframe->kernel_satp.
        ld t1, 0(a0) # 加载内核页表地址（从陷阱帧的kernel_satp字段）

        # wait for any previous memory operations to complete, so that
        # they use the user page table.
        sfence.vma zero, zero # 清空TLB和内存操作流水线

        # install the kernel page table.
        #加载的是内核页表的satp格式地址，使用内核映射
        csrw satp, t1 # 将内核页表信息写入satp寄存器

        # flush now-stale user entries from the TLB.
        sfence.vma zero, zero # 刷新TLB，清除过时的用户页表项

        # call usertrap()
        jalr t0 # 跳转到t0指向的地址（usertrap函数）

# ========================================================
# 用户态陷阱返回点
# ========================================================
.globl userret
userret:
        # usertrap() returns here, with user satp in a0.
        # return from kernel to user.
        # --------------------------------------------------------
        # usertrap()函数返回到这里，a0包含用户页表的satp值
        # 此时准备从内核空间返回到用户空间
        # --------------------------------------------------------
        # switch to the user page table.
        sfence.vma zero, zero
        # a0是参数，也就是proc里传入的satp，将其写入satp，切换进程页表
        csrw satp, a0 # 将用户页表satp值写入satp寄存器
        sfence.vma zero, zero
        # TRAPFRAME是虚拟地址，将TRAPFRAME地址加载进a0
        li a0, TRAPFRAME

        # restore all but a0 from TRAPFRAME
        ld ra, 40(a0) # 恢复返回地址
        ld sp, 48(a0) # 恢复用户栈指针
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	# restore user a0
        # 恢复用户a0寄存器（最后恢复，因为a0当前正指向TRAPFRAME）
        ld a0, 112(a0) # 从陷阱帧恢复用户原始的a0值
        
        # return to user mode and user pc.
        # usertrapret() set up sstatus and sepc.
        sret # 从陷阱返回，切换回用户模式
