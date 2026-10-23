
#define _POSIX_C_SOURCE 200809L

#include "loan_core.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOAN_RECORD_SIZE 128
#define ID_WIDTH 6
#define ID_OFFSET 0
#define ACCOUNT_OFFSET 7
#define AMOUNT_OFFSET 14
#define AMOUNT_WIDTH 12
#define TERM_OFFSET 27
#define TERM_WIDTH 3
#define STATUS_OFFSET 31
#define APPROVED_AMOUNT_OFFSET 33
#define OUTSTANDING_OFFSET 46
#define MAX_AMOUNT 999999999999LL
#define MAX_TERM_MONTHS 360

/* Safely format a response without overflowing the caller's buffer. */
static int set_response(char *response, size_t capacity,
                        const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(response, capacity, format, args);
    va_end(args);

    if (n < 0 || (size_t)n >= capacity) {
        errno = ENOSPC;
        return -1;
    }

    return 0;
}

/* Append text to a response while checking the remaining capacity. */
static int append_response(char *response, size_t capacity,
                           size_t *used, const char *format, ...)
{
    if (*used >= capacity) {
        errno = ENOSPC;
        return -1;
    }

    va_list args;
    va_start(args, format);
    int n = vsnprintf(response + *used, capacity - *used,
                      format, args);
    va_end(args);

    if (n < 0 || (size_t)n >= capacity - *used) {
        errno = ENOSPC;
        return -1;
    }

    *used += (size_t)n;
    return 0;
}

/* Accept exactly six decimal digits as an account or loan ID. */
static int valid_six_digits(const char *text)
{
    if (text == NULL || strlen(text) != ID_WIDTH)
        return 0;

    for (int i = 0; i < ID_WIDTH; i++) {
        if (!isdigit((unsigned char)text[i]))
            return 0;
    }

    return 1;
}

/* Convert a positive decimal currency amount to minor units (cents). */
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

/* Read exactly len bytes from a file offset, retrying interrupted reads. */
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

/* Write exactly len bytes at a file offset, retrying interrupted writes. */
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

/* Lock the entire file; l_len = 0 means from the start to EOF. */
static int loan_file_lock(int fd, short type)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }

    return 0;
}

/* Explicitly release the loan-file lock. */
static int loan_file_unlock(int fd)
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

/* Format minor units as a decimal currency value. */
static void format_money(char *buffer, size_t capacity, long long amount)
{
    (void)snprintf(buffer, capacity, "%lld.%02lld",
                   amount / 100, amount % 100);
}

/* Convert a fixed-width numeric field to a non-negative integer. */
static int parse_fixed_amount(const char *field, long long *value)
{
    char text[AMOUNT_WIDTH + 1];
    memcpy(text, field, AMOUNT_WIDTH);
    text[AMOUNT_WIDTH] = '\0';

    for (int i = 0; i < AMOUNT_WIDTH; i++) {
        if (!isdigit((unsigned char)text[i]))
            return -1;
    }

    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(text, &end, 10);

    if (errno == ERANGE || end == text || *end != '\0' || parsed < 0)
        return -1;

    *value = parsed;
    return 0;
}

/* Parse the six-digit loan ID stored at the beginning of a record. */
static int parse_record_id(const char *record, unsigned long long *id)
{
    char text[ID_WIDTH + 1];
    memcpy(text, record + ID_OFFSET, ID_WIDTH);
    text[ID_WIDTH] = '\0';

    if (!valid_six_digits(text))
        return -1;

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);

    if (errno == ERANGE || end == text || *end != '\0')
        return -1;

    *id = parsed;
    return 0;
}

/*
 * Submit a loan application.
 * The write lock protects record numbering and appending.
 */
