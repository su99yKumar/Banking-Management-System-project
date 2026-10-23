
#define _POSIX_C_SOURCE 200809L

#include "bank_core.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RECORD_SIZE 256
#define ID_OFFSET 0
#define ID_WIDTH 6
#define BALANCE_OFFSET 149
#define BALANCE_WIDTH 12
#define MAX_BALANCE 999999999999LL
#define TRANSACTION_LOG "transactions.log"

/* Format a response without writing beyond the caller's buffer. */
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

/* Read exactly len bytes from a file at a given offset. */
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

/* Write exactly len bytes at a given file offset. */
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

/* Write all bytes to a file, handling partial writes. */
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

/* Check that an account ID contains exactly six digits. */
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

/* Convert an amount such as 25.50 into 2550 minor units. */
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

/* Parse the fixed-width 12-digit balance field. */
static int parse_balance(const char *record, long long *balance_out)
{
    long long value = 0;

    for (int i = 0; i < BALANCE_WIDTH; i++) {
        char c = record[BALANCE_OFFSET + i];

        if (c < '0' || c > '9') {
            fprintf(stderr,
                    "ERROR: Invalid digit in account balance field\n");
            return -1;
        }

        value = value * 10 + (c - '0');
    }

    *balance_out = value;
    return 0;
}

/* Format minor currency units with two decimal places. */
static void format_money(char *buffer, size_t capacity, long long amount)
{
    (void)snprintf(buffer, capacity, "%lld.%02lld",
                   amount / 100, amount % 100);
}

/*
 * Search fixed-width records.
 * Returns 1 if found, 0 if absent, or -1 on an I/O error.
 */
static int find_record(int fd, const char *id, off_t file_size,
                       off_t *record_offset, char record[RECORD_SIZE])
{
    for (off_t offset = 0; offset < file_size;
         offset += RECORD_SIZE) {
        if (pread_exact(fd, record, RECORD_SIZE, offset) == -1)
            return -1;

        if (memcmp(record + ID_OFFSET, id, ID_WIDTH) == 0) {
            *record_offset = offset;
            return 1;
        }
    }

    return 0;
}

/* Acquire an exclusive lock across one complete account record. */
static int lock_record(int fd, off_t offset)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = offset;
    lock.l_len = RECORD_SIZE;

    /* F_SETLKW waits until the record lock can be acquired. */
    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }

    return 0;
}

/* Explicitly release the account record lock. */
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

/*
 * Change semaphore 0:
 * -1 waits to acquire the semaphore.
 * +1 releases it.
 *
 * SEM_UNDO asks Linux to reverse this process's outstanding
 * semaphore adjustment if the process exits unexpectedly.
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

        return -1;
    }

    return 0;
}

/*
 * Determine the next transaction ID.
 * The caller must hold the transaction semaphore while scanning
 * and appending, so cooperating processes cannot allocate the
 * same ID at the same time.
 */
