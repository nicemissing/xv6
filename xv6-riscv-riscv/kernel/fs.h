// On-disk file system format.
// Both the kernel and user programs use this header file.
// 磁盘文件系统格式定义
// 内核和用户程序都使用这个头文件

// 根目录的inode编号（1号inode总是根目录）
#define ROOTINO  1   // root i-number
// 块大小：1024字节
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
// 磁盘布局：
// [引导块 | 超级块 | 日志块 | inode块 | 空闲位图块 | 数据块]
//
// mkfs程序计算超级块并构建初始文件系统。超级块描述磁盘布局：
struct superblock {
  // 用于标识文件系统类型的魔数，编译 fs.img 时写入固定值（0x10203040），系统启动后读取这个值，判定是否是 xv6 文件系统
  uint magic;        // Must be FSMAGIC
  // 文件系统镜像的总块数
  uint size;         // Size of file system image (blocks)
  // 可用于存储数据的块数（不包括引导块、超级块、inode块、位图块等系统块）
  uint nblocks;      // Number of data blocks
  // inode 节点数量，目前 NINODES 为 200，也就是说 xv6 最多能创建 200 个文件/目录
  uint ninodes;      // Number of inodes.
  // 日志区的块数量，当前 LOGSIZE 固定为 30
  uint nlog;         // Number of log blocks
  // 日志区起始块号，由于日志区、inode区的大小是固定的，所以下面几个值都可以通过计算得到，把它们看做常量好了
  uint logstart;     // Block number of first log block
  // 第一个inode块的块号
  uint inodestart;   // Block number of first inode block
  // 第一个空闲位图块的块号
  uint bmapstart;    // Block number of first free map block
};

// 文件系统魔数，用于标识xv6文件系统
#define FSMAGIC 0x10203040

// 通过块寻址，寻找文件系统在磁盘上的地址
#define NDIRECT 12 // 直接块数量：12个直接块
#define NINDIRECT (BSIZE / sizeof(uint)) // 间接块能包含的指针数：每个块指针4字节，所以1024/4=256
#define MAXFILE (NDIRECT + NINDIRECT) // 最大文件大小（块数）：直接块+间接块

// On-disk inode structure
// 磁盘inode结构，一个文件一个 inode
struct dinode {
  // 文件类型（普通文件、目录、设备文件等）
  short type;           // File type
  // 主设备号（仅T_DEVICE类型使用）
  short major;          // Major device number (T_DEVICE only)
  // 次设备号（仅T_DEVICE类型使用）
  short minor;          // Minor device number (T_DEVICE only)
  // 文件系统中链接到此inode的链接数
  short nlink;          // Number of links to inode in file system
  // 文件大小（字节）
  uint size;            // Size of file (bytes)
  // 数据块地址数组：12个直接块地址+1个间接块地址
  uint addrs[NDIRECT+1];   // Data block addresses
};

// Inodes per block. // 每个块能包含的inode数量
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i // 计算第i个inode在inode区的第几块，inode块号 = inode编号 / 每块inode数 + inode区域起始块
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block // 每个位图块能表示的位数（每个字节8位）
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b // 计算数据块b对应在哪个位图块中，位图块号 = 数据块号 / 每块位数 + 位图区域起始块
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures. // 目录是包含一系列dirent结构的文件
#define DIRSIZ 14 // 目录项中文件名的最大长度

// The name field may have DIRSIZ characters and not end in a NUL
// character.
// 注意：name字段可能有DIRSIZ个字符，但不以NUL字符结尾
struct dirent {
  ushort inum; // inode编号
  char name[DIRSIZ] __attribute__((nonstring)); // 文件名
};


/*
    超级块是地图，告诉你其他块在哪里

    Inode块是文件目录，记录每个文件的元数据，也就是记录每个文件信息

    数据块是仓库，存储实际文件内容

    位图块是仓库管理员，管理仓库空间，也就是管理数据块

    日志块是安全审计，保证操作安全可靠
*/