static int apply_for_loan(const char *path, const char *account_id,
                          long long amount, int term_months,
                          char *response, size_t response_capacity)
{
    int fd = open(path, O_RDWR | O_CREAT, 0600);

    if (fd == -1) {
        perror("ERROR: Cannot open loan application file");
        return set_response(response, response_capacity,
                            "ERROR LOAN_FILE_UNAVAILABLE");
    }

    int locked = 0;
    int result = -1;
    struct stat info;
    char record[LOAN_RECORD_SIZE];

    if (loan_file_lock(fd, F_WRLCK) == -1) {
        perror("ERROR: Cannot lock loan file for writing");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_FILE_LOCK_FAILED");
        goto cleanup;
    }

    locked = 1;

    if (fstat(fd, &info) == -1) {
        perror("ERROR: Cannot inspect loan file");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_FILE_ERROR");
        goto cleanup;
    }

    if (info.st_size % LOAN_RECORD_SIZE != 0) {
        (void)set_response(response, response_capacity,
                           "ERROR CORRUPT_LOAN_FILE");
        goto cleanup;
    }

    unsigned long long count =
        (unsigned long long)(info.st_size / LOAN_RECORD_SIZE);

    if (count >= 999999) {
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_ID_LIMIT_REACHED");
        goto cleanup;
    }

    unsigned long long loan_id = count + 1;

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
    snprintf(amount_field, sizeof(amount_field), "%012lld", amount);
    snprintf(term_field, sizeof(term_field), "%03d", term_months);

    memset(zero_amount, '0', AMOUNT_WIDTH);
    zero_amount[AMOUNT_WIDTH] = '\0';

    memcpy(record + ID_OFFSET, id_field, ID_WIDTH);
    memcpy(record + ACCOUNT_OFFSET, account_id, ID_WIDTH);
    memcpy(record + AMOUNT_OFFSET, amount_field, AMOUNT_WIDTH);
    memcpy(record + TERM_OFFSET, term_field, TERM_WIDTH);

    record[STATUS_OFFSET] = 'P';
    memcpy(record + APPROVED_AMOUNT_OFFSET, zero_amount, AMOUNT_WIDTH);
    memcpy(record + OUTSTANDING_OFFSET, zero_amount, AMOUNT_WIDTH);

    if (pwrite_exact(fd, record, sizeof(record), info.st_size) == -1) {
        perror("ERROR: Cannot write loan application");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_WRITE_FAILED");
        goto cleanup;
    }

    if (fsync(fd) == -1) {
        perror("ERROR: Loan record written but durability is uncertain");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_DURABILITY_UNCERTAIN");
        goto cleanup;
    }

    char money[32];
    format_money(money, sizeof(money), amount);

    if (set_response(response, response_capacity,
                     "LOAN_APPLIED ID=%06llu AMOUNT=%s TERM=%d STATUS=PENDING",
                     loan_id, money, term_months) == -1)
        goto cleanup;

    result = 0;

cleanup:
    if (locked && loan_file_unlock(fd) == -1) {
        perror("ERROR: Cannot unlock loan file");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_UNLOCK_FAILED");
        result = -1;
    }

    if (close(fd) == -1) {
        perror("ERROR: Cannot close loan file");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_CLOSE_FAILED");
        result = -1;
    }

    return result == 0 ? 0 : (response[0] != '\0' ? 0 : -1);
}