static int next_transaction_id(unsigned long long *next_id)
{
    FILE *file = fopen(TRANSACTION_LOG, "r");

    if (file == NULL) {
        if (errno == ENOENT) {
            *next_id = 1;
            return 0;
        }

        perror("ERROR: Cannot open transaction log for reading");
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

        if (fields != 6) {
            fprintf(stderr,
                    "ERROR: Malformed entry in transaction log\n");
            fclose(file);
            errno = EINVAL;
            return -1;
        }

        if (id > last_id)
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
 * Append one transaction entry using the shared System V semaphore.
 * Each entry contains:
 * ID, account ID, operation, amount, resulting balance, process ID.
 */
static int append_transaction(int semid, const char *account_id,
                              const char *operation, long long amount,
                              long long resulting_balance)
{
    if (semaphore_change(semid, -1) == -1) {
        perror("ERROR: Cannot acquire transaction-log semaphore");
        return -1;
    }

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
        fprintf(stderr, "ERROR: Transaction entry is too long\n");
        goto cleanup;
    }

    /*
     * O_APPEND directs writes to the end of the shared log.
     * All writers in this project must also follow the semaphore protocol.
     */
    log_fd = open(TRANSACTION_LOG,
                  O_WRONLY | O_CREAT | O_APPEND, 0600);

    if (log_fd == -1) {
        perror("ERROR: Cannot open transaction log for append");
        goto cleanup;
    }

    if (write_all(log_fd, line, (size_t)length) == -1) {
        perror("ERROR: Cannot append transaction entry");
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

cleanup:
    if (log_fd != -1 && close(log_fd) == -1) {
        perror("ERROR: Cannot close transaction log");
        result = -1;
    }

    /* Release the semaphore even if a logging operation failed. */
    if (semaphore_change(semid, +1) == -1) {
        perror("ERROR: Cannot release transaction-log semaphore");
        result = -1;
    }

    return result;
}

/*
 * Process BALANCE, DEPOSIT amount, or WITHDRAW amount.
 *
 * The account lock protects the record update. The System V
 * semaphore protects transaction-ID allocation and log appending.
 *
 * Return 0 when a response is prepared; return -1 for invalid
 * arguments or if a response cannot fit in the caller's buffer.
 */
int bank_process_command(const char *file_path,
                         const char *account_id,
                         const char *command,
                         char *response,
                         size_t response_capacity,
                         int transaction_semid)
{
    if (file_path == NULL || account_id == NULL || command == NULL ||
        response == NULL || response_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    response[0] = '\0';

    if (!valid_account_id(account_id))
        return set_response(response, response_capacity,
                            "ERROR INVALID_ACCOUNT_ID");

    char operation[16];
    char amount_text[64];
    char extra[2];

    int fields = sscanf(command, "%15s %63s %1s",
                        operation, amount_text, extra);

    int is_balance = fields == 1 &&
                     strcmp(operation, "BALANCE") == 0;
    int is_deposit = fields == 2 &&
                     strcmp(operation, "DEPOSIT") == 0;
    int is_withdraw = fields == 2 &&
                      strcmp(operation, "WITHDRAW") == 0;

    if (!is_balance && !is_deposit && !is_withdraw)
        return set_response(response, response_capacity,
                            "ERROR INVALID_COMMAND");

    long long amount = 0;

    if (!is_balance &&
        (parse_amount(amount_text, &amount) == -1 || amount <= 0)) {
        return set_response(response, response_capacity,
                            "ERROR INVALID_AMOUNT");
    }

    int fd = open(file_path, O_RDWR);

    if (fd == -1) {
        perror("ERROR: Cannot open account file");
        return set_response(response, response_capacity,
                            "ERROR SERVER_ERROR");
    }

    int locked = 0;
    int response_ready = 0;
    off_t record_offset = 0;
    struct stat info;
    char record[RECORD_SIZE];
    long long current_balance = 0;
    long long new_balance = 0;
    char money[32];

    if (fstat(fd, &info) == -1) {
        perror("ERROR: Cannot inspect account file");
        (void)set_response(response, response_capacity,
                           "ERROR SERVER_ERROR");
        response_ready = 1;
        goto cleanup;
    }

    if (info.st_size == 0 || info.st_size % RECORD_SIZE != 0) {
        (void)set_response(response, response_capacity,
                           "ERROR CORRUPT_ACCOUNT_FILE");
        response_ready = 1;
        goto cleanup;
    }

    int found = find_record(fd, account_id, info.st_size,
                            &record_offset, record);

    if (found == -1) {
        perror("ERROR: Cannot read account records");
        (void)set_response(response, response_capacity,
                           "ERROR SERVER_ERROR");
        response_ready = 1;
        goto cleanup;
    }

    if (found == 0) {
        (void)set_response(response, response_capacity,
                           "ERROR ACCOUNT_NOT_FOUND");
        response_ready = 1;
        goto cleanup;
    }

    if (lock_record(fd, record_offset) == -1) {
        perror("ERROR: Cannot acquire account record lock");
        (void)set_response(response, response_capacity,
                           "ERROR SERVER_ERROR");
        response_ready = 1;
        goto cleanup;
    }

    locked = 1;

    /* Always reread the account after acquiring the record lock. */
    if (pread_exact(fd, record, RECORD_SIZE, record_offset) == -1) {
        perror("ERROR: Cannot reread locked account record");
        (void)set_response(response, response_capacity,
                           "ERROR SERVER_ERROR");
        response_ready = 1;
        goto cleanup;
    }

    if (memcmp(record + ID_OFFSET, account_id, ID_WIDTH) != 0) {
        (void)set_response(response, response_capacity,
                           "ERROR ACCOUNT_RECORD_CHANGED");
        response_ready = 1;
        goto cleanup;
    }

    if (parse_balance(record, &current_balance) == -1) {
        (void)set_response(response, response_capacity,
                           "ERROR INVALID_BALANCE_FIELD");
        response_ready = 1;
        goto cleanup;
    }

    if (is_balance) {
        format_money(money, sizeof(money), current_balance);
        (void)set_response(response, response_capacity,
                           "BALANCE %s", money);
        response_ready = 1;
        goto cleanup;
    }

    if (is_deposit) {
        if (amount > MAX_BALANCE - current_balance) {
            (void)set_response(response, response_capacity,
                               "ERROR BALANCE_LIMIT_EXCEEDED");
            response_ready = 1;
            goto cleanup;
        }

        new_balance = current_balance + amount;
    } else {
        if (amount > current_balance) {
            (void)set_response(response, response_capacity,
                               "ERROR INSUFFICIENT_FUNDS");
            response_ready = 1;
            goto cleanup;
        }

        new_balance = current_balance - amount;
    }

    char balance_field[BALANCE_WIDTH + 1];

    int written = snprintf(balance_field, sizeof(balance_field),
                           "%012lld", new_balance);

    if (written != BALANCE_WIDTH) {
        (void)set_response(response, response_capacity,
                           "ERROR BALANCE_FIELD_OVERFLOW");
        response_ready = 1;
        goto cleanup;
    }

    /*
     * Update and flush the account record first.
     * This teaching version does not yet provide cross-file rollback
     * if the transaction-log operation subsequently fails.
     */
    if (pwrite_exact(fd, balance_field, BALANCE_WIDTH,
                     record_offset + BALANCE_OFFSET) == -1) {
        perror("ERROR: Cannot write updated balance");
        (void)set_response(response, response_capacity,
                           "ERROR WRITE_FAILED");
        response_ready = 1;
        goto cleanup;
    }

    if (fsync(fd) == -1) {
        perror("ERROR: Account fsync failed; balance outcome is uncertain");
        (void)set_response(response, response_capacity,
                           "ERROR BALANCE_DURABILITY_UNCERTAIN");
        response_ready = 1;
        goto cleanup;
    }

    /*
     * Log the committed balance change while still holding the
     * account lock. The semaphore serializes log IDs across accounts.
     */
    if (append_transaction(transaction_semid, account_id,
                           is_deposit ? "DEPOSIT" : "WITHDRAW",
                           amount, new_balance) == -1) {
        (void)set_response(response, response_capacity,
                           "ERROR LOG_STATUS_UNCERTAIN_BALANCE_UPDATED");
        response_ready = 1;
        goto cleanup;
    }

    format_money(money, sizeof(money), new_balance);

    (void)set_response(response, response_capacity, "%s %s",
                       is_deposit ? "DEPOSIT_OK" : "WITHDRAW_OK",
                       money);
    response_ready = 1;

cleanup:
    if (locked && unlock_record(fd, record_offset) == -1) {
        perror("ERROR: Cannot release account record lock");
        (void)set_response(response, response_capacity,
                           "ERROR LOCK_RELEASE_FAILED");
    }

    /*
     * Closing the descriptor also releases this process's traditional
     * fcntl record locks on the file.
     */
    if (close(fd) == -1) {
        perror("ERROR: Cannot close account file");
        (void)set_response(response, response_capacity,
                           "ERROR FILE_CLOSE_FAILED");
    }

    if (!response_ready)
        return set_response(response, response_capacity,
                            "ERROR SERVER_ERROR");

    return response[0] != '\0' ? 0 : -1;
}


/*
 * Look up one account without changing it.
 *
 * The initial scan locates the record. The function then obtains
 * a shared read lock and rereads the record while holding it, so
 * the returned balance is coordinated with our locked writers.
 *
 * Authorization is intentionally handled by session_server.c.
 */

/*
 * Append one new active customer record.
 * The whole account file is locked while choosing an ID and appending,
 * so concurrent creators cannot choose the same ID.
 */
int bank_create_customer(const char *file_path,
                         const char *username,
                         const char *full_name,
                         char account_id_out[7],
                         char *response,
                         size_t response_capacity)
{
    if (file_path == NULL || username == NULL || full_name == NULL ||
        account_id_out == NULL || response == NULL ||
        response_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t username_len = strlen(username);
    size_t name_len = strlen(full_name);

    if (username_len == 0 || username_len > 24 ||
        name_len == 0 || name_len > 32) {
        return set_response(response, response_capacity,
                            "ERROR INVALID_USERNAME_OR_NAME");
    }

    for (size_t i = 0; i < username_len; i++) {
        unsigned char ch = (unsigned char)username[i];
        if (!isalnum(ch) && ch != '_' && ch != '-') {
            return set_response(response, response_capacity,
                                "ERROR INVALID_USERNAME");
        }
    }

    for (size_t i = 0; i < name_len; i++) {
        unsigned char ch = (unsigned char)full_name[i];
        if (ch == '|' || ch == '\n' || ch == '\r' || ch < 32) {
            return set_response(response, response_capacity,
                                "ERROR INVALID_CUSTOMER_NAME");
        }
    }

    int fd = open(file_path, O_RDWR);
    if (fd == -1) {
        perror("ERROR: Cannot open account file for customer creation");
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_FILE_UNAVAILABLE");
    }

    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0; /* Lock from the beginning through end of file. */

    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR)
            continue;
        perror("ERROR: Cannot lock account file for customer creation");
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_FILE_LOCK_FAILED");
    }

    int result = -1;
    struct stat info;
    char record[RECORD_SIZE];
    unsigned long max_id = 0;

    if (fstat(fd, &info) == -1 || info.st_size < 0 ||
        info.st_size % RECORD_SIZE != 0) {
        (void)set_response(response, response_capacity,
                           "ERROR ACCOUNT_FILE_CORRUPT");
        goto customer_done;
    }

    for (off_t offset = 0; offset < info.st_size;
         offset += RECORD_SIZE) {
        if (pread_exact(fd, record, RECORD_SIZE, offset) == -1) {
            (void)set_response(response, response_capacity,
                               "ERROR ACCOUNT_FILE_READ_FAILED");
            goto customer_done;
        }

        unsigned long id_value = 0;
        for (int i = 0; i < 6; i++) {
            if (!isdigit((unsigned char)record[i])) {
                (void)set_response(response, response_capacity,
                                   "ERROR ACCOUNT_FILE_CORRUPT");
                goto customer_done;
            }
            id_value = id_value * 10 + (unsigned long)(record[i] - '0');
        }

        if (id_value > max_id)
            max_id = id_value;
    }

    if (max_id >= 999999UL) {
        (void)set_response(response, response_capacity,
                           "ERROR ACCOUNT_ID_LIMIT_REACHED");
        goto customer_done;
    }

    unsigned long next_id = max_id + 1;
    if (snprintf(account_id_out, 7, "%06lu", next_id) != 6) {
        (void)set_response(response, response_capacity,
                           "ERROR ACCOUNT_ID_GENERATION_FAILED");
        goto customer_done;
    }

    /*
     * Record layout:
     * ID 0-5, separators 6/8/33/66/83/148/161,
     * role 7, username 9-32, name 34-65,
     * balance 149-160, status 162, newline 255.
     */
    memset(record, ' ', sizeof(record));
    memcpy(record, account_id_out, 6);
    record[6] = '|';
    record[7] = 'C';
    record[8] = '|';
    memcpy(record + 9, username, username_len);
    record[33] = '|';
    memcpy(record + 34, full_name, name_len);
    record[66] = '|';
    record[83] = '|';
    record[148] = '|';
    memcpy(record + 149, "000000000000", 12);
    record[161] = '|';
    record[162] = 'A';
    record[255] = '\n';

    if (pwrite_exact(fd, record, sizeof(record), info.st_size) == -1 ||
        fsync(fd) == -1) {
        perror("ERROR: Cannot write new customer account");
        (void)set_response(response, response_capacity,
                           "ERROR ACCOUNT_CREATION_FAILED");
        goto customer_done;
    }

    result = set_response(response, response_capacity,
                          "ACCOUNT_CREATED %s", account_id_out);

customer_done:
    {
        int saved = errno;
        lock.l_type = F_UNLCK;
        if (fcntl(fd, F_SETLK, &lock) == -1 && result == 0) {
            perror("ERROR: Cannot unlock account file");
            result = -1;
        }
        if (close(fd) == -1 && result == 0) {
            perror("ERROR: Cannot close account file");
            result = -1;
        }
        errno = saved;
    }
    return result;
}

int bank_lookup_account(const char *file_path,
                        const char *account_id,
                        char *response,
                        size_t response_capacity)
{
    if (file_path == NULL || account_id == NULL ||
        response == NULL || response_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    if (!valid_account_id(account_id))
        return set_response(response, response_capacity,
                            "ERROR INVALID_ACCOUNT_ID");

    int fd = open(file_path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "ERROR: Cannot open account file for lookup: %s\n",
                strerror(errno));
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_FILE_UNAVAILABLE");
    }

    struct stat info;
    if (fstat(fd, &info) == -1) {
        fprintf(stderr, "ERROR: Cannot inspect account file: %s\n",
                strerror(errno));
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_FILE_READ_FAILED");
    }

    if (info.st_size < 0 || info.st_size % RECORD_SIZE != 0) {
        fprintf(stderr, "ERROR: Account file has an invalid record size\n");
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_FILE_CORRUPT");
    }

    char record[RECORD_SIZE];
    off_t record_offset = 0;

    int found = find_record(fd, account_id, info.st_size,
                            &record_offset, record);

    if (found == -1) {
        fprintf(stderr, "ERROR: Cannot search account records: %s\n",
                strerror(errno));
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_FILE_READ_FAILED");
    }

    if (found == 0) {
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_NOT_FOUND");
    }

    /* Request a shared read lock over this complete fixed-width record. */
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = record_offset;
    lock.l_len = RECORD_SIZE;

    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR)
            continue;

        fprintf(stderr, "ERROR: Cannot lock account for lookup: %s\n",
                strerror(errno));
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_LOOKUP_LOCK_FAILED");
    }

    /*
     * Reread after acquiring the lock. Do not rely on the earlier
     * unlocked scan for the balance value.
     */
    if (pread_exact(fd, record, RECORD_SIZE, record_offset) == -1) {
        fprintf(stderr, "ERROR: Cannot read locked account record: %s\n",
                strerror(errno));
        unlock_record(fd, record_offset);
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_FILE_READ_FAILED");
    }

    if (memcmp(record + ID_OFFSET, account_id, ID_WIDTH) != 0) {
        unlock_record(fd, record_offset);
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_CHANGED_RETRY");
    }

    long long balance = 0;
    if (parse_balance(record, &balance) == -1) {
        unlock_record(fd, record_offset);
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR INVALID_BALANCE_FIELD");
    }

    char money[32];
    format_money(money, sizeof(money), balance);

    if (unlock_record(fd, record_offset) == -1) {
        fprintf(stderr, "ERROR: Cannot release account read lock: %s\n",
                strerror(errno));
        close(fd);
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_LOOKUP_UNLOCK_FAILED");
    }

    if (close(fd) == -1) {
        fprintf(stderr, "ERROR: Cannot close account file: %s\n",
                strerror(errno));
        return set_response(response, response_capacity,
                            "ERROR ACCOUNT_FILE_READ_FAILED");
    }

    return set_response(response, response_capacity,
                        "ACCOUNT %s BALANCE %s", account_id, money);
}

