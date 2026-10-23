#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define RECORD_SIZE 256
#define LOCK_FILE "lock_balance.txt"
#define DEPOSIT_AMOUNT 10

/*
 * Write every byte at the requested offset.
 * pwrite() may write fewer bytes than requested, so keep writing
 * until the whole record has been written.
 * Returns 0 on success or -1 on error.
 */
static int write_all_at(int fd, const char *buffer,
                        size_t count, off_t offset)
{
    size_t done = 0;

    while (done < count) {
        ssize_t n = pwrite(fd, buffer + done, count - done,
                           offset + (off_t)done);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("ERROR: Failed to write the account record");
            return -1;
        }

        if (n == 0) {
            fprintf(stderr,
                    "ERROR: Account-record write made no progress.\n");
            errno = EIO;
            return -1;
        }

        done += (size_t)n;
    }

    return 0;
}

/*
 * Read exactly one 256-byte record from the specified offset.
 * An early end-of-file means the record is incomplete.
 * Returns 0 on success or -1 on error.
 */
static int read_record(int fd, char *record, off_t offset)
{
    size_t done = 0;

    while (done < RECORD_SIZE) {
        ssize_t n = pread(fd, record + done, RECORD_SIZE - done,
                          offset + (off_t)done);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("ERROR: Failed to read the account record");
            return -1;
        }

        if (n == 0) {
            fprintf(stderr,
                    "ERROR: Account record is truncated at byte %lld.\n",
                    (long long)(offset + (off_t)done));
            errno = EIO;
            return -1;
        }

        done += (size_t)n;
    }

    if (record[RECORD_SIZE - 1] != '\n') {
        fprintf(stderr,
                "ERROR: Account record does not end with a newline.\n");
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/*
 * Build one fixed-width account record.
 * The first 12 bytes hold the balance as zero-padded decimal digits.
 */
static int make_record(char *record, long balance)
{
    char digits[13];

    if (balance < 0) {
        fprintf(stderr, "ERROR: Account balance cannot be negative.\n");
        return -1;
    }

    int n = snprintf(digits, sizeof(digits), "%012ld", balance);

    if (n != 12) {
        fprintf(stderr,
                "ERROR: Balance does not fit the 12-digit record field.\n");
        errno = EINVAL;
        return -1;
    }

    memset(record, ' ', RECORD_SIZE);
    memcpy(record, digits, 12);
    record[RECORD_SIZE - 1] = '\n';

    return 0;
}

/*
 * Extract the balance from a fixed-width record.
 * Returns the balance, or -1 when the record contains invalid data.
 */
static long parse_balance(const char *record)
{
    char digits[13];

    memcpy(digits, record, 12);
    digits[12] = '\0';

    for (int i = 0; i < 12; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            fprintf(stderr,
                    "ERROR: Balance field contains a non-digit.\n");
            errno = EINVAL;
            return -1;
        }
    }

    errno = 0;
    char *end = NULL;
    long balance = strtol(digits, &end, 10);

    if (errno != 0 || end == digits || *end != '\0') {
        fprintf(stderr, "ERROR: Could not parse the account balance.\n");
        return -1;
    }

    return balance;
}

/*
 * Acquire an exclusive write lock on one account record.
 *
 * F_SETLKW waits while another process holds a conflicting lock.
 * EINTR means a signal interrupted the wait, so the call is retried.
 */
static int lock_record(int fd)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_WRLCK;       /* Exclusive write lock */
    lock.l_whence = SEEK_SET;    /* Offset is measured from file start */
    lock.l_start = 0;            /* Record begins at byte zero */
    lock.l_len = RECORD_SIZE;    /* Protect exactly 256 bytes */

    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR) {
            continue;
        }

        perror("ERROR: Could not acquire the account write lock");
        return -1;
    }

    return 0;
}

/*
 * Release the record lock held by this process.
 * Returns 0 on success or -1 on error.
 */
static int unlock_record(int fd)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = RECORD_SIZE;

    if (fcntl(fd, F_SETLK, &lock) == -1) {
        perror("ERROR: Could not release the account lock");
        return -1;
    }

    return 0;
}

/*
 * Each child deposits 10 units while holding the account's write lock.
 * The lock covers reading, calculating, writing and fsync(), preventing
 * cooperating processes from overwriting each other's updates.
 */