/* Show loan applications belonging only to the authenticated account. */
static int loan_status(const char *path, const char *account_id,
                       char *response, size_t response_capacity)
{
    int fd = open(path, O_RDONLY);

    if (fd == -1) {
        if (errno == ENOENT)
            return set_response(response, response_capacity,
                                "LOAN_STATUS\nNO_APPLICATIONS");

        perror("ERROR: Cannot open loan file for reading");
        return set_response(response, response_capacity,
                            "ERROR LOAN_FILE_UNAVAILABLE");
    }

    int locked = 0;
    int success = 0;
    struct stat info;
    char record[LOAN_RECORD_SIZE];
    size_t used = 0;
    unsigned int found = 0;

    if (loan_file_lock(fd, F_RDLCK) == -1) {
        perror("ERROR: Cannot lock loan file for reading");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_FILE_LOCK_FAILED");
        goto cleanup;
    }

    locked = 1;

    if (fstat(fd, &info) == -1) {
        perror("ERROR: Cannot inspect loan file");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_FILE_ERROR");
        goto cleanup;
    }

    if (info.st_size % LOAN_RECORD_SIZE != 0) {
        (void)set_response(response, response_capacity,
                           "ERROR CORRUPT_LOAN_FILE");
        goto cleanup;
    }

    if (set_response(response, response_capacity, "LOAN_STATUS") == -1)
        goto cleanup;

    used = strlen(response);

    for (off_t offset = 0; offset < info.st_size;
         offset += LOAN_RECORD_SIZE) {
        if (pread_exact(fd, record, sizeof(record), offset) == -1) {
            perror("ERROR: Cannot read loan record");
            (void)set_response(response, response_capacity,
                               "ERROR LOAN_READ_FAILED");
            goto cleanup;
        }

        if (record[LOAN_RECORD_SIZE - 1] != '\n') {
            (void)set_response(response, response_capacity,
                               "ERROR CORRUPT_LOAN_RECORD");
            goto cleanup;
        }

        if (memcmp(record + ACCOUNT_OFFSET, account_id, ID_WIDTH) != 0)
            continue;

        unsigned long long id;
        long long amount;

        if (parse_record_id(record, &id) == -1 ||
            parse_fixed_amount(record + AMOUNT_OFFSET, &amount) == -1 ||
            amount <= 0) {
            (void)set_response(response, response_capacity,
                               "ERROR INVALID_LOAN_RECORD");
            goto cleanup;
        }

        char term_field[TERM_WIDTH + 1];
        memcpy(term_field, record + TERM_OFFSET, TERM_WIDTH);
        term_field[TERM_WIDTH] = '\0';

        char *end = NULL;
        errno = 0;
        long term = strtol(term_field, &end, 10);

        if (errno == ERANGE || end == term_field || *end != '\0' ||
            term < 1 || term > MAX_TERM_MONTHS) {
            (void)set_response(response, response_capacity,
                               "ERROR INVALID_LOAN_TERM_FIELD");
            goto cleanup;
        }

        const char *status;

        switch (record[STATUS_OFFSET]) {
        case 'P': status = "PENDING"; break;
        case 'A': status = "APPROVED"; break;
        case 'R': status = "REJECTED"; break;
        default:
            (void)set_response(response, response_capacity,
                               "ERROR INVALID_LOAN_STATUS");
            goto cleanup;
        }

        if (append_response(response, response_capacity, &used,
                            "\nID=%06llu AMOUNT=%lld.%02lld TERM=%ld STATUS=%s",
                            id, amount / 100, amount % 100,
                            term, status) == -1) {
            (void)set_response(response, response_capacity,
                               "ERROR TOO_MANY_LOAN_APPLICATIONS");
            goto cleanup;
        }

        found++;
    }

    if (found == 0 &&
        append_response(response, response_capacity, &used,
                        "\nNO_APPLICATIONS") == -1) {
        (void)set_response(response, response_capacity,
                           "ERROR RESPONSE_TOO_LARGE");
        goto cleanup;
    }

    success = 1;

cleanup:
    if (locked && loan_file_unlock(fd) == -1) {
        perror("ERROR: Cannot unlock loan file");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_UNLOCK_FAILED");
        success = 0;
    }

    if (close(fd) == -1) {
        perror("ERROR: Cannot close loan file");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_CLOSE_FAILED");
        success = 0;
    }

    return success || response[0] != '\0' ? 0 : -1;
}

/*
 * Decide a pending loan.
 * Approval stores the approved amount and initializes outstanding balance
 * to that amount. Rejection leaves both amounts at zero.
 */