/*
 * Append the two sides of a transfer while holding the shared log
 * semaphore. This prevents another cooperating process from placing
 * its transaction ID between the transfer's OUT and IN entries.
 */
static int append_transfer_transactions(int semid,
                                        const char *from_id,
                                        const char *to_id,
                                        long long amount,
                                        long long from_balance,
                                        long long to_balance)
{
    if (semaphore_change(semid, -1) == -1) {
        perror("ERROR: Cannot acquire transaction-log semaphore");
        return -1;
    }

    int result = -1;
    int log_fd = -1;
    unsigned long long first_id;
    char lines[512];

    if (next_transaction_id(&first_id) == -1)
        goto cleanup;

    /* A transfer requires two different transaction IDs. */
    if (first_id >= ULLONG_MAX) {
        errno = ERANGE;
        perror("ERROR: Not enough transaction IDs for transfer");
        goto cleanup;
    }

    int first_length = snprintf(
        lines, sizeof(lines), "%llu %s TRANSFER_OUT %lld %lld %ld\n",
        first_id, from_id, amount, from_balance, (long)getpid());

    if (first_length < 0 || (size_t)first_length >= sizeof(lines)) {
        fprintf(stderr, "ERROR: Transfer log entry is too long\n");
        goto cleanup;
    }

    int second_length = snprintf(
        lines + first_length, sizeof(lines) - (size_t)first_length,
        "%llu %s TRANSFER_IN %lld %lld %ld\n",
        first_id + 1, to_id, amount, to_balance, (long)getpid());

    if (second_length < 0 ||
        (size_t)second_length >= sizeof(lines) - (size_t)first_length) {
        fprintf(stderr, "ERROR: Transfer log pair is too long\n");
        goto cleanup;
    }

    size_t total_length = (size_t)first_length + (size_t)second_length;

    log_fd = open(TRANSACTION_LOG,
                  O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_fd == -1) {
        perror("ERROR: Cannot open transaction log");
        goto cleanup;
    }

    if (write_all(log_fd, lines, total_length) == -1) {
        perror("ERROR: Transfer log write failed; outcome is uncertain");
        goto cleanup;
    }

    if (fsync(log_fd) == -1) {
        perror("ERROR: Transfer log fsync failed; outcome is uncertain");
        goto cleanup;
    }

    if (close(log_fd) == -1) {
        log_fd = -1;
        perror("ERROR: Cannot close transfer log");
        goto cleanup;
    }

    log_fd = -1;
    result = 0;

cleanup:
    if (log_fd != -1 && close(log_fd) == -1) {
        perror("ERROR: Cannot close transfer log");
        result = -1;
    }

    if (semaphore_change(semid, +1) == -1) {
        perror("ERROR: Cannot release transaction-log semaphore");
        result = -1;
    }

    return result;
}

