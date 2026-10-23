#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOCK_FILE "deadlock_demo.txt"
#define RECORD_SIZE 256

/*
 * Acquire a blocking write lock on one record.
 * The process waits if another process holds a conflicting lock.
 */
static int lock_record(int fd, off_t offset)
{
    struct flock lock = {0};

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = offset;
    lock.l_len = RECORD_SIZE;

    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR) {
            continue;
        }

        perror("ERROR: Could not acquire record lock");
        return -1;
    }

    return 0;
}

/*
 * Open the shared file, lock one record, then request the other.
 * The opposite lock order is intentional: it creates a deadlock.
 */
static void run_process(int process_number, off_t first, off_t second)
{
    int fd = open(LOCK_FILE, O_RDWR);

    if (fd == -1) {
        perror("ERROR: Cannot open deadlock_demo.txt");
        _exit(EXIT_FAILURE);
    }

    printf("Process %d: requesting first lock at byte %lld\n",
           process_number, (long long)first);
    fflush(stdout);

    if (lock_record(fd, first) == -1) {
        close(fd);
        _exit(EXIT_FAILURE);
    }

    printf("Process %d: acquired first lock at byte %lld\n",
           process_number, (long long)first);
    fflush(stdout);

    /* Give the other process time to acquire its first lock. */
    sleep(1);

    printf("Process %d: requesting second lock at byte %lld\n",
           process_number, (long long)second);
    fflush(stdout);

    if (lock_record(fd, second) == -1) {
        close(fd);
        _exit(EXIT_FAILURE);
    }

    /* Both record locks were acquired successfully. */
    printf("Process %d: acquired both locks\n", process_number);
    fflush(stdout);

    close(fd);
    _exit(EXIT_SUCCESS);
}

int main(void)
{
    /*
     * Prepare a file containing two complete records so both lock ranges
     * exist. The demo does not store any banking data.
     */
    int fd = open(LOCK_FILE, O_RDWR | O_CREAT | O_TRUNC, 0600);

    if (fd == -1) {
        perror("ERROR: Cannot create deadlock_demo.txt");
        return EXIT_FAILURE;
    }

    char empty_records[RECORD_SIZE * 2] = {0};

    if (write(fd, empty_records, sizeof(empty_records)) !=
        (ssize_t)sizeof(empty_records)) {
        perror("ERROR: Could not initialize the two lock regions");
        close(fd);
        return EXIT_FAILURE;
    }

    if (close(fd) == -1) {
        perror("ERROR: Could not close deadlock_demo.txt");
        return EXIT_FAILURE;
    }

    pid_t first = fork();

    if (first == -1) {
        perror("ERROR: Could not create process A");
        return EXIT_FAILURE;
    }

    if (first == 0) {
        run_process(1, 0, RECORD_SIZE);
    }

    pid_t second = fork();

    if (second == -1) {
        perror("ERROR: Could not create process B");
        kill(first, SIGTERM);
        (void)waitpid(first, NULL, 0);
        return EXIT_FAILURE;
    }

    if (second == 0) {
        run_process(2, 0,RECORD_SIZE);
    }

    printf("Parent: both children started. Consistent lock ordering is enabled.\n");
    printf("Parent: waiting for both children to finish.\n");
    fflush(stdout);

    /*
     * Wait for the children. Once deadlocked, neither child can finish.
     * Ctrl+C terminates the foreground demonstration so you can continue.
     */
    while (waitpid(first, NULL, 0) == -1 && errno == EINTR) {
        /* Retry if a signal interrupts waitpid(). */
    }

    while (waitpid(second, NULL, 0) == -1 && errno == EINTR) {
        /* Retry if a signal interrupts waitpid(). */
    }

    return EXIT_SUCCESS;
}
