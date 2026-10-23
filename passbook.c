#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>

#define ACCOUNT_ID_WIDTH 6
#define LINE_CAPACITY 256

/* Required argument type for semctl() on this Linux environment. */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/* Verify that an account ID contains exactly six digits. */
static int valid_account_id(const char *id)
{
    if (strlen(id) != ACCOUNT_ID_WIDTH)
        return 0;

    for (int i = 0; i < ACCOUNT_ID_WIDTH; i++) {
        if (!isdigit((unsigned char)id[i]))
            return 0;
    }

    return 1;
}

/* Wait for or release the shared transaction-log semaphore. */
static int semaphore_change(int semid, short amount)
{
    struct sembuf operation;

    operation.sem_num = 0;
    operation.sem_op = amount;
    operation.sem_flg = SEM_UNDO;

    /* Retry if a signal interrupts semop(). */
    while (semop(semid, &operation, 1) == -1) {
        if (errno == EINTR)
            continue;

        return -1;
    }

    return 0;
}

/* Print integer minor currency units using two decimal places. */
static void print_money(long long amount)
{
    printf("%lld.%02lld", amount / 100, amount % 100);
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s TRANSACTION_LOG ACCOUNT_ID SEMAPHORE_ID\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const char *log_path = argv[1];
    const char *account_id = argv[2];

    if (!valid_account_id(account_id)) {
        fprintf(stderr,
                "ERROR: Account ID must contain exactly six digits\n");
        return EXIT_FAILURE;
    }

    /* Validate the semaphore ID supplied on the command line. */
    errno = 0;
    char *end = NULL;
    long parsed_id = strtol(argv[3], &end, 10);

    if (errno == ERANGE || end == argv[3] ||
        *end != '\0' || parsed_id < 0 || parsed_id > INT_MAX) {
        fprintf(stderr, "ERROR: Invalid semaphore ID\n");
        return EXIT_FAILURE;
    }

    int semid = (int)parsed_id;

    /*
     * Acquire the SAME semaphore used by the server's log writer.
     * If a writer holds it, this call waits.
     */
    if (semaphore_change(semid, -1) == -1) {
        perror("ERROR: Cannot acquire transaction-log semaphore");
        return EXIT_FAILURE;
    }

    int exit_code = EXIT_FAILURE;
    FILE *file = fopen(log_path, "r");

    if (file == NULL) {
        perror("ERROR: Cannot open transaction log");
        goto cleanup;
    }

    printf("\nPassbook for account %s\n", account_id);
    printf("%-8s %-10s %-12s %-18s %-8s\n",
           "Txn ID", "Operation", "Amount",
           "Resulting Balance", "PID");
    printf("----------------------------------------------------------------\n");

    char line[LINE_CAPACITY];
    unsigned long line_number = 0;
    unsigned long transactions_found = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        size_t length = strlen(line);

        if (length == 0 || line[length - 1] != '\n') {
            fprintf(stderr,
                    "ERROR: Log line %lu is too long or incomplete\n",
                    line_number);
            goto close_file;
        }

        unsigned long long transaction_id;
        char logged_account[ACCOUNT_ID_WIDTH + 1];
        char operation[16];
        long long amount;
        long long balance;
        long process_id;
        char extra[2];

        int fields = sscanf(
            line,
            "%llu %6s %15s %lld %lld %ld %1s",
            &transaction_id,
            logged_account,
            operation,
            &amount,
            &balance,
            &process_id,
            extra);

        if (fields != 6 ||
            !valid_account_id(logged_account) ||
            (strcmp(operation, "DEPOSIT") != 0 &&
             strcmp(operation, "WITHDRAW") != 0) ||
            amount <= 0 || balance < 0 || process_id < 0) {
            fprintf(stderr,
                    "ERROR: Malformed transaction on line %lu\n",
                    line_number);
            goto close_file;
        }

        /* Show only records belonging to this account. */
        if (strcmp(logged_account, account_id) != 0)
            continue;

        printf("%-8llu %-10s ", transaction_id, operation);
        print_money(amount);
        printf("       ");
        print_money(balance);
        printf("             %ld\n", process_id);

        transactions_found++;
    }

    if (ferror(file)) {
        perror("ERROR: Failed while reading transaction log");
        goto close_file;
    }

    if (transactions_found == 0)
        printf("No transactions found for this account.\n");

    printf("\nTransactions found: %lu\n", transactions_found);
    exit_code = EXIT_SUCCESS;

close_file:
    if (fclose(file) == EOF) {
        perror("ERROR: Cannot close transaction log");
        exit_code = EXIT_FAILURE;
    }

cleanup:
    /* Release the semaphore on success and error paths alike. */
    if (semaphore_change(semid, +1) == -1) {
        perror("ERROR: Cannot release transaction-log semaphore");
        exit_code = EXIT_FAILURE;
    }

    return exit_code;
}
