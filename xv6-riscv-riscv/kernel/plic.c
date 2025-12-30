#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//
// 初始化PLIC，设置设备中断优先级
// 输入：无
// 输出：设置UART和VIRTIO磁盘设备的中断优先级为1
void
plicinit(void)
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  *(uint32*)(PLIC + UART0_IRQ*4) = 1; // 设置UART串口中断优先级
  *(uint32*)(PLIC + VIRTIO0_IRQ*4) = 1; // 设置磁盘中断优先级
}
// 初始化当前CPU核心（hart）的PLIC设置
// 输入：无（通过cpuid()获取当前CPU核心ID）
// 输出：设置当前CPU核心的中断使能位和优先级阈值
void
plicinithart(void)
{
  int hart = cpuid();
  
  // set enable bits for this hart's S-mode
  // for the uart and virtio disk.
  // 启用该hart的中断使能位
  *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

  // set this hart's S-mode priority threshold to 0.
  // 设置hart的优先级阈值为0
  *(uint32*)PLIC_SPRIORITY(hart) = 0;
}

// ask the PLIC what interrupt we should serve.
// 向PLIC请求当前待处理的中断（中断认领）
// 输入：无（通过cpuid()获取当前CPU核心ID）
// 输出：返回中断号（IRQ number），0表示没有待处理中断
int
plic_claim(void)
{
  int hart = cpuid();
  int irq = *(uint32*)PLIC_SCLAIM(hart);
  return irq; // 返回中断号
}

// tell the PLIC we've served this IRQ.
// 通知PLIC已完成指定中断的处理（中断完成）
// 输入：irq - 已完成处理的中断号
// 输出：将中断号写回SCLAIM寄存器，告知PLIC可以继续接收该设备的中断
void
plic_complete(int irq)
{
  int hart = cpuid();
  *(uint32*)PLIC_SCLAIM(hart) = irq;
}