static int perform_deposit(int process_number)
{
    int fd = open(LOCK_FILE, O_RDWR);

    if (fd == -1) {
        perror("ERROR: Cannot open lock_balance.txt");
        return EXIT_FAILURE;
    }

    printf("Process %d (PID %ld) waiting for write lock...\n",
           process_number, (long)getpid());
    fflush(stdout);

    if (lock_record(fd) == -1) {
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Process %d acquired write lock.\n", process_number);
    fflush(stdout);

    char record[RECORD_SIZE];

    if (read_record(fd, record, 0) == -1) {
        unlock_record(fd);
        close(fd);
        return EXIT_FAILURE;
    }

    long balance = parse_balance(record);

    if (balance == -1) {
        unlock_record(fd);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Process %d read balance: %ld\n", process_number, balance);
    fflush(stdout);

    /* Deliberately hold the lock longer so blocking is visible. */
    sleep(1);

    if (balance > LONG_MAX - DEPOSIT_AMOUNT) {
        fprintf(stderr, "ERROR: Deposit would overflow the balance.\n");
        unlock_record(fd);
        close(fd);
        return EXIT_FAILURE;
    }

    long updated_balance = balance + DEPOSIT_AMOUNT;

    if (make_record(record, updated_balance) == -1 ||
        write_all_at(fd, record, RECORD_SIZE, 0) == -1) {
        unlock_record(fd);
        close(fd);
        return EXIT_FAILURE;
    }

    /* Request synchronization before permitting the next deposit. */
    if (fsync(fd) == -1) {
        perror("ERROR: Could not synchronize the updated balance");
        unlock_record(fd);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Process %d wrote balance: %ld\n",
           process_number, updated_balance);
    fflush(stdout);

    if (unlock_record(fd) == -1) {
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Process %d released write lock.\n", process_number);

    if (close(fd) == -1) {
        perror("ERROR: Could not close the account file");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/*
 * Wait for a child and report whether its deposit completed successfully.
 */
static int wait_for_child(pid_t child, const char *description)
{
    int status;

    while (waitpid(child, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }

        perror("ERROR: Could not wait for a deposit process");
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR: %s did not complete successfully.\n",
                description);
        return -1;
    }

    return 0;
}

/*
 * Initialize a clean 100-unit demo account, run two concurrent deposits,
 * and display the final balance. This file is separate from users.txt.
 */
int main(void)
{
    int fd = open(LOCK_FILE, O_RDWR | O_CREAT | O_TRUNC, 0600);

    if (fd == -1) {
        perror("ERROR: Cannot create lock_balance.txt");
        return EXIT_FAILURE;
    }

    char record[RECORD_SIZE];

    if (make_record(record, 100) == -1 ||
        write_all_at(fd, record, RECORD_SIZE, 0) == -1) {
        close(fd);
        return EXIT_FAILURE;
    }

    if (fsync(fd) == -1) {
        perror("ERROR: Could not synchronize the starting balance");
        close(fd);
        return EXIT_FAILURE;
    }

    if (close(fd) == -1) {
        perror("ERROR: Could not close the initialized account");
        return EXIT_FAILURE;
    }

    printf("Starting balance: 100\n");
    fflush(stdout);

    pid_t first = fork();

    if (first == -1) {
        perror("ERROR: Could not create the first child process");
        return EXIT_FAILURE;
    }

    if (first == 0) {
        _exit(perform_deposit(1));
    }

    pid_t second = fork();

    if (second == -1) {
        perror("ERROR: Could not create the second child process");
        (void)wait_for_child(first, "First deposit");
        return EXIT_FAILURE;
    }

    if (second == 0) {
        _exit(perform_deposit(2));
    }

    int first_result = wait_for_child(first, "First deposit");
    int second_result = wait_for_child(second, "Second deposit");

    if (first_result == -1 || second_result == -1) {
        fprintf(stderr,
                "ERROR: At least one deposit failed; check the messages above.\n");
        return EXIT_FAILURE;
    }

    fd = open(LOCK_FILE, O_RDONLY);

    if (fd == -1) {
        perror("ERROR: Cannot open account to verify final balance");
        return EXIT_FAILURE;
    }

    if (read_record(fd, record, 0) == -1) {
        close(fd);
        return EXIT_FAILURE;
    }

    long final_balance = parse_balance(record);

    if (final_balance == -1) {
        close(fd);
        return EXIT_FAILURE;
    }

    if (close(fd) == -1) {
        perror("ERROR: Could not close the account after verification");
        return EXIT_FAILURE;
    }

    printf("Final balance: %ld\n", final_balance);
    printf("Expected balance: 120\n");

    if (final_balance != 120) {
        fprintf(stderr,
                "ERROR: Final balance is incorrect; investigate "
                "the locking or file operations.\n");
        return EXIT_FAILURE;
    }

    printf("SUCCESS: Both deposits were preserved.\n");
    return EXIT_SUCCESS;
}
