#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char **argv)
{
    // 创建两个管道：pp2c 用于父进程到子进程的通信，pc2p 用于子进程到父进程的通信
    int pp2c[2], pc2p[2];
    pipe(pp2c);
    pipe(pc2p);

    if(fork()!=0)
    {
        // 父进程写信号
        write(pp2c[1], ".",1);
        close(pp2c[1]);

        char buf;
        // 父进程从子进程读取一个字符
        read(pc2p[0], &buf, 1);
        printf("%d: received pong\n", getpid());
        wait(0);
    }
    else
    {
        char buf;
        // 子进程读取信号，如果读取不到就阻塞
        read(pp2c[0], &buf, 1);
        printf("%d: received ping\n", getpid());

        // 子进程向父进程发送一个字符
        write(pc2p[1], ".",1);
        close(pc2p[1]);
    }

    // 关闭管道的读端
    close(pp2c[0]);
    close(pc2p[0]);

    exit(0);
}
