#define _POSIX_C_SOURCE 200809L

#include "bank_core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sem.h>

/* Linux's semctl() interface needs this union for SETVAL. */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/* Run one banking command and display the response from bank_core.c. */
static int test_command(int semid, const char *command)
{
    char response[128];

    if (bank_process_command("users_core_test.txt",
                             "000001",
                             command,
                             response,
                             sizeof(response),
                             semid) == -1) {
        fprintf(stderr, "ERROR: Could not process command: %s\n",
                command);
        return -1;
    }

    printf("%-20s -> %s\n", command, response);
    return 0;
}

int main(void)
{
    int semid;
    union semun argument;

    /* Create a private semaphore for this test process. */
    semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid == -1) {
        perror("ERROR: semget could not create test semaphore");
        return EXIT_FAILURE;
    }

    /* Initialize the semaphore to 1, meaning the lock is available. */
    argument.val = 1;
    if (semctl(semid, 0, SETVAL, argument) == -1) {
        perror("ERROR: semctl could not initialize test semaphore");
        (void)semctl(semid, 0, IPC_RMID);
        return EXIT_FAILURE;
    }

    if (test_command(semid, "BALANCE") == -1 ||
        test_command(semid, "DEPOSIT 25.00") == -1 ||
        test_command(semid, "WITHDRAW 10.00") == -1 ||
        test_command(semid, "BALANCE") == -1 ||
        test_command(semid, "WITHDRAW 999.00") == -1 ||
        test_command(semid, "BALANCE") == -1) {
        (void)semctl(semid, 0, IPC_RMID);
        return EXIT_FAILURE;
    }

    /* Remove the kernel semaphore so the test leaves no IPC resource behind. */
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("ERROR: Could not remove test semaphore");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
