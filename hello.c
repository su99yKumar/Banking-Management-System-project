#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

int main(void)
{
    pid_t pid = getpid();

    printf("Hello from Banking Management System!\n");
    printf("PID (process ID): %ld\n", (long)pid);

    return 0;
}