static int decide_loan(const char *path, const char *loan_id_text,
                       int approve, long long approved_amount,
                       char *response, size_t response_capacity)
{
    if (!valid_six_digits(loan_id_text))
        return set_response(response, response_capacity,
                            "ERROR INVALID_LOAN_ID");

    int fd = open(path, O_RDWR);

    if (fd == -1) {
        if (errno == ENOENT)
            return set_response(response, response_capacity,
                                "ERROR LOAN_NOT_FOUND");

        perror("ERROR: Cannot open loan file for decision");
        return set_response(response, response_capacity,
                            "ERROR LOAN_FILE_UNAVAILABLE");
    }

    int locked = 0;
    int result = -1;
    struct stat info;
    char record[LOAN_RECORD_SIZE];
    unsigned long long wanted_id = strtoull(loan_id_text, NULL, 10);

    if (loan_file_lock(fd, F_WRLCK) == -1) {
        perror("ERROR: Cannot lock loan file for decision");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_FILE_LOCK_FAILED");
        goto cleanup;
    }

    locked = 1;

    if (fstat(fd, &info) == -1) {
        perror("ERROR: Cannot inspect loan file");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_FILE_ERROR");
        goto cleanup;
    }

    if (info.st_size % LOAN_RECORD_SIZE != 0) {
        (void)set_response(response, response_capacity,
                           "ERROR CORRUPT_LOAN_FILE");
        goto cleanup;
    }

    int found = 0;

    for (off_t offset = 0; offset < info.st_size;
         offset += LOAN_RECORD_SIZE) {
        if (pread_exact(fd, record, sizeof(record), offset) == -1) {
            perror("ERROR: Cannot read loan record");
            (void)set_response(response, response_capacity,
                               "ERROR LOAN_READ_FAILED");
            goto cleanup;
        }

        if (record[LOAN_RECORD_SIZE - 1] != '\n') {
            (void)set_response(response, response_capacity,
                               "ERROR CORRUPT_LOAN_RECORD");
            goto cleanup;
        }

        unsigned long long current_id;

        if (parse_record_id(record, &current_id) == -1) {
            (void)set_response(response, response_capacity,
                               "ERROR INVALID_LOAN_ID_FIELD");
            goto cleanup;
        }

        if (current_id != wanted_id)
            continue;

        found = 1;

        if (record[STATUS_OFFSET] != 'P') {
            (void)set_response(response, response_capacity,
                               "ERROR LOAN_ALREADY_DECIDED");
            goto cleanup;
        }

        long long requested_amount;

        if (parse_fixed_amount(record + AMOUNT_OFFSET,
                               &requested_amount) == -1 ||
            requested_amount <= 0) {
            (void)set_response(response, response_capacity,
                               "ERROR INVALID_LOAN_AMOUNT_FIELD");
            goto cleanup;
        }

        if (approve && approved_amount > requested_amount) {
            (void)set_response(response, response_capacity,
                               "ERROR APPROVAL_EXCEEDS_REQUEST");
            goto cleanup;
        }

        char zero_amount[AMOUNT_WIDTH];
        char approved_field[AMOUNT_WIDTH + 1];

        memset(zero_amount, '0', sizeof(zero_amount));

        if (approve) {
            snprintf(approved_field, sizeof(approved_field),
                     "%012lld", approved_amount);

            memcpy(record + APPROVED_AMOUNT_OFFSET,
                   approved_field, AMOUNT_WIDTH);
            memcpy(record + OUTSTANDING_OFFSET,
                   approved_field, AMOUNT_WIDTH);
            record[STATUS_OFFSET] = 'A';
        } else {
            memcpy(record + APPROVED_AMOUNT_OFFSET,
                   zero_amount, AMOUNT_WIDTH);
            memcpy(record + OUTSTANDING_OFFSET,
                   zero_amount, AMOUNT_WIDTH);
            record[STATUS_OFFSET] = 'R';
        }

        if (pwrite_exact(fd, record, sizeof(record), offset) == -1) {
            perror("ERROR: Cannot update loan decision");
            (void)set_response(response, response_capacity,
                               "ERROR LOAN_WRITE_FAILED");
            goto cleanup;
        }

        if (fsync(fd) == -1) {
            perror("ERROR: Loan decision written but durability is uncertain");
            (void)set_response(response, response_capacity,
                               "ERROR LOAN_DURABILITY_UNCERTAIN");
            goto cleanup;
        }

        if (approve) {
            char money[32];
            format_money(money, sizeof(money), approved_amount);

            if (set_response(response, response_capacity,
                             "LOAN_APPROVED ID=%06llu AMOUNT=%s STATUS=APPROVED",
                             wanted_id, money) == -1)
                goto cleanup;
        } else {
            if (set_response(response, response_capacity,
                             "LOAN_REJECTED ID=%06llu STATUS=REJECTED",
                             wanted_id) == -1)
                goto cleanup;
        }

        result = 0;
        goto cleanup;
    }

    if (!found)
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_NOT_FOUND");

cleanup:
    if (locked && loan_file_unlock(fd) == -1) {
        perror("ERROR: Cannot unlock loan file");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_UNLOCK_FAILED");
        result = -1;
    }

    if (close(fd) == -1) {
        perror("ERROR: Cannot close loan file");
        (void)set_response(response, response_capacity,
                           "ERROR LOAN_CLOSE_FAILED");
        result = -1;
    }

    return result == 0 ? 0 : (response[0] != '\0' ? 0 : -1);
}

