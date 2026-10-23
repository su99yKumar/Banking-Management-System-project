#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Some Linux environments require the application to define union semun
 * for semctl(). The application supplies the value used by SETVAL.
 */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/* Change semaphore 0 by the requested amount. */
static int semaphore_change(int semid, short amount)
{
    struct sembuf operation;

    operation.sem_num = 0;
    operation.sem_op = amount;
    operation.sem_flg = SEM_UNDO;

    /* semop() may be interrupted by a signal, so retry EINTR. */
    while (semop(semid, &operation, 1) == -1) {
        if (errno == EINTR)
            continue;

        perror("ERROR: semop failed");
        return -1;
    }

    return 0;
}

/* Enter, demonstrate, and leave the protected critical section. */
static void worker(int semid, int worker_number)
{
    for (int round = 1; round <= 3; round++) {
        /* -1 acquires the semaphore; wait if its value is zero. */
        if (semaphore_change(semid, -1) == -1)
            _exit(EXIT_FAILURE);

        printf("Child %d (PID %ld): ENTER round %d\n",
               worker_number, (long)getpid(), round);
        fflush(stdout);

        /* Hold the semaphore briefly so serialization is visible. */
        sleep(1);

        printf("Child %d (PID %ld): EXIT  round %d\n",
               worker_number, (long)getpid(), round);
        fflush(stdout);

        /* +1 releases the semaphore for another process. */
        if (semaphore_change(semid, +1) == -1)
            _exit(EXIT_FAILURE);
    }

    _exit(EXIT_SUCCESS);
}

int main(void)
{
    /* IPC_PRIVATE creates a new semaphore set for this demonstration. */
    int semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);

    if (semid == -1) {
        perror("ERROR: Cannot create semaphore");
        return EXIT_FAILURE;
    }

    /* Initialize semaphore 0 to 1: one process may enter at a time. */
    union semun argument;
    argument.val = 1;

    if (semctl(semid, 0, SETVAL, argument) == -1) {
        perror("ERROR: Cannot initialize semaphore");
        semctl(semid, 0, IPC_RMID);
        return EXIT_FAILURE;
    }

    printf("Created semaphore set: %d\n", semid);
    printf("Initial semaphore value: 1\n");
    fflush(stdout);

    pid_t children[2];
    int started = 0;

    for (int i = 0; i < 2; i++) {
        pid_t pid = fork();

        if (pid == -1) {
            perror("ERROR: Cannot fork worker");
            break;
        }

        if (pid == 0)
            worker(semid, i + 1);

        children[started++] = pid;
    }

    int exit_code = started == 2 ? EXIT_SUCCESS : EXIT_FAILURE;

    /* Collect each child so the demo does not leave zombies. */
    for (int i = 0; i < started; i++) {
        int status;
        pid_t result;

        do {
            result = waitpid(children[i], &status, 0);
        } while (result == -1 && errno == EINTR);

        if (result == -1) {
            perror("ERROR: waitpid failed");
            exit_code = EXIT_FAILURE;
        } else if (!WIFEXITED(status) ||
                   WEXITSTATUS(status) != EXIT_SUCCESS) {
            fprintf(stderr, "ERROR: A worker failed\n");
            exit_code = EXIT_FAILURE;
        }
    }

    /* Remove the kernel semaphore set when the demonstration ends. */
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("ERROR: Cannot remove semaphore set");
        exit_code = EXIT_FAILURE;
    } else {
        printf("Semaphore set removed.\n");
    }

    return exit_code;
}
