#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RECORD_SIZE 256
#define ID_OFFSET 0
#define ID_WIDTH 6
#define BALANCE_OFFSET 149
#define BALANCE_WIDTH 12
#define MAX_BALANCE 999999999999LL

/*
 * Read exactly len bytes at a specific file offset.
 * Return 0 on success and -1 if the read fails or is incomplete.
 */
static int pread_exact(int fd, void *buffer, size_t len, off_t offset)
{
    size_t total = 0;
    char *p = buffer;

    while (total < len) {
        ssize_t n = pread(fd, p + total, len - total,
                          offset + (off_t)total);

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
 * Write exactly len bytes at a specific file offset.
 * Handle partial writes and interrupted system calls.
 */
static int pwrite_exact(int fd, const void *buffer,
                        size_t len, off_t offset)
{
    size_t total = 0;
    const char *p = buffer;

    while (total < len) {
        ssize_t n = pwrite(fd, p + total, len - total,
                           offset + (off_t)total);

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

/* Validate that an account ID contains exactly six digits. */
static int valid_account_id(const char *id)
{
    if (strlen(id) != ID_WIDTH)
        return 0;

    for (int i = 0; i < ID_WIDTH; i++) {
        if (!isdigit((unsigned char)id[i]))
            return 0;
    }

    return 1;
}

/*
 * Convert an amount such as "25.50" to 2550 minor units.
 * Reject negative values, malformed amounts, and excess precision.
 */
static int parse_amount(const char *text, long long *amount_out)
{
    if (text == NULL || !isdigit((unsigned char)text[0]))
        return -1;

    errno = 0;
    char *end = NULL;
    long long whole = strtoll(text, &end, 10);

    if (errno == ERANGE || end == text || whole < 0)
        return -1;

    int fraction = 0;

    if (*end == '.') {
        const char *p = end + 1;

        if (!isdigit((unsigned char)p[0]))
            return -1;

        fraction = (p[0] - '0') * 10;

        if (p[1] != '\0') {
            if (!isdigit((unsigned char)p[1]) || p[2] != '\0')
                return -1;

            fraction += p[1] - '0';
        }
    } else if (*end != '\0') {
        return -1;
    }

    if (whole > MAX_BALANCE / 100)
        return -1;

    long long amount = whole * 100 + fraction;

    if (amount > MAX_BALANCE)
        return -1;

    *amount_out = amount;
    return 0;
}

/* Parse the 12-digit balance field as an integer. */
static int parse_balance(const char *record, long long *balance_out)
{
    long long value = 0;

    for (int i = 0; i < BALANCE_WIDTH; i++) {
        char c = record[BALANCE_OFFSET + i];

        if (c < '0' || c > '9') {
            fprintf(stderr,
                    "ERROR: Account record contains an invalid balance digit\n");
            return -1;
        }

        value = value * 10 + (c - '0');
    }

    *balance_out = value;
    return 0;
}

/* Print a minor-unit amount as rupees and paise. */
static void print_money(long long amount)
{
    printf("%lld.%02lld", amount / 100, amount % 100);
}

/*
 * Find an account by ID.
 * Return 1 if found, 0 if not found, and -1 on an I/O error.
 */
static int find_record(int fd, const char *id, off_t file_size,
                       off_t *record_offset, char record[RECORD_SIZE])
{
    for (off_t offset = 0; offset < file_size;
         offset += RECORD_SIZE) {

        if (pread_exact(fd, record, RECORD_SIZE, offset) == -1) {
            perror("ERROR: Cannot read account record");
            return -1;
        }

        if (memcmp(record + ID_OFFSET, id, ID_WIDTH) == 0) {
            *record_offset = offset;
            return 1;
        }
    }

    return 0;
}

/* Lock one entire 256-byte account record. */
static int lock_record(int fd, off_t offset)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = offset;
    lock.l_len = RECORD_SIZE;

    /* F_SETLKW waits until the conflicting lock becomes available. */
    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }

    return 0;
}

/* Explicitly release the record lock. */
static int unlock_record(int fd, off_t offset)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = offset;
    lock.l_len = RECORD_SIZE;

    while (fcntl(fd, F_SETLK, &lock) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr,
                "Usage:\n"
                "  %s FILE ACCOUNT_ID balance\n"
                "  %s FILE ACCOUNT_ID deposit AMOUNT\n"
                "  %s FILE ACCOUNT_ID withdraw AMOUNT\n",
                argv[0], argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const char *file_path = argv[1];
    const char *account_id = argv[2];
    const char *operation = argv[3];

    if (!valid_account_id(account_id)) {
        fprintf(stderr,
                "ERROR: Account ID must contain exactly six digits\n");
        return EXIT_FAILURE;
    }

    int is_balance = strcmp(operation, "balance") == 0;
    int is_deposit = strcmp(operation, "deposit") == 0;
    int is_withdraw = strcmp(operation, "withdraw") == 0;

    if ((!is_balance && !is_deposit && !is_withdraw) ||
        (is_balance && argc != 4) ||
        (!is_balance && argc != 5)) {
        fprintf(stderr, "ERROR: Invalid operation or argument count\n");
        return EXIT_FAILURE;
    }

    long long amount = 0;

    if (!is_balance) {
        if (parse_amount(argv[4], &amount) == -1 || amount <= 0) {
            fprintf(stderr,
                    "ERROR: Amount must be positive with at most two decimal places\n");
            return EXIT_FAILURE;
        }
    }

    /* Open existing data only; never silently create a missing bank file. */
    int fd = open(file_path, O_RDWR);

    if (fd == -1) {
        perror("ERROR: Cannot open account file");
        return EXIT_FAILURE;
    }

    int exit_code = EXIT_FAILURE;
    int locked = 0;
    struct stat info;
    char record[RECORD_SIZE];
    off_t record_offset = 0;
    long long current_balance = 0;
    long long new_balance = 0;

    if (fstat(fd, &info) == -1) {
        perror("ERROR: Cannot inspect account file");
        goto cleanup;
    }

    if (info.st_size == 0 || info.st_size % RECORD_SIZE != 0) {
        fprintf(stderr,
                "ERROR: Account file size is not a non-zero multiple of %d bytes\n",
                RECORD_SIZE);
        goto cleanup;
    }

    int found = find_record(fd, account_id, info.st_size,
                            &record_offset, record);

    if (found == -1)
        goto cleanup;

    if (found == 0) {
        fprintf(stderr, "ERROR: Account ID %s was not found\n",
                account_id);
        goto cleanup;
    }

    /* Protect the complete read-modify-write operation. */
    if (lock_record(fd, record_offset) == -1) {
        perror("ERROR: Cannot acquire account record lock");
        goto cleanup;
    }

    locked = 1;

    /*
     * Re-read after locking. Do not base a transaction on an
     * account record read before acquiring the lock.
     */
    if (pread_exact(fd, record, RECORD_SIZE, record_offset) == -1) {
        perror("ERROR: Cannot reread locked account record");
        goto cleanup;
    }

    if (memcmp(record + ID_OFFSET, account_id, ID_WIDTH) != 0) {
        fprintf(stderr, "ERROR: Account ID changed unexpectedly\n");
        goto cleanup;
    }

    if (parse_balance(record, &current_balance) == -1)
        goto cleanup;

    if (is_balance) {
        printf("Account %s balance: ", account_id);
        print_money(current_balance);
        putchar('\n');
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    if (is_deposit) {
        if (amount > MAX_BALANCE - current_balance) {
            fprintf(stderr, "ERROR: Deposit would exceed maximum balance\n");
            goto cleanup;
        }

        new_balance = current_balance + amount;
    } else {
        if (amount > current_balance) {
            fprintf(stderr, "ERROR: Insufficient funds\n");
            goto cleanup;
        }

        new_balance = current_balance - amount;
    }

    char balance_field[BALANCE_WIDTH + 1];
    int written = snprintf(balance_field, sizeof(balance_field),
                           "%012lld", new_balance);

    if (written != BALANCE_WIDTH) {
        fprintf(stderr,
                "ERROR: New balance cannot fit in the fixed-width field\n");
        goto cleanup;
    }

    /*
     * Update only the balance field. Other account fields stay intact.
     * The record lock remains held during the write and fsync.
     */
    if (pwrite_exact(fd, balance_field, BALANCE_WIDTH,
                     record_offset + BALANCE_OFFSET) == -1) {
        perror("ERROR: Cannot write updated balance");
        goto cleanup;
    }

    if (fsync(fd) == -1) {
        perror("ERROR: Balance was written but fsync failed; durability is uncertain");
        goto cleanup;
    }

    printf("%s successful. New balance: ",
           is_deposit ? "Deposit" : "Withdrawal");
    print_money(new_balance);
    putchar('\n');

    exit_code = EXIT_SUCCESS;

cleanup:
    if (locked && unlock_record(fd, record_offset) == -1) {
        perror("ERROR: Cannot release account record lock");
        exit_code = EXIT_FAILURE;
    }

    if (close(fd) == -1) {
        perror("ERROR: Cannot close account file");
        exit_code = EXIT_FAILURE;
    }

    return exit_code;
}