/*
 * Restore both original balance fields after a failed transfer write.
 * This is best-effort rollback, not full crash recovery.
 */
static int restore_transfer_balances(int fd,
                                     off_t from_offset,
                                     const char *old_from,
                                     off_t to_offset,
                                     const char *old_to)
{
    int failed = 0;

    if (pwrite_exact(fd, old_from, BALANCE_WIDTH,
                     from_offset + BALANCE_OFFSET) == -1) {
        perror("ERROR: Cannot restore sender balance");
        failed = 1;
    }

    if (pwrite_exact(fd, old_to, BALANCE_WIDTH,
                     to_offset + BALANCE_OFFSET) == -1) {
        perror("ERROR: Cannot restore recipient balance");
        failed = 1;
    }

    if (fsync(fd) == -1) {
        perror("ERROR: Cannot sync restored balances");
        failed = 1;
    }

    return failed ? -1 : 0;
}

/*
 * TRANSFER target_account amount
 *
 * The caller must authorize the customer role. Both account records
 * are locked in ascending file-offset order to avoid lock-order
 * deadlocks. The balances are reread only after both locks are held.
 */
int bank_transfer_command(const char *file_path,
                          const char *from_id,
                          const char *command,
                          char *response,
                          size_t response_capacity,
                          int transaction_semid)
{
    if (file_path == NULL || from_id == NULL || command == NULL ||
        response == NULL || response_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    response[0] = '\0';

    if (!valid_account_id(from_id))
        return set_response(response, response_capacity,
                            "ERROR INVALID_ACCOUNT_ID");

    char operation[16];
    char target_text[64];
    char amount_text[64];
    char extra[2];

    int fields = sscanf(command, "%15s %63s %63s %1s",
                        operation, target_text, amount_text, extra);

    if (fields != 3 || strcmp(operation, "TRANSFER") != 0)
        return set_response(response, response_capacity,
                            "ERROR INVALID_COMMAND");

    if (!valid_account_id(target_text))
        return set_response(response, response_capacity,
                            "ERROR INVALID_TARGET_ACCOUNT");

    if (strcmp(from_id, target_text) == 0)
        return set_response(response, response_capacity,
                            "ERROR CANNOT_TRANSFER_TO_SELF");

    long long amount;
    if (parse_amount(amount_text, &amount) == -1 || amount <= 0)
        return set_response(response, response_capacity,
                            "ERROR INVALID_AMOUNT");

    int fd = open(file_path, O_RDWR);
    if (fd == -1) {
        perror("ERROR: Cannot open account file for transfer");
        return set_response(response, response_capacity,
                            "ERROR SERVER_ERROR");
    }

    int result = 0;
    int first_locked = 0;
    int second_locked = 0;
    off_t from_offset = 0;
    off_t to_offset = 0;
    off_t first_offset;
    off_t second_offset;
    struct stat info;
    char from_record[RECORD_SIZE];
    char to_record[RECORD_SIZE];
    char old_from[BALANCE_WIDTH];
    char old_to[BALANCE_WIDTH];
    char new_from_field[BALANCE_WIDTH + 1];
    char new_to_field[BALANCE_WIDTH + 1];
    long long from_balance;
    long long to_balance;
    long long new_from;
    long long new_to;

    if (fstat(fd, &info) == -1) {
        perror("ERROR: Cannot inspect account file");
        result = set_response(response, response_capacity,
                              "ERROR SERVER_ERROR");
        goto transfer_cleanup;
    }

    if (info.st_size == 0 || info.st_size % RECORD_SIZE != 0) {
        result = set_response(response, response_capacity,
                              "ERROR CORRUPT_ACCOUNT_FILE");
        goto transfer_cleanup;
    }

    int found_from = find_record(fd, from_id, info.st_size,
                                 &from_offset, from_record);
    int found_to = find_record(fd, target_text, info.st_size,
                               &to_offset, to_record);

    if (found_from == -1 || found_to == -1) {
        perror("ERROR: Cannot scan accounts for transfer");
        result = set_response(response, response_capacity,
                              "ERROR SERVER_ERROR");
        goto transfer_cleanup;
    }

    if (found_from == 0) {
        result = set_response(response, response_capacity,
                              "ERROR ACCOUNT_NOT_FOUND");
        goto transfer_cleanup;
    }

    if (found_to == 0) {
        result = set_response(response, response_capacity,
                              "ERROR TARGET_ACCOUNT_NOT_FOUND");
        goto transfer_cleanup;
    }

    /*
     * Lock order is always lower offset first, regardless of which
     * account is sender or recipient.
     */
    if (from_offset < to_offset) {
        first_offset = from_offset;
        second_offset = to_offset;
    } else {
        first_offset = to_offset;
        second_offset = from_offset;
    }

    if (lock_record(fd, first_offset) == -1) {
        perror("ERROR: Cannot lock first transfer account");
        result = set_response(response, response_capacity,
                              "ERROR LOCK_FAILED");
        goto transfer_cleanup;
    }
    first_locked = 1;

    if (lock_record(fd, second_offset) == -1) {
        perror("ERROR: Cannot lock second transfer account");
        result = set_response(response, response_capacity,
                              "ERROR LOCK_FAILED");
        goto transfer_cleanup;
    }
    second_locked = 1;

    /* Reread both records while their locks are held. */
    if (pread_exact(fd, from_record, RECORD_SIZE, from_offset) == -1 ||
        pread_exact(fd, to_record, RECORD_SIZE, to_offset) == -1) {
        perror("ERROR: Cannot reread locked transfer accounts");
        result = set_response(response, response_capacity,
                              "ERROR SERVER_ERROR");
        goto transfer_cleanup;
    }

    if (memcmp(from_record + ID_OFFSET, from_id, ID_WIDTH) != 0 ||
        memcmp(to_record + ID_OFFSET, target_text, ID_WIDTH) != 0) {
        result = set_response(response, response_capacity,
                              "ERROR ACCOUNT_RECORD_CHANGED");
        goto transfer_cleanup;
    }

    if (parse_balance(from_record, &from_balance) == -1 ||
        parse_balance(to_record, &to_balance) == -1) {
        result = set_response(response, response_capacity,
                              "ERROR INVALID_BALANCE_FIELD");
        goto transfer_cleanup;
    }

    if (amount > from_balance) {
        result = set_response(response, response_capacity,
                              "ERROR INSUFFICIENT_FUNDS");
        goto transfer_cleanup;
    }

    if (amount > MAX_BALANCE - to_balance) {
        result = set_response(response, response_capacity,
                              "ERROR TARGET_BALANCE_LIMIT_EXCEEDED");
        goto transfer_cleanup;
    }

    new_from = from_balance - amount;
    new_to = to_balance + amount;

    memcpy(old_from, from_record + BALANCE_OFFSET, BALANCE_WIDTH);
    memcpy(old_to, to_record + BALANCE_OFFSET, BALANCE_WIDTH);

    int from_length = snprintf(new_from_field, sizeof(new_from_field),
                               "%012lld", new_from);
    int to_length = snprintf(new_to_field, sizeof(new_to_field),
                             "%012lld", new_to);

    if (from_length != BALANCE_WIDTH || to_length != BALANCE_WIDTH) {
        result = set_response(response, response_capacity,
                              "ERROR BALANCE_FIELD_OVERFLOW");
        goto transfer_cleanup;
    }

    /*
     * If either write fails, attempt to restore both original fields.
     * An abrupt process/machine failure can still interrupt this sequence.
     */
    if (pwrite_exact(fd, new_from_field, BALANCE_WIDTH,
                     from_offset + BALANCE_OFFSET) == -1 ||
        pwrite_exact(fd, new_to_field, BALANCE_WIDTH,
                     to_offset + BALANCE_OFFSET) == -1) {
        perror("ERROR: Transfer balance write failed");

        if (restore_transfer_balances(fd, from_offset, old_from,
                                      to_offset, old_to) == -1) {
            result = set_response(response, response_capacity,
                                  "ERROR TRANSFER_ROLLBACK_UNCERTAIN");
        } else {
            result = set_response(response, response_capacity,
                                  "ERROR TRANSFER_NOT_COMMITTED");
        }
        goto transfer_cleanup;
    }

    if (fsync(fd) == -1) {
        perror("ERROR: Transfer account fsync failed");

        if (restore_transfer_balances(fd, from_offset, old_from,
                                      to_offset, old_to) == -1) {
            result = set_response(response, response_capacity,
                                  "ERROR TRANSFER_DURABILITY_UNCERTAIN");
        } else {
            result = set_response(response, response_capacity,
                                  "ERROR TRANSFER_NOT_COMMITTED");
        }
        goto transfer_cleanup;
    }

    /*
     * The balances have been synced. A log failure is reported as
     * uncertain because a partial log write may already have occurred.
     */
    if (append_transfer_transactions(transaction_semid, from_id, target_text,
                                     amount, new_from, new_to) == -1) {
        result = set_response(response, response_capacity,
                              "ERROR TRANSFER_LOG_STATUS_UNCERTAIN");
        goto transfer_cleanup;
    }

    char money[32];
    format_money(money, sizeof(money), new_from);

    result = set_response(response, response_capacity,
                          "TRANSFER_OK TO=%s AMOUNT=%s BALANCE=%s",
                          target_text, amount_text, money);

transfer_cleanup:
    /* Release locks in reverse acquisition order. */
    if (second_locked && unlock_record(fd, second_offset) == -1) {
        perror("ERROR: Cannot unlock second transfer account");
        (void)set_response(response, response_capacity,
                           "ERROR LOCK_RELEASE_FAILED");
    }

    if (first_locked && unlock_record(fd, first_offset) == -1) {
        perror("ERROR: Cannot unlock first transfer account");
        (void)set_response(response, response_capacity,
                           "ERROR LOCK_RELEASE_FAILED");
    }

    if (close(fd) == -1) {
        perror("ERROR: Cannot close transfer account file");
        (void)set_response(response, response_capacity,
                           "ERROR FILE_CLOSE_FAILED");
    }

    return result < 0 ? -1 : 0;
}

/*
 * Return the five latest transaction records for one account.
 * The shared semaphore makes the read wait until a cooperating writer
 * has finished its log append.
 */
int bank_transaction_history(const char *log_path,
                             const char *account_id,
                             char *response,
                             size_t response_capacity,
                             int transaction_semid)
{
    if (log_path == NULL || account_id == NULL || response == NULL ||
        response_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    response[0] = '\0';

    if (!valid_account_id(account_id))
        return set_response(response, response_capacity,
                            "ERROR INVALID_ACCOUNT_ID");

    enum { HISTORY_LIMIT = 5, LOG_LINE_CAPACITY = 256 };
    char latest[HISTORY_LIMIT][LOG_LINE_CAPACITY];
    size_t count = 0;
    size_t next = 0;
    int scan_error = 0;
    int missing_log = 0;
    FILE *file = NULL;

    if (semaphore_change(transaction_semid, -1) == -1) {
        perror("ERROR: Cannot lock transaction log for reading");
        return set_response(response, response_capacity,
                            "ERROR TRANSACTION_LOG_BUSY");
    }

    file = fopen(log_path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            missing_log = 1;
        } else {
            perror("ERROR: Cannot open transaction log");
            scan_error = 1;
        }
    }

    if (file != NULL) {
        char line[LOG_LINE_CAPACITY];

        while (fgets(line, sizeof(line), file) != NULL) {
            unsigned long long id;
            char logged_account[7];
            char operation[16];
            long long amount;
            long long balance;
            long process_id;

            int fields = sscanf(line, "%llu %6s %15s %lld %lld %ld",
                                &id, logged_account, operation,
                                &amount, &balance, &process_id);

            if (fields != 6) {
                fprintf(stderr,
                        "ERROR: Malformed transaction log entry\n");
                scan_error = 1;
                break;
            }

            if (strcmp(logged_account, account_id) == 0) {
                (void)snprintf(latest[next], sizeof(latest[next]),
                               "%s", line);
                next = (next + 1) % HISTORY_LIMIT;

                if (count < HISTORY_LIMIT)
                    count++;
            }
        }

        if (ferror(file)) {
            perror("ERROR: Failed reading transaction log");
            scan_error = 1;
        }

        if (fclose(file) == EOF) {
            perror("ERROR: Cannot close transaction log");
            scan_error = 1;
        }
    }

    if (semaphore_change(transaction_semid, +1) == -1) {
        perror("ERROR: Cannot unlock transaction log");
        scan_error = 1;
    }

    if (scan_error)
        return set_response(response, response_capacity,
                            "ERROR TRANSACTION_LOG_READ_FAILED");

    if (missing_log || count == 0)
        return set_response(response, response_capacity,
                            "TRANSACTION_HISTORY EMPTY");

    int written = snprintf(response, response_capacity,
                           "TRANSACTION_HISTORY ACCOUNT=%s (LATEST %zu)\n",
                           account_id, count);

    if (written < 0 || (size_t)written >= response_capacity)
        return set_response(response, response_capacity,
                            "ERROR HISTORY_RESPONSE_TOO_LARGE");

    size_t used = (size_t)written;
    size_t oldest = count < HISTORY_LIMIT ? 0 : next;

    for (size_t i = 0; i < count; i++) {
        size_t index = (oldest + i) % HISTORY_LIMIT;
        size_t length = strlen(latest[index]);

        if (used + length + 1 > response_capacity)
            return set_response(response, response_capacity,
                                "ERROR HISTORY_RESPONSE_TOO_LARGE");

        memcpy(response + used, latest[index], length);
        used += length;

        /* Preserve a newline if the source log's final row lacked one. */
        if (used == 0 || response[used - 1] != '\n')
            response[used++] = '\n';

        response[used] = '\0';
    }

    return 0;
}

/* Manager operation: set account status to A (active) or D (deactivated). */
int bank_set_account_status(const char *file_path, const char *account_id,
                            char status, char *response, size_t capacity)
{
    if (file_path == NULL || account_id == NULL || response == NULL ||
        capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    if (!valid_account_id(account_id))
        return set_response(response, capacity, "ERROR INVALID_ACCOUNT_ID");

    if (status != 'A' && status != 'D')
        return set_response(response, capacity, "ERROR INVALID_STATUS");

    int fd = open(file_path, O_RDWR);
    if (fd == -1) {
        perror("ERROR: Cannot open account file");
        return set_response(response, capacity, "ERROR SERVER_ERROR");
    }

    int result = 0;
    int locked = 0;
    struct stat info;
    off_t offset = 0;
    char record[RECORD_SIZE];

    if (fstat(fd, &info) == -1 ||
        info.st_size == 0 || info.st_size % RECORD_SIZE != 0) {
        result = set_response(response, capacity, "ERROR CORRUPT_ACCOUNT_FILE");
        goto status_done;
    }

    int found = find_record(fd, account_id, info.st_size, &offset, record);
    if (found == -1) {
        result = set_response(response, capacity, "ERROR SERVER_ERROR");
        goto status_done;
    }
    if (found == 0) {
        result = set_response(response, capacity, "ERROR ACCOUNT_NOT_FOUND");
        goto status_done;
    }

    if (lock_record(fd, offset) == -1) {
        result = set_response(response, capacity, "ERROR LOCK_FAILED");
        goto status_done;
    }
    locked = 1;

    if (pread_exact(fd, record, RECORD_SIZE, offset) == -1 ||
        memcmp(record + ID_OFFSET, account_id, ID_WIDTH) != 0) {
        result = set_response(response, capacity, "ERROR ACCOUNT_READ_FAILED");
        goto status_done;
    }

    if (record[162] != 'A' && record[162] != 'D') {
        result = set_response(response, capacity, "ERROR INVALID_STATUS_FIELD");
        goto status_done;
    }

    if (pwrite_exact(fd, &status, 1, offset + 162) == -1 ||
        fsync(fd) == -1) {
        perror("ERROR: Account status write failed");
        result = set_response(response, capacity, "ERROR STATUS_UPDATE_UNCERTAIN");
        goto status_done;
    }

    result = set_response(response, capacity, "ACCOUNT_STATUS_UPDATED %s %c",
                          account_id, status);

status_done:
    if (locked && unlock_record(fd, offset) == -1) {
        perror("ERROR: Cannot unlock account record");
        result = set_response(response, capacity, "ERROR LOCK_RELEASE_FAILED");
    }
    if (close(fd) == -1) {
        perror("ERROR: Cannot close account file");
        result = set_response(response, capacity, "ERROR FILE_CLOSE_FAILED");
    }
    return result;
}

/* Employee/admin operation: modify the customer's display name. */
int bank_modify_customer_name(const char *file_path, const char *account_id,
                              const char *new_name, char *response,
                              size_t capacity)
{
    if (file_path == NULL || account_id == NULL || new_name == NULL ||
        response == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    if (!valid_account_id(account_id))
        return set_response(response, capacity, "ERROR INVALID_ACCOUNT_ID");

    size_t name_length = strlen(new_name);
    if (name_length == 0 || name_length > 32)
        return set_response(response, capacity, "ERROR INVALID_NAME_LENGTH");

    for (size_t i = 0; i < name_length; i++) {
        unsigned char c = (unsigned char)new_name[i];
        if (c < 32 || c == 127 || c == '|')
            return set_response(response, capacity, "ERROR INVALID_NAME");
    }

    int fd = open(file_path, O_RDWR);
    if (fd == -1) {
        perror("ERROR: Cannot open account file");
        return set_response(response, capacity, "ERROR SERVER_ERROR");
    }

    int result = 0;
    int locked = 0;
    struct stat info;
    off_t offset = 0;
    char record[RECORD_SIZE];
    char field[32];

    if (fstat(fd, &info) == -1 ||
        info.st_size == 0 || info.st_size % RECORD_SIZE != 0) {
        result = set_response(response, capacity, "ERROR CORRUPT_ACCOUNT_FILE");
        goto name_done;
    }

    int found = find_record(fd, account_id, info.st_size, &offset, record);
    if (found == -1) {
        result = set_response(response, capacity, "ERROR SERVER_ERROR");
        goto name_done;
    }
    if (found == 0) {
        result = set_response(response, capacity, "ERROR ACCOUNT_NOT_FOUND");
        goto name_done;
    }

    if (lock_record(fd, offset) == -1) {
        result = set_response(response, capacity, "ERROR LOCK_FAILED");
        goto name_done;
    }
    locked = 1;

    if (pread_exact(fd, record, RECORD_SIZE, offset) == -1 ||
        memcmp(record + ID_OFFSET, account_id, ID_WIDTH) != 0) {
        result = set_response(response, capacity, "ERROR ACCOUNT_READ_FAILED");
        goto name_done;
    }

    memset(field, ' ', sizeof(field));
    memcpy(field, new_name, name_length);

    if (pwrite_exact(fd, field, sizeof(field), offset + 34) == -1 ||
        fsync(fd) == -1) {
        perror("ERROR: Customer name update failed");
        result = set_response(response, capacity, "ERROR NAME_UPDATE_UNCERTAIN");
        goto name_done;
    }

    result = set_response(response, capacity, "CUSTOMER_NAME_UPDATED %s",
                          account_id);

name_done:
    if (locked && unlock_record(fd, offset) == -1) {
        perror("ERROR: Cannot unlock account record");
        result = set_response(response, capacity, "ERROR LOCK_RELEASE_FAILED");
    }
    if (close(fd) == -1) {
        perror("ERROR: Cannot close account file");
        result = set_response(response, capacity, "ERROR FILE_CLOSE_FAILED");
    }
    return result;
}

/* Return 1 for active, 0 for deactivated, or -1 on an error. */
int bank_account_is_active(const char *file_path, const char *account_id)
{
    if (file_path == NULL || account_id == NULL ||
        !valid_account_id(account_id)) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(file_path, O_RDWR);
    if (fd == -1)
        return -1;

    int result = -1;
    int locked = 0;
    struct stat info;
    off_t offset = 0;
    char record[RECORD_SIZE];

    if (fstat(fd, &info) == -1 ||
        info.st_size == 0 || info.st_size % RECORD_SIZE != 0)
        goto active_done;

    int found = find_record(fd, account_id, info.st_size, &offset, record);
    if (found <= 0) {
        if (found == 0)
            errno = ENOENT;
        goto active_done;
    }

    if (lock_record(fd, offset) == -1)
        goto active_done;
    locked = 1;

    if (pread_exact(fd, record, RECORD_SIZE, offset) == -1)
        goto active_done;

    if (memcmp(record + ID_OFFSET, account_id, ID_WIDTH) != 0) {
        errno = EINVAL;
        goto active_done;
    }

    if (record[162] == 'A')
        result = 1;
    else if (record[162] == 'D')
        result = 0;
    else
        errno = EINVAL;

active_done:
    if (locked && unlock_record(fd, offset) == -1)
        result = -1;
    if (close(fd) == -1)
        result = -1;
    return result;
}
