
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOAN_RECORD_SIZE 128
#define LOAN_ID_OFFSET 0
#define ACCOUNT_ID_OFFSET 7
#define AMOUNT_OFFSET 14
#define TERM_OFFSET 27
#define STATUS_OFFSET 31
#define APPROVED_OFFSET 33
#define OUTSTANDING_OFFSET 46
#define ID_WIDTH 6
#define AMOUNT_WIDTH 12
#define TERM_WIDTH 3
#define MAX_AMOUNT 999999999999LL
#define MAX_TERM_MONTHS 360

/* Read or write exactly len bytes at an explicit file offset. */
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

/* Validate a six-digit account ID. */
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

/* Convert a positive amount such as 5000.00 into minor units. */
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

    if (whole > MAX_AMOUNT / 100)
        return -1;

    long long amount = whole * 100 + fraction;

    if (amount <= 0 || amount > MAX_AMOUNT)
        return -1;

    *amount_out = amount;
    return 0;
}

/*
 * Lock from the beginning of the loan file to EOF.
 * length 0 means "from this offset to the end of the file."
 */
static int lock_file(int fd, short lock_type)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    /* Wait until the conflicting loan-file lock becomes available. */
    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }

    return 0;
}

/* Release the whole-file lock explicitly. */
static int unlock_file(int fd)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    while (fcntl(fd, F_SETLK, &lock) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }

    return 0;
}

/* Format minor units as a currency amount with two decimal places. */
static void print_money(long long amount)
{
    printf("%lld.%02lld", amount / 100, amount % 100);
}

/*
 * Create a new application record and append it while holding
 * the whole-file write lock. The file size determines the next ID.
 */
static int apply_for_loan(const char *path, const char *account_id,
                          long long requested_amount, int term_months)
{
    int fd = open(path, O_RDWR | O_CREAT, 0600);

    if (fd == -1) {
        perror("ERROR: Cannot open loan file");
        return -1;
    }

    int locked = 0;
    int result = -1;
    struct stat info;
    char record[LOAN_RECORD_SIZE];

    if (lock_file(fd, F_WRLCK) == -1) {
        perror("ERROR: Cannot lock loan file for application");
        goto cleanup;
    }

    locked = 1;

    if (fstat(fd, &info) == -1) {
        perror("ERROR: Cannot inspect loan file");
        goto cleanup;
    }

    if (info.st_size % LOAN_RECORD_SIZE != 0) {
        fprintf(stderr, "ERROR: Loan file has an incomplete record\n");
        goto cleanup;
    }

    unsigned long long count =
        (unsigned long long)(info.st_size / LOAN_RECORD_SIZE);

    if (count >= 999999) {
        fprintf(stderr, "ERROR: Loan ID limit reached\n");
        goto cleanup;
    }

    unsigned long long loan_id = count + 1;

    /* Fill unused bytes with spaces, then set required fixed-width fields. */
    memset(record, ' ', sizeof(record));
    record[6] = ' ';
    record[13] = ' ';
    record[26] = ' ';
    record[30] = ' ';
    record[32] = ' ';
    record[45] = ' ';
    record[LOAN_RECORD_SIZE - 1] = '\n';

    char id_field[ID_WIDTH + 1];
    char amount_field[AMOUNT_WIDTH + 1];
    char term_field[TERM_WIDTH + 1];
    char zero_amount[AMOUNT_WIDTH + 1];

    snprintf(id_field, sizeof(id_field), "%06llu", loan_id);
    snprintf(amount_field, sizeof(amount_field), "%012lld",
             requested_amount);
    snprintf(term_field, sizeof(term_field), "%03d", term_months);
    memset(zero_amount, '0', AMOUNT_WIDTH);
    zero_amount[AMOUNT_WIDTH] = '\0';

    memcpy(record + LOAN_ID_OFFSET, id_field, ID_WIDTH);
    memcpy(record + ACCOUNT_ID_OFFSET, account_id, ID_WIDTH);
    memcpy(record + AMOUNT_OFFSET, amount_field, AMOUNT_WIDTH);
    memcpy(record + TERM_OFFSET, term_field, TERM_WIDTH);

    record[STATUS_OFFSET] = 'P';

    /* Amounts are zero until a manager approves the application. */
    memcpy(record + APPROVED_OFFSET, zero_amount, AMOUNT_WIDTH);
    memcpy(record + OUTSTANDING_OFFSET, zero_amount, AMOUNT_WIDTH);

    /* Append after the last complete 128-byte record. */
    if (pwrite_exact(fd, record, sizeof(record), info.st_size) == -1) {
        perror("ERROR: Cannot write loan application");
        goto cleanup;
    }

    if (fsync(fd) == -1) {
        perror("ERROR: Loan application was written but fsync failed; durability is uncertain");
        goto cleanup;
    }

    printf("Loan application created successfully.\n");
    printf("Loan ID: %06llu\n", loan_id);
    printf("Account ID: %s\n", account_id);
    printf("Requested amount: ");
    print_money(requested_amount);
    printf("\nTerm: %d months\nStatus: PENDING\n", term_months);

    result = 0;

cleanup:
    if (locked && unlock_file(fd) == -1) {
        perror("ERROR: Cannot unlock loan file");
        result = -1;
    }

    if (close(fd) == -1) {
        perror("ERROR: Cannot close loan file");
        result = -1;
    }

    return result;
}

