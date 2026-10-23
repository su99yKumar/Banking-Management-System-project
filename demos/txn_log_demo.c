
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOG_FILE "transactions_demo.log"
#define WORKER_COUNT 2
#define ROUNDS 3

/* Argument type required by semctl() on this Linux environment. */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/*
 * Change semaphore 0.
 * -1 acquires the semaphore; +1 releases it.
 * SEM_UNDO lets the kernel undo an outstanding adjustment
 * if the process terminates unexpectedly.
 */
static int semaphore_change(int semid, short amount)
{
    struct sembuf operation;

    operation.sem_num = 0;
    operation.sem_op = amount;
    operation.sem_flg = SEM_UNDO;

    while (semop(semid, &operation, 1) == -1) {
        if (errno == EINTR)
            continue;

        perror("ERROR: semop failed");
        return -1;
    }

    return 0;
}

/* Write the complete line, handling partial writes and interruptions. */
static int write_all(int fd, const char *buffer, size_t length)
{
    size_t total = 0;

    while (total < length) {
        ssize_t n = write(fd, buffer + total, length - total);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}

/*
 * Find the next transaction ID by scanning the existing log.
 * The caller must hold the semaphore while doing this and appending.
 */
static int next_transaction_id(unsigned long long *next_id)
{
    FILE *file = fopen(LOG_FILE, "r");

    if (file == NULL) {
        if (errno == ENOENT) {
            *next_id = 1;
            return 0;
        }

        perror("ERROR: Cannot read transaction log");
        return -1;
    }

    unsigned long long last_id = 0;
    char line[256];

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long long id;
        char account[7];
        char operation[16];
        long long amount;
        long long balance;
        long process_id;

        int fields = sscanf(line, "%llu %6s %15s %lld %lld %ld",
                            &id, account, operation,
                            &amount, &balance, &process_id);

        if (fields == 6 && id > last_id)
            last_id = id;
    }

    if (ferror(file)) {
        perror("ERROR: Cannot scan transaction log");
        fclose(file);
        return -1;
    }

    if (fclose(file) == EOF) {
        perror("ERROR: Cannot close transaction log");
        return -1;
    }

    if (last_id == ULLONG_MAX) {
        errno = ERANGE;
        perror("ERROR: Transaction ID limit reached");
        return -1;
    }

    *next_id = last_id + 1;
    return 0;
}

/*
 * Append one transaction while holding the shared semaphore.
 * This protects ID allocation and appending as one coordinated action.
 */
static int append_transaction(int semid,
                              const char *account_id,
                              const char *operation,
                              long long amount,
                              long long resulting_balance)
{
    if (semaphore_change(semid, -1) == -1)
        return -1;

    int result = -1;
    int log_fd = -1;
    unsigned long long transaction_id;
    char line[256];

    if (next_transaction_id(&transaction_id) == -1)
        goto cleanup;

    int length = snprintf(
        line, sizeof(line),
        "%llu %s %s %lld %lld %ld\n",
        transaction_id,
        account_id,
        operation,
        amount,
        resulting_balance,
        (long)getpid());

    if (length < 0 || (size_t)length >= sizeof(line)) {
        fprintf(stderr, "ERROR: Transaction log line is too long\n");
        goto cleanup;
    }

    /*
     * O_APPEND positions each write at the end of the log.
     * All participating writers must still follow the semaphore protocol.
     */
    log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);

    if (log_fd == -1) {
        perror("ERROR: Cannot open transaction log for append");
        goto cleanup;
    }

    if (write_all(log_fd, line, (size_t)length) == -1) {
        perror("ERROR: Cannot append transaction record");
        goto cleanup;
    }

    if (fsync(log_fd) == -1) {
        perror("ERROR: Transaction log fsync failed; outcome is uncertain");
        goto cleanup;
    }

    if (close(log_fd) == -1) {
        log_fd = -1;
        perror("ERROR: Cannot close transaction log");
        goto cleanup;
    }

    log_fd = -1;
    result = 0;

    printf("Appended transaction %llu for account %s\n",
           transaction_id, account_id);
    fflush(stdout);

cleanup:
    if (log_fd != -1 && close(log_fd) == -1) {
        perror("ERROR: Cannot close transaction log");
        result = -1;
    }

    /* Release even if an append operation encountered an error. */
    if (semaphore_change(semid, +1) == -1)
        result = -1;

    return result;
}

