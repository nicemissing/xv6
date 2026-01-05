// Mutual exclusion lock.
struct spinlock {
  // 锁是否被持有，0：未持有， 1：持有
  uint locked;       // Is the lock held?

  // For debugging:
  // 锁的名字
  char *name;        // Name of lock.
  // 持有此锁的CPU指针
  struct cpu *cpu;   // The cpu holding the lock.
};