/*
 * Process a loan command.
 * IMPORTANT: callers must authorize manager commands before invoking this
 * function. The account ID is supplied by the server, not by the command.
 */
int loan_process_command(const char *file_path,
                         const char *account_id,
                         const char *command,
                         char *response,
                         size_t response_capacity)
{
    if (file_path == NULL || account_id == NULL || command == NULL ||
        response == NULL || response_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    response[0] = '\0';

    if (!valid_six_digits(account_id))
        return set_response(response, response_capacity,
                            "ERROR INVALID_ACCOUNT_ID");

    char operation[24] = "";
    char arg1[64] = "";
    char arg2[64] = "";
    char extra[2] = "";

    int fields = sscanf(command, "%23s %63s %63s %1s",
                        operation, arg1, arg2, extra);

    if (fields == 1 && strcmp(operation, "LOAN_STATUS") == 0)
        return loan_status(file_path, account_id,
                           response, response_capacity);

    if (fields == 3 && strcmp(operation, "APPLY_LOAN") == 0) {
        long long amount;

        if (parse_amount(arg1, &amount) == -1)
            return set_response(response, response_capacity,
                                "ERROR INVALID_LOAN_AMOUNT");

        errno = 0;
        char *end = NULL;
        long term = strtol(arg2, &end, 10);

        if (errno == ERANGE || end == arg2 || *end != '\0' ||
            term < 1 || term > MAX_TERM_MONTHS) {
            return set_response(response, response_capacity,
                                "ERROR INVALID_LOAN_TERM");
        }

        return apply_for_loan(file_path, account_id, amount, (int)term,
                              response, response_capacity);
    }

    if (fields == 3 && strcmp(operation, "APPROVE_LOAN") == 0) {
        long long approved_amount;

        if (parse_amount(arg2, &approved_amount) == -1)
            return set_response(response, response_capacity,
                                "ERROR INVALID_APPROVED_AMOUNT");

        return decide_loan(file_path, arg1, 1, approved_amount,
                           response, response_capacity);
    }

    if (fields == 2 && strcmp(operation, "REJECT_LOAN") == 0)
        return decide_loan(file_path, arg1, 0, 0,
                           response, response_capacity);

    return set_response(response, response_capacity,
                        "ERROR INVALID_LOAN_COMMAND");
}
