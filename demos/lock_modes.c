#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOCK_FILE "lock_modes.txt"

/*
 * Try to acquire a whole-file write lock.
 * blocking = 1 waits for the lock; blocking = 0 returns immediately
 * if a conflicting lock prevents acquisition.
 */
static int acquire_lock(int fd, int blocking)
{
    struct flock lock = {0};

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0; /* From byte zero to the end of the file. */

    int command = blocking ? F_SETLKW : F_SETLK;

    while (fcntl(fd, command, &lock) == -1) {
        if (errno == EINTR && blocking) {
            continue;
        }

        if (!blocking && (errno == EACCES || errno == EAGAIN)) {
            return 1; /* Lock is busy; this is an expected result. */
        }

        perror("ERROR: Failed to acquire the write lock");
        return -1;
    }

    return 0;
}

/* Release the whole-file write lock. */
static int release_lock(int fd)
{
    struct flock lock = {0};

    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    if (fcntl(fd, F_SETLK, &lock) == -1) {
        perror("ERROR: Failed to release the write lock");
        return -1;
    }

    return 0;
}

int main(void)
{
    int fd = open(LOCK_FILE, O_RDWR | O_CREAT, 0600);

    if (fd == -1) {
        perror("ERROR: Cannot open lock_modes.txt");
        return EXIT_FAILURE;
    }

    /*
     * Acquire the lock before fork(). The child will try a conflicting
     * lock using its own newly opened file descriptor.
     */
    if (acquire_lock(fd, 0) != 0) {
        fprintf(stderr, "ERROR: Parent could not acquire initial lock\n");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Parent: acquired write lock.\n");
    fflush(stdout);

    pid_t child = fork();

    if (child == -1) {
        perror("ERROR: Could not create child process");
        release_lock(fd);
        close(fd);
        return EXIT_FAILURE;
    }

    if (child == 0) {
        int child_fd = open(LOCK_FILE, O_RDWR);

        if (child_fd == -1) {
            perror("ERROR: Child could not open lock_modes.txt");
            _exit(EXIT_FAILURE);
        }

        printf("Child: trying non-blocking lock...\n");
        fflush(stdout);

        int result = acquire_lock(child_fd, 0);

        if (result == 1) {
            printf("Child: lock is busy; returned immediately.\n");
        } else if (result == 0) {
            printf("Child: acquired the lock unexpectedly.\n");
            release_lock(child_fd);
        } else {
            close(child_fd);
            _exit(EXIT_FAILURE);
        }

        close(child_fd);
        _exit(result == 1 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    int status;
    while (waitpid(child, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }

        perror("ERROR: Parent could not wait for child");
        release_lock(fd);
        close(fd);
        return EXIT_FAILURE;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR: Child lock experiment failed\n");
        release_lock(fd);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Parent: releasing write lock.\n");

    if (release_lock(fd) == -1) {
        close(fd);
        return EXIT_FAILURE;
    }

    if (close(fd) == -1) {
        perror("ERROR: Could not close lock_modes.txt");
        return EXIT_FAILURE;
    }

    printf("SUCCESS: Non-blocking lock behavior verified.\n");
    return EXIT_SUCCESS;
}
