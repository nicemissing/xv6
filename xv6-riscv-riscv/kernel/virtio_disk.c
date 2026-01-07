//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

//
// qemu virtio磁盘设备的驱动程序
// 使用qemu的mmio接口访问virtio
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"
#include "proc.h"

// the address of virtio mmio register r.
// 获取virtio mmio寄存器r的地址，32位设备寄存器
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

// 磁盘数据结构
static struct disk {
  // a set (not a ring) of DMA descriptors, with which the
  // driver tells the device where to read and write individual
  // disk operations. there are NUM descriptors.
  // most commands consist of a "chain" (a linked list) of a couple of
  // these descriptors.
  struct virtq_desc *desc;

  // a ring in which the driver writes descriptor numbers
  // that the driver would like the device to process.  it only
  // includes the head descriptor of each chain. the ring has
  // NUM elements.
  struct virtq_avail *avail;

  // a ring in which the device writes descriptor numbers that
  // the device has finished processing (just the head of each chain).
  // there are NUM used ring entries.
  struct virtq_used *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  // track info about in-flight operations,
  // for use when completion interrupt arrives.
  // indexed by first descriptor index of chain.
  struct {
    struct buf *b;
    char status;
  } info[NUM];

  // disk command headers.
  // one-for-one with descriptors, for convenience.
  struct virtio_blk_req ops[NUM];
  
  struct spinlock vdisk_lock;
  
} disk;

// 初始化virtio磁盘
void
virtio_disk_init(void)
{
  uint32 status = 0; // 设备状态
  // 初始化磁盘锁
  initlock(&disk.vdisk_lock, "virtio_disk");
  // 检查设备标识：幻数、版本、设备ID、供应商ID
  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 2 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk");
  }
  
  // reset device // 重置设备
  *R(VIRTIO_MMIO_STATUS) = status;

  // set ACKNOWLEDGE status bit // 设置ACKNOWLEDGE状态位
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  // set DRIVER status bit // 设置DRIVER状态位
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // negotiate features // 协商特性
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  // 告诉设备特性协商完成
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // re-read status to ensure FEATURES_OK is set.
  // 重新读取状态以确保FEATURES_OK被设置
  status = *R(VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // initialize queue 0. // 初始化队列0
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

  // ensure queue 0 is not in use.// 确保队列0未被使用
  if(*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  // check maximum queue size. // 检查最大队列大小
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM) // 确保设备支持足够的描述符
    panic("virtio disk max queue too short");

  // allocate and zero queue memory.
  disk.desc = kalloc(); // 分配描述符表
  disk.avail = kalloc(); // 分配可用环
  disk.used = kalloc(); // 分配已用环
  if(!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc, 0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);

  // set queue size. // 设置队列大小
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // write physical addresses. // 写入物理地址
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

  // queue is ready. // 队列就绪
  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // all NUM descriptors start out unused. // 所有NUM个描述符初始状态为空闲
  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // tell device we're completely ready. // 告诉设备我们完全就绪
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;
  // plic.c和trap.c安排来自VIRTIO0_IRQ的中断
  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
}

// find a free descriptor, mark it non-free, return its index.
// 查找空闲描述符，标记为非空闲，返回其索引
static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
// 标记描述符为空闲
static void
free_desc(int i)
{
  if(i >= NUM)
    panic("free_desc 1");
  if(disk.free[i])
    panic("free_desc 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]); // 唤醒可能等待空闲描述符的进程
}

// free a chain of descriptors.
// 释放描述符链
static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
// 分配三个描述符（它们不需要连续）
// 磁盘传输总是使用三个描述符
static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

// virtio_disk_rw函数：执行磁盘读写操作
// 输入：b - 缓冲区指针，包含要读写的数据和块号等信息
//       write - 0表示读操作，非0表示写操作
// 输出：无（但会修改缓冲区b的内容，并可能阻塞直到操作完成）
void
virtio_disk_rw(struct buf *b, int write)
{
  // 计算扇区号：将文件系统块号转换为磁盘扇区号
  // 注意：BSIZE通常是1024字节，磁盘扇区是512字节，所以每个块占2个扇区
  uint64 sector = b->blockno * (BSIZE / 512);
  // 获取磁盘锁，防止并发访问磁盘数据结构
  acquire(&disk.vdisk_lock);

  // the spec's Section 5.2 says that legacy block operations use
  // three descriptors: one for type/reserved/sector, one for the
  // data, one for a 1-byte status result.

  // allocate the three descriptors.
  // 分配三个描述符用于本次操作
  int idx[3]; // 存储三个描述符的索引
  while(1){
    // 尝试分配三个连续（逻辑上，不一定物理连续）的描述符
    if(alloc3_desc(idx) == 0) {
      break;
    }
    // 分配失败（没有空闲描述符），等待直到有描述符可用
    // sleep会释放锁并在被唤醒后重新获取锁
    sleep(&disk.free[0], &disk.vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.
  // 第一个描述符对应请求头（命令）
  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];
  // 设置操作类型：读或写
  if(write)
    buf0->type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0->type = VIRTIO_BLK_T_IN; // read the disk
  buf0->reserved = 0; // 保留字段置0
  buf0->sector = sector; // 设置要读写的扇区号

  // disk.desc[idx[0]].addr = (uint64) buf0;
  // lab3
  // 设置第一个描述符：指向请求头
  // 原代码：disk.desc[idx[0]].addr = (uint64) buf0;
  // lab3修改：使用kvmpa将虚拟地址转换为物理地址，使用当前进程的内核页表
  disk.desc[idx[0]].addr = (uint64) kvmpa(myproc()->kpagetable, (uint64) buf0);  // 调用 myproc()获取进程内核页表
  disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];
  // 设置第二个描述符：指向数据缓冲区
  disk.desc[idx[1]].addr = (uint64) b->data;
  disk.desc[idx[1]].len = BSIZE;
  if(write)
    disk.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];
  // 设置第三个描述符：指向状态字节
  disk.info[idx[0]].status = 0xff; // device writes 0 on success
  disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  disk.desc[idx[2]].next = 0;

  // record struct buf for virtio_disk_intr().
  b->disk = 1;  // 标记缓冲区正在被磁盘使用
  disk.info[idx[0]].b = b; // 将缓冲区与描述符链关联

  // tell the device the first index in our chain of descriptors.
  // 将链头索引放入可用环的当前位置
  disk.avail->ring[disk.avail->idx % NUM] = idx[0];
  // 内存屏障：确保之前的写入（特别是avail->ring的更新）对设备可见
  __sync_synchronize();

  // tell the device another avail ring entry is available.
  // 告诉设备可用环中有新的可用条目
  disk.avail->idx += 1; // not % NUM ...

  __sync_synchronize();

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 1) {
    sleep(b, &disk.vdisk_lock);
  }

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

  release(&disk.vdisk_lock);
}

void
virtio_disk_intr()
{
  acquire(&disk.vdisk_lock);

  // the device won't raise another interrupt until we tell it
  // we've seen this interrupt, which the following line does.
  // this may race with the device writing new entries to
  // the "used" ring, in which case we may process the new
  // completion entries in this interrupt, and have nothing to do
  // in the next interrupt, which is harmless.
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __sync_synchronize();

  // the device increments disk.used->idx when it
  // adds an entry to the used ring.

  while(disk.used_idx != disk.used->idx){
    __sync_synchronize();
    int id = disk.used->ring[disk.used_idx % NUM].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    struct buf *b = disk.info[id].b;
    b->disk = 0;   // disk is done with buf
    wakeup(b);

    disk.used_idx += 1;
  }

  release(&disk.vdisk_lock);
}
