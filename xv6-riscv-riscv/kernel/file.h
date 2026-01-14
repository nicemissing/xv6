/*
这段代码定义了 xv6 操作系统中文件系统的高层抽象结构。
它主要包含三个核心概念：
  打开的文件 (struct file)、
  内存中的索引节点 (struct inode)、
  设备驱动接口 (struct devsw)。
*/

// 文件结构体。代表一个“打开的文件”。
// 它是进程文件描述符表（fd table）指向的对象。
// 即使是同一个磁盘文件，如果被多次打开，也会有多个 struct file 实例（为了维护各自的偏移量 off）。
struct file {
  // 文件类型枚举：无、管道、普通文件/目录(INODE)、设备
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;

  /* 
    引用计数。
    记录有多少个文件描述符（fd）指向这个结构体。
    例如：fork() 会复制 fd 表，导致 ref 增加；dup() 也会增加。
    当 ref 为 0 时，内核会回收这个结构体。
  */
  int ref;
  // 标志位：是否以“读”权限打开
  char readable;
  // 标志位：是否以“写”权限打开
  char writable;
  // 如果 type 是 FD_PIPE，指向管道结构体的指针
  struct pipe *pipe; // FD_PIPE
  // 如果 type 是 FD_INODE 或 FD_DEVICE，指向内存 inode 的指针
  struct inode *ip;  // FD_INODE and FD_DEVICE
  /* 
    文件读写偏移量 (offset)。
    记录下一次读写操作在文件中的字节位置。
    这是 struct file 存在的主要原因：区分不同打开实例的读取进度。
  */
  uint off;          // FD_INODE
  /* 
    如果 type 是 FD_DEVICE，存储主设备号。
    用于在 devsw 数组中查找对应的设备驱动程序。
  */
  short major;       // FD_DEVICE
};

// 宏定义：处理设备号
// xv6 使用 32 位的整数表示设备号，高 16 位是主设备号，低 16 位是次设备号。
#define major(dev)  ((dev) >> 16 & 0xFFFF) // 右移16位，提取主设备号
#define minor(dev)  ((dev) & 0xFFFF) // 提取次设备号
#define	mkdev(m,n)  ((uint)((m)<<16| (n))) // 将主设备号 m 和次设备号 n 组合成 32 位设备号。

// 内存中的 inode 副本 (In-memory Inode)
// 它是磁盘上 inode (struct dinode) 在内核内存中的缓存和扩展。
// 内核保证对于同一个磁盘文件，内存中只有一个 struct inode 实例。
struct inode {
  // 设备号：该文件存储在哪个磁盘设备上。
  uint dev;           
  /* 
    Inode 编号：该文件在磁盘上的唯一标识 ID。
    dev + inum 唯一确定了系统中的一个文件。
  */
  uint inum; 
  /*
    引用计数：该 inode 被多少个进程打开。
  */         
  int ref;
  /*
     睡眠锁：
     保护下面所有字段（valid, type, size, addrs 等）的并发访问。
     在读取或修改 inode 内容前必须持有此锁。
  */
  struct sleeplock lock;
  /* 
    有效性标志：
     valid = 0 表示数据尚未从磁盘读取（刚分配的空槽）。
     valid = 1 表示数据已从磁盘加载，可以安全使用。
     相关函数：ilock() 会检查此标志，若为 0 则触发磁盘读取。
  */
  int valid;
  // 文件类型：0(空闲), T_FILE(文件), T_DIR(目录), T_DEV(设备)
  short type;
  // 如果是设备文件 (T_DEV)，存储主设备号
  short major;
  // 如果是设备文件 (T_DEV)，存储次设备号
  short minor;
  // 硬链接计数。记录磁盘上有多少个目录项指向此 inode
  short nlink;
  // 文件大小（字节数）
  uint size;
  /* 
    数据块地址列表：
    存储文件内容所在的磁盘块号。包含 NDIRECT 个直接块和 1 个间接块。
  */
  uint addrs[NDIRECT+1];
};

// 设备开关表结构体 (Device Switch Table)
// 这是一个接口定义，用于实现设备驱动的多态性。
// 允许操作系统以统一的方式（read/write）操作不同的硬件设备。
struct devsw {
  int (*read)(int, uint64, int); // 函数指针：指向设备的读函数
  int (*write)(int, uint64, int); // 函数指针：指向设备的写函数
};

// 全局设备开关数组。
// 索引是主设备号 (major)，值是对应的读写函数
extern struct devsw devsw[];

// 定义控制台的主设备号为 1
#define CONSOLE 1