/* Demonstrate independent processes appending simulated deposits. */
static void worker(int semid, int worker_number)
{
    const char *account_id =
        worker_number == 1 ? "000001" : "000002";

    long long starting_balance =
        worker_number == 1 ? 10000 : 50000;

    long long amount =
        worker_number == 1 ? 2500 : 1000;

    for (int round = 1; round <= ROUNDS; round++) {
        long long resulting_balance =
            starting_balance + amount * round;

        if (append_transaction(semid, account_id,
                               "DEPOSIT", amount,
                               resulting_balance) == -1) {
            fprintf(stderr, "ERROR: Worker %d failed to log a transaction\n",
                    worker_number);
            _exit(EXIT_FAILURE);
        }
    }

    _exit(EXIT_SUCCESS);
}

/* Display only the log entries belonging to the requested account. */
static int show_passbook(int semid, const char *account_id)
{
    if (semaphore_change(semid, -1) == -1)
        return -1;

    int result = -1;
    FILE *file = fopen(LOG_FILE, "r");

    if (file == NULL) {
        perror("ERROR: Cannot open transaction log for passbook");
        goto cleanup;
    }

    printf("\nPassbook for account %s\n", account_id);
    printf("ID       Operation  Amount     Balance    PID\n");
    printf("------------------------------------------------\n");

    char line[256];
    int count = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long long id;
        char account[7];
        char operation[16];
        long long amount;
        long long balance;
        long process_id;

        int fields = sscanf(line, "%llu %6s %15s %lld %lld %ld",
                            &id, account, operation,
                            &amount, &balance, &process_id);

        if (fields != 6 || strcmp(account, account_id) != 0)
            continue;

        printf("%-8llu %-10s %lld.%02lld  %lld.%02lld  %ld\n",
               id, operation,
               amount / 100, amount % 100,
               balance / 100, balance % 100,
               process_id);

        count++;
    }

    if (ferror(file)) {
        perror("ERROR: Cannot read passbook entries");
        fclose(file);
        goto cleanup;
    }

    if (fclose(file) == EOF) {
        perror("ERROR: Cannot close passbook log");
        goto cleanup;
    }

    if (count == 0)
        printf("No transactions found for this account.\n");

    result = 0;

cleanup:
    if (semaphore_change(semid, +1) == -1)
        result = -1;

    return result;
}

int main(void)
{
    /*
     * This is a disposable demo log: reset it at the start.
     * Do not use this truncation behavior for the real banking log.
     */
    int log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (log_fd == -1) {
        perror("ERROR: Cannot initialize demo transaction log");
        return EXIT_FAILURE;
    }

    if (close(log_fd) == -1) {
        perror("ERROR: Cannot close initialized demo log");
        return EXIT_FAILURE;
    }

    /* Create one shared semaphore set visible to both forked workers. */
    int semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);

    if (semid == -1) {
        perror("ERROR: Cannot create transaction-log semaphore");
        return EXIT_FAILURE;
    }

    union semun argument;
    argument.val = 1;

    if (semctl(semid, 0, SETVAL, argument) == -1) {
        perror("ERROR: Cannot initialize transaction-log semaphore");
        semctl(semid, 0, IPC_RMID);
        return EXIT_FAILURE;
    }

    printf("Transaction-log semaphore ID: %d\n", semid);
    printf("Each child will append three simulated deposits.\n");
    fflush(stdout);

    pid_t children[WORKER_COUNT];
    int started = 0;

    for (int i = 0; i < WORKER_COUNT; i++) {
        pid_t pid = fork();

        if (pid == -1) {
            perror("ERROR: Cannot fork transaction worker");
            break;
        }

        if (pid == 0)
            worker(semid, i + 1);

        children[started++] = pid;
    }

    int exit_code =
        started == WORKER_COUNT ? EXIT_SUCCESS : EXIT_FAILURE;

    /* Wait for all workers before reading the completed demo log. */
    for (int i = 0; i < started; i++) {
        int status;
        pid_t waited;

        do {
            waited = waitpid(children[i], &status, 0);
        } while (waited == -1 && errno == EINTR);

        if (waited == -1) {
            perror("ERROR: waitpid failed");
            exit_code = EXIT_FAILURE;
        } else if (!WIFEXITED(status) ||
                   WEXITSTATUS(status) != EXIT_SUCCESS) {
            fprintf(stderr, "ERROR: A transaction worker failed\n");
            exit_code = EXIT_FAILURE;
        }
    }

    /* Show the per-account histories after the children finish. */
    if (show_passbook(semid, "000001") == -1)
        exit_code = EXIT_FAILURE;

    if (show_passbook(semid, "000002") == -1)
        exit_code = EXIT_FAILURE;

    /* Remove the kernel semaphore set after all users of it have exited. */
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("ERROR: Cannot remove transaction-log semaphore");
        exit_code = EXIT_FAILURE;
    } else {
        printf("\nTransaction-log semaphore removed.\n");
    }

    return exit_code;
}
