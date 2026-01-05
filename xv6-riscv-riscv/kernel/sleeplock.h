// Long-term locks for processes
// 进程使用的长期锁（睡眠锁）
struct sleeplock {
  // 锁是否被持有？0=未持有，1=已持有
  uint locked;       // Is the lock held?
  // 保护这个睡眠锁的自旋锁（用于内部同步）
  struct spinlock lk; // spinlock protecting this sleep lock
  
  // For debugging:
  // 锁的名称，便于调试时识别
  char *name;        // Name of lock.
  // 持有此锁的进程ID
  int pid;           // Process holding lock
};