/* Read the file under a shared lock and display this account's applications. */
static int show_loan_status(const char *path, const char *account_id)
{
    int fd = open(path, O_RDONLY);

    if (fd == -1) {
        perror("ERROR: Cannot open loan file; submit an application first");
        return -1;
    }

    int locked = 0;
    int result = -1;
    struct stat info;
    unsigned int found = 0;
    char record[LOAN_RECORD_SIZE];

    if (lock_file(fd, F_RDLCK) == -1) {
        perror("ERROR: Cannot lock loan file for reading");
        goto cleanup;
    }

    locked = 1;

    if (fstat(fd, &info) == -1) {
        perror("ERROR: Cannot inspect loan file");
        goto cleanup;
    }

    if (info.st_size % LOAN_RECORD_SIZE != 0) {
        fprintf(stderr, "ERROR: Loan file has an incomplete record\n");
        goto cleanup;
    }

    printf("\nLoan applications for account %s\n", account_id);
    printf("Loan ID  Requested Amount  Term  Status\n");
    printf("---------------------------------------\n");

    for (off_t offset = 0; offset < info.st_size;
         offset += LOAN_RECORD_SIZE) {
        if (pread_exact(fd, record, sizeof(record), offset) == -1) {
            perror("ERROR: Cannot read loan record");
            goto cleanup;
        }

        if (record[LOAN_RECORD_SIZE - 1] != '\n') {
            fprintf(stderr, "ERROR: Invalid loan record terminator\n");
            goto cleanup;
        }

        if (memcmp(record + ACCOUNT_ID_OFFSET,
                   account_id, ID_WIDTH) != 0) {
            continue;
        }

        char id_field[ID_WIDTH + 1];
        char amount_field[AMOUNT_WIDTH + 1];
        char term_field[TERM_WIDTH + 1];

        memcpy(id_field, record + LOAN_ID_OFFSET, ID_WIDTH);
        id_field[ID_WIDTH] = '\0';

        memcpy(amount_field, record + AMOUNT_OFFSET, AMOUNT_WIDTH);
        amount_field[AMOUNT_WIDTH] = '\0';

        memcpy(term_field, record + TERM_OFFSET, TERM_WIDTH);
        term_field[TERM_WIDTH] = '\0';

        char *end = NULL;
        errno = 0;
        unsigned long long loan_id = strtoull(id_field, &end, 10);

        if (errno == ERANGE || end == id_field || *end != '\0') {
            fprintf(stderr, "ERROR: Invalid loan ID field\n");
            goto cleanup;
        }

        errno = 0;
        long long amount = strtoll(amount_field, &end, 10);

        if (errno == ERANGE || end == amount_field || *end != '\0' ||
            amount <= 0) {
            fprintf(stderr, "ERROR: Invalid requested amount field\n");
            goto cleanup;
        }

        errno = 0;
        long term = strtol(term_field, &end, 10);

        if (errno == ERANGE || end == term_field || *end != '\0' ||
            term < 1 || term > MAX_TERM_MONTHS) {
            fprintf(stderr, "ERROR: Invalid loan term field\n");
            goto cleanup;
        }

        const char *status_text;

        switch (record[STATUS_OFFSET]) {
        case 'P': status_text = "PENDING"; break;
        case 'A': status_text = "APPROVED"; break;
        case 'R': status_text = "REJECTED"; break;
        default:
            fprintf(stderr, "ERROR: Unknown loan status code\n");
            goto cleanup;
        }

        printf("%06llu   ", loan_id);
        print_money(amount);
        printf("           %3ld   %s\n", term, status_text);
        found++;
    }

    if (found == 0)
        printf("No loan applications found for this account.\n");

    printf("Applications found: %u\n", found);
    result = 0;

cleanup:
    if (locked && unlock_file(fd) == -1) {
        perror("ERROR: Cannot unlock loan file");
        result = -1;
    }

    if (close(fd) == -1) {
        perror("ERROR: Cannot close loan file");
        result = -1;
    }

    return result;
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
                "Usage:\n"
                "  %s FILE apply ACCOUNT_ID AMOUNT TERM_MONTHS\n"
                "  %s FILE status ACCOUNT_ID\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argv[1];
    const char *command = argv[2];
    const char *account_id = argv[3];

    if (!valid_account_id(account_id)) {
        fprintf(stderr,
                "ERROR: Account ID must contain exactly six digits\n");
        return EXIT_FAILURE;
    }

    if (strcmp(command, "apply") == 0) {
        if (argc != 6) {
            fprintf(stderr,
                    "ERROR: apply requires AMOUNT and TERM_MONTHS\n");
            return EXIT_FAILURE;
        }

        long long amount;

        if (parse_amount(argv[4], &amount) == -1) {
            fprintf(stderr,
                    "ERROR: Amount must be positive, valid, and have at most two decimal places\n");
            return EXIT_FAILURE;
        }

        errno = 0;
        char *end = NULL;
        long term = strtol(argv[5], &end, 10);

        if (errno == ERANGE || end == argv[5] || *end != '\0' ||
            term < 1 || term > MAX_TERM_MONTHS) {
            fprintf(stderr,
                    "ERROR: Term must be between 1 and %d months\n",
                    MAX_TERM_MONTHS);
            return EXIT_FAILURE;
        }

        return apply_for_loan(path, account_id, amount, (int)term) == 0
                   ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(command, "status") == 0) {
        if (argc != 4) {
            fprintf(stderr, "ERROR: status requires only an account ID\n");
            return EXIT_FAILURE;
        }

        return show_loan_status(path, account_id) == 0
                   ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    fprintf(stderr, "ERROR: Unknown command '%s'\n", command);
    return EXIT_FAILURE;
}
