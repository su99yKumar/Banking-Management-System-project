#define _POSIX_C_SOURCE 200809L

#include "user_store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CREDENTIAL_FILE "credentials.dat"
#define FEEDBACK_FILE "feedback.log"
#define CREDENTIAL_RECORD_SIZE 128
#define USERNAME_WIDTH 31
#define PASSWORD_OFFSET 32
#define PASSWORD_WIDTH 64

static size_t credential_count;

/* Read exactly len bytes at an explicit offset; short reads are errors. */
static int pread_full(int fd, void *buffer, size_t len, off_t offset)
{
    size_t done = 0;
    char *p = buffer;

    while (done < len) {
        ssize_t n = pread(fd, p + done, len - done, offset + (off_t)done);

        if (n == 0) {
            errno = EIO;
            return -1;
        }
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* Write exactly len bytes at an explicit offset. */
static int pwrite_full(int fd, const void *buffer, size_t len, off_t offset)
{
    size_t done = 0;
    const char *p = buffer;

    while (done < len) {
        ssize_t n = pwrite(fd, p + done, len - done,
                           offset + (off_t)done);

        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* Lock one fixed-width credential record. */
static int credential_lock(int fd, off_t offset, short type)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = offset;
    lock.l_len = CREDENTIAL_RECORD_SIZE;

    int cmd = (type == F_UNLCK) ? F_SETLK : F_SETLKW;
    while (fcntl(fd, cmd, &lock) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

/* Create one padded credential record. This demo stores plaintext passwords. */
static int make_record(char record[CREDENTIAL_RECORD_SIZE],
                       const char *username, const char *password)
{
    size_t user_len = strlen(username);
    size_t password_len = strlen(password);

    if (user_len == 0 || user_len > USERNAME_WIDTH ||
        password_len == 0 || password_len >= PASSWORD_WIDTH) {
        errno = EINVAL;
        return -1;
    }

    memset(record, ' ', CREDENTIAL_RECORD_SIZE);
    memcpy(record, username, user_len);
    record[USERNAME_WIDTH] = ' ';
    memcpy(record + PASSWORD_OFFSET, password, password_len);
    record[CREDENTIAL_RECORD_SIZE - 1] = '\n';
    return 0;
}

/* Check if a record belongs to username, using its fixed-width field. */
static int record_matches_user(const char *record, const char *username)
{
    char field[USERNAME_WIDTH];
    size_t length = strlen(username);

    if (length == 0 || length > sizeof(field))
        return 0;

    memset(field, ' ', sizeof(field));
    memcpy(field, username, length);

    return record[USERNAME_WIDTH] == ' ' &&
           record[CREDENTIAL_RECORD_SIZE - 1] == '\n' &&
           memcmp(record, field, sizeof(field)) == 0;
}

/* Extract the stored password after validating the record layout. */
static int record_password(const char *record, char password[PASSWORD_WIDTH + 1])
{
    if (record[USERNAME_WIDTH] != ' ' ||
        record[CREDENTIAL_RECORD_SIZE - 1] != '\n') {
        errno = EINVAL;
        return -1;
    }

    memcpy(password, record + PASSWORD_OFFSET, PASSWORD_WIDTH);
    password[PASSWORD_WIDTH] = '\0';

    for (int i = PASSWORD_WIDTH - 1; i >= 0 && password[i] == ' '; i--)
        password[i] = '\0';

    if (password[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int user_store_init(const char *const usernames[],
                    const char *const passwords[],
                    size_t count)
{
    if (usernames == NULL || passwords == NULL || count == 0 ||
        count > (size_t)LLONG_MAX / CREDENTIAL_RECORD_SIZE) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(CREDENTIAL_FILE, O_RDWR | O_CREAT, 0600);
    if (fd == -1)
        return -1;

    struct stat info;
    int result = -1;

    if (fstat(fd, &info) == -1)
        goto cleanup;

    if (info.st_size == 0) {
        for (size_t i = 0; i < count; i++) {
            char record[CREDENTIAL_RECORD_SIZE];

            if (make_record(record, usernames[i], passwords[i]) == -1)
                goto cleanup;

            if (pwrite_full(fd, record, sizeof(record),
                            (off_t)(i * CREDENTIAL_RECORD_SIZE)) == -1)
                goto cleanup;
        }

        if (fsync(fd) == -1)
            goto cleanup;
        info.st_size = (off_t)(count * CREDENTIAL_RECORD_SIZE);
    }

    if (info.st_size < (off_t)(count * CREDENTIAL_RECORD_SIZE) ||
        info.st_size % CREDENTIAL_RECORD_SIZE != 0) {
        errno = EINVAL;
        goto cleanup;
    }

    credential_count = (size_t)(info.st_size / CREDENTIAL_RECORD_SIZE);
    result = 0;

cleanup:
    {
        int saved_errno = errno;
        if (close(fd) == -1 && result == 0) {
            saved_errno = errno;
            result = -1;
        }
        errno = saved_errno;
    }
    return result;
}


static int refresh_credential_count_from_disk(void)
{
    struct stat info;
    if (stat(CREDENTIAL_FILE, &info) == -1 || info.st_size < 0 ||
        info.st_size % CREDENTIAL_RECORD_SIZE != 0) {
        if (errno == 0) errno = EINVAL;
        return -1;
    }
    credential_count = (size_t)(info.st_size / CREDENTIAL_RECORD_SIZE);
    return 0;
}

int user_store_password_matches(const char *username, const char *password)
{
    if (username == NULL || password == NULL) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(CREDENTIAL_FILE, O_RDONLY);
    if (fd == -1)
        return -1;

    struct stat info;
    if (fstat(fd, &info) == -1 || info.st_size < 0 ||
        info.st_size % CREDENTIAL_RECORD_SIZE != 0) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }

    size_t count = (size_t)(info.st_size / CREDENTIAL_RECORD_SIZE);
    int result = 0;
    char record[CREDENTIAL_RECORD_SIZE];
    char saved_password[PASSWORD_WIDTH + 1];

    for (size_t i = 0; i < count; i++) {
        off_t offset = (off_t)(i * CREDENTIAL_RECORD_SIZE);

        if (credential_lock(fd, offset, F_RDLCK) == -1) {
            result = -1;
            break;
        }

        int read_result = pread_full(fd, record, sizeof(record), offset);
        int unlock_result = credential_lock(fd, offset, F_UNLCK);

        if (read_result == -1 || unlock_result == -1) {
            result = -1;
            break;
        }

        if (!record_matches_user(record, username))
            continue;

        if (record_password(record, saved_password) == -1) {
            result = -1;
            break;
        }

        result = (strcmp(saved_password, password) == 0) ? 1 : 0;
        break;
    }

    if (close(fd) == -1)
        result = -1;

    return result;
}

int user_store_change_password(const char *username,
                               const char *command,
                               char *response,
                               size_t response_capacity)
{
    if (refresh_credential_count_from_disk() == -1) return -1;

    if (username == NULL || command == NULL || response == NULL ||
        response_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    char old_password[PASSWORD_WIDTH];
    char new_password[PASSWORD_WIDTH];
    char extra[2];

    int fields = sscanf(command, "CHANGE_PASSWORD %63s %63s %1s",
                        old_password, new_password, extra);

    if (fields != 2 || strlen(new_password) < 8 ||
        strcmp(old_password, new_password) == 0) {
        (void)snprintf(response, response_capacity,
                       "ERROR INVALID_PASSWORD_FORMAT");
        return 0;
    }

    int fd = open(CREDENTIAL_FILE, O_RDWR);
    if (fd == -1) {
        (void)snprintf(response, response_capacity,
                       "ERROR CREDENTIAL_STORE_UNAVAILABLE");
        return 0;
    }

    struct stat info;
    if (fstat(fd, &info) == -1 || info.st_size < 0 ||
        info.st_size % CREDENTIAL_RECORD_SIZE != 0) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }

    size_t count = (size_t)(info.st_size / CREDENTIAL_RECORD_SIZE);
    int found = 0;
    int result = 0;
    char record[CREDENTIAL_RECORD_SIZE];
    char saved_password[PASSWORD_WIDTH + 1];

    for (size_t i = 0; i < count; i++) {
        off_t offset = (off_t)(i * CREDENTIAL_RECORD_SIZE);

        if (credential_lock(fd, offset, F_WRLCK) == -1) {
            (void)snprintf(response, response_capacity,
                           "ERROR CREDENTIAL_LOCK_FAILED");
            result = -1;
            break;
        }

        int read_result = pread_full(fd, record, sizeof(record), offset);

        if (read_result == -1) {
            (void)credential_lock(fd, offset, F_UNLCK);
            (void)snprintf(response, response_capacity,
                           "ERROR CREDENTIAL_READ_FAILED");
            result = -1;
            break;
        }

        if (!record_matches_user(record, username)) {
            if (credential_lock(fd, offset, F_UNLCK) == -1) {
                result = -1;
                break;
            }
            continue;
        }

        found = 1;

        if (record_password(record, saved_password) == -1) {
            (void)snprintf(response, response_capacity,
                           "ERROR CREDENTIAL_RECORD_INVALID");
            result = -1;
        } else if (strcmp(saved_password, old_password) != 0) {
            (void)snprintf(response, response_capacity,
                           "ERROR CURRENT_PASSWORD_INCORRECT");
        } else {
            char updated[CREDENTIAL_RECORD_SIZE];

            if (make_record(updated, username, new_password) == -1 ||
                pwrite_full(fd, updated, sizeof(updated), offset) == -1 ||
                fsync(fd) == -1) {
                perror("ERROR: Password update durability is uncertain");
                (void)snprintf(response, response_capacity,
                               "ERROR PASSWORD_UPDATE_UNCERTAIN");
                result = -1;
            } else {
                (void)snprintf(response, response_capacity,
                               "PASSWORD_CHANGED");
            }
        }

        if (credential_lock(fd, offset, F_UNLCK) == -1) {
            (void)snprintf(response, response_capacity,
                           "ERROR CREDENTIAL_UNLOCK_FAILED");
            result = -1;
        }
        break;
    }

    if (!found && result == 0)
        (void)snprintf(response, response_capacity,
                       "ERROR USER_NOT_FOUND");

    if (close(fd) == -1) {
        (void)snprintf(response, response_capacity,
                       "ERROR CREDENTIAL_CLOSE_FAILED");
        result = -1;
    }

    return result;
}

/* Append a new credential record without truncating existing users. */
int user_store_add_password(const char *username, const char *password)
{
    if (username == NULL || password == NULL) {
        errno = EINVAL;
        return -1;
    }

    char record[CREDENTIAL_RECORD_SIZE];
    if (make_record(record, username, password) == -1)
        return -1;

    int fd = open(CREDENTIAL_FILE, O_RDWR | O_CREAT, 0600);
    if (fd == -1)
        return -1;

    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    int result = -1;
    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR)
            continue;
        goto done;
    }

    struct stat info;
    if (fstat(fd, &info) == -1 || info.st_size < 0 ||
        info.st_size % CREDENTIAL_RECORD_SIZE != 0)
        goto unlock;

    char existing[CREDENTIAL_RECORD_SIZE];
    size_t count = (size_t)(info.st_size / CREDENTIAL_RECORD_SIZE);

    for (size_t i = 0; i < count; i++) {
        off_t offset = (off_t)(i * CREDENTIAL_RECORD_SIZE);
        if (pread_full(fd, existing, sizeof(existing), offset) == -1)
            goto unlock;
        if (record_matches_user(existing, username)) {
            errno = EEXIST;
            goto unlock;
        }
    }

    if (pwrite_full(fd, record, sizeof(record), info.st_size) == -1 ||
        fsync(fd) == -1)
        goto unlock;

    result = 0;

unlock:
    {
        int saved = errno;
        lock.l_type = F_UNLCK;
        if (fcntl(fd, F_SETLK, &lock) == -1 && result == 0) {
            saved = errno;
            result = -1;
        }
        errno = saved;
    }

done:
    {
        int saved = errno;
        if (close(fd) == -1 && result == 0) {
            saved = errno;
            result = -1;
        }
        errno = saved;
    }

    return result;
}

/* Acquire/release the shared SysV semaphore; retry interrupted operations. */
static int change_semaphore(int semaphore_id, short change)
{
    struct sembuf operation = { .sem_num = 0, .sem_op = change,
                                .sem_flg = SEM_UNDO };

    while (semop(semaphore_id, &operation, 1) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

int user_store_append_feedback(const char *username,
                               const char *account_id,
                               const char *feedback,
                               int semaphore_id,
                               char *response,
                               size_t response_capacity)
{
    if (username == NULL || account_id == NULL || feedback == NULL ||
        response == NULL || response_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t length = strlen(feedback);
    if (length == 0 || length > 700) {
        (void)snprintf(response, response_capacity,
                       "ERROR FEEDBACK_LENGTH_INVALID");
        return 0;
    }

    char safe_text[701];
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)feedback[i];
        safe_text[i] = (c < 32 || c == 127) ? ' ' : (char)c;
    }
    safe_text[length] = '\0';

    char line[1024];
    int line_length = snprintf(line, sizeof(line), "%lld %s %s %s\n",
                               (long long)time(NULL), username,
                               account_id, safe_text);

    if (line_length < 0 || (size_t)line_length >= sizeof(line)) {
        (void)snprintf(response, response_capacity,
                       "ERROR FEEDBACK_TOO_LONG");
        return 0;
    }

    if (change_semaphore(semaphore_id, -1) == -1) {
        (void)snprintf(response, response_capacity,
                       "ERROR FEEDBACK_LOCK_FAILED");
        return 0;
    }

    int fd = open(FEEDBACK_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
    int result = 0;

    if (fd == -1) {
        perror("ERROR: Cannot open feedback log");
        (void)snprintf(response, response_capacity,
                       "ERROR FEEDBACK_WRITE_FAILED");
        result = -1;
    } else {
        size_t done = 0;

        while (done < (size_t)line_length) {
            ssize_t n = write(fd, line + done, (size_t)line_length - done);
            if (n == -1) {
                if (errno == EINTR)
                    continue;
                result = -1;
                break;
            }
            if (n == 0) {
                errno = EIO;
                result = -1;
                break;
            }
            done += (size_t)n;
        }

        if (result == 0 && fsync(fd) == -1)
            result = -1;

        if (close(fd) == -1)
            result = -1;

        if (result == -1) {
            perror("ERROR: Feedback write outcome is uncertain");
            (void)snprintf(response, response_capacity,
                           "ERROR FEEDBACK_WRITE_UNCERTAIN");
        } else {
            (void)snprintf(response, response_capacity,
                           "FEEDBACK_SUBMITTED");
        }
    }

    if (change_semaphore(semaphore_id, +1) == -1) {
        perror("ERROR: Cannot release feedback semaphore");
        (void)snprintf(response, response_capacity,
                       "ERROR FEEDBACK_LOCK_RELEASE_FAILED");
        result = -1;
    }

    return result;
}

/* List feedback summaries in pages of eight. IDs are stable line numbers in append-only feedback.log. */
int user_store_list_feedback(int semaphore_id, unsigned long start_id,
                             char *response, size_t response_capacity)
{
    if (!response || response_capacity == 0 || start_id == 0) { errno = EINVAL; return -1; }
    if (change_semaphore(semaphore_id, -1) == -1) {
        (void)snprintf(response, response_capacity, "ERROR FEEDBACK_LOCK_FAILED"); return -1;
    }
    int result = 0;
    FILE *fp = fopen(FEEDBACK_FILE, "r");
    if (!fp) {
        if (errno == ENOENT) (void)snprintf(response, response_capacity, "FEEDBACK_LOG EMPTY");
        else { perror("ERROR: Cannot open feedback log"); (void)snprintf(response, response_capacity, "ERROR FEEDBACK_READ_FAILED"); result = -1; }
    } else {
        char line[1024]; unsigned long id = 0, next = 0; size_t shown = 0;
        int n = snprintf(response, response_capacity, "FEEDBACK_PAGE START=%lu\n", start_id);
        size_t used = n < 0 ? response_capacity : (size_t)n;
        if (used >= response_capacity) { (void)snprintf(response, response_capacity, "ERROR RESPONSE_TOO_SMALL"); result = -1; }
        else while (fgets(line, sizeof(line), fp)) {
            id++;
            size_t len = strlen(line);
            if (len == sizeof(line)-1 && line[len-1] != '\n') {
                (void)snprintf(response, response_capacity, "ERROR FEEDBACK_ENTRY_TOO_LONG"); result = -1; break;
            }
            if (id < start_id) continue;
            if (shown == 8) { next = id; break; }
            char ts[32] = "?", user[32] = "?", acct[16] = "?";
            if (sscanf(line, "%31s %31s %15s", ts, user, acct) != 3) {
                (void)snprintf(response, response_capacity, "ERROR INVALID_FEEDBACK_ENTRY"); result = -1; break;
            }
            n = snprintf(response + used, response_capacity - used,
                         "ID=%lu TIME=%s USER=%s ACCOUNT=%s\n", id, ts, user, acct);
            if (n < 0 || (size_t)n >= response_capacity - used) {
                (void)snprintf(response, response_capacity, "ERROR RESPONSE_TOO_SMALL"); result = -1; break;
            }
            used += (size_t)n; shown++;
        }
        if (fp && ferror(fp)) result = -1;
        if (fp && fclose(fp) == EOF) result = -1;
        if (result == 0 && shown == 0 && id < start_id)
            (void)snprintf(response, response_capacity, "FEEDBACK_PAGE_EMPTY TOTAL=%lu", id);
        else if (result == 0 && shown == 0 && id == 0)
            (void)snprintf(response, response_capacity, "FEEDBACK_LOG EMPTY");
        else if (result == 0 && next) {
            n = snprintf(response + used, response_capacity - used, "MORE: LIST_FEEDBACK %lu", next);
            if (n < 0 || (size_t)n >= response_capacity - used) { (void)snprintf(response, response_capacity, "ERROR RESPONSE_TOO_SMALL"); result = -1; }
        }
    }
    if (change_semaphore(semaphore_id, +1) == -1) {
        (void)snprintf(response, response_capacity, "ERROR FEEDBACK_UNLOCK_FAILED"); return -1;
    }
    return result;
}

/* View one feedback entry by its stable 1-based line number. */
int user_store_view_feedback(int semaphore_id, unsigned long feedback_id,
                             char *response, size_t response_capacity)
{
    if (!response || response_capacity == 0 || feedback_id == 0) { errno = EINVAL; return -1; }
    if (change_semaphore(semaphore_id, -1) == -1) {
        (void)snprintf(response, response_capacity, "ERROR FEEDBACK_LOCK_FAILED"); return -1;
    }
    int result = 0; FILE *fp = fopen(FEEDBACK_FILE, "r");
    if (!fp) (void)snprintf(response, response_capacity, "ERROR FEEDBACK_NOT_FOUND");
    else {
        char line[1024]; unsigned long id = 0; int found = 0;
        while (fgets(line, sizeof(line), fp)) {
            id++;
            if (id != feedback_id) continue;
            size_t len = strlen(line);
            if (len && line[len-1] == '\n') line[--len] = '\0';
            int n = snprintf(response, response_capacity, "FEEDBACK ID=%lu %s", id, line);
            if (n < 0 || (size_t)n >= response_capacity) (void)snprintf(response, response_capacity, "ERROR RESPONSE_TOO_SMALL");
            else found = 1;
            break;
        }
        if (ferror(fp)) result = -1;
        if (fclose(fp) == EOF) result = -1;
        if (result == 0 && !found) (void)snprintf(response, response_capacity, "ERROR FEEDBACK_NOT_FOUND");
    }
    if (change_semaphore(semaphore_id, +1) == -1) {
        (void)snprintf(response, response_capacity, "ERROR FEEDBACK_UNLOCK_FAILED"); return -1;
    }
    return result;
}

/* Read recent feedback while holding the shared feedback/log semaphore. */
int user_store_read_feedback(int semaphore_id,
                             char *response,
                             size_t response_capacity)
{
    if (response == NULL || response_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    if (change_semaphore(semaphore_id, -1) == -1) {
        perror("ERROR: Cannot lock feedback log");
        (void)snprintf(response, response_capacity,
                       "ERROR FEEDBACK_LOCK_FAILED");
        return -1;
    }

    enum { KEEP_LINES = 3, LINE_CAPACITY = 800 };
    char lines[KEEP_LINES][LINE_CAPACITY];
    size_t count = 0;
    size_t next = 0;
    int failed = 0;
    FILE *fp = fopen(FEEDBACK_FILE, "r");

    if (fp == NULL && errno != ENOENT) {
        perror("ERROR: Cannot open feedback log");
        failed = 1;
    } else if (fp != NULL) {
        char line[LINE_CAPACITY];

        while (fgets(line, sizeof(line), fp) != NULL) {
            if (strlen(line) == sizeof(line) - 1 &&
                line[sizeof(line) - 2] != '\n') {
                fprintf(stderr, "ERROR: Feedback entry exceeds supported size\n");
                failed = 1;
                break;
            }

            (void)snprintf(lines[next], sizeof(lines[next]), "%s", line);
            next = (next + 1) % KEEP_LINES;
            if (count < KEEP_LINES)
                count++;
        }

        if (ferror(fp)) {
            perror("ERROR: Cannot read feedback log");
            failed = 1;
        }
        if (fclose(fp) == EOF) {
            perror("ERROR: Cannot close feedback log");
            failed = 1;
        }
    }

    if (change_semaphore(semaphore_id, +1) == -1) {
        perror("ERROR: Cannot unlock feedback log");
        failed = 1;
    }

    if (failed) {
        (void)snprintf(response, response_capacity,
                       "ERROR FEEDBACK_READ_FAILED");
        return -1;
    }

    if (count == 0) {
        (void)snprintf(response, response_capacity, "FEEDBACK_LOG EMPTY");
        return 0;
    }

    size_t used = (size_t)snprintf(response, response_capacity,
                                   "FEEDBACK_LOG (LATEST FIRST)\n");
    if (used >= response_capacity) {
        (void)snprintf(response, response_capacity,
                       "ERROR RESPONSE_TOO_SMALL");
        return 0;
    }

    /* Present the newest entries first; omit older entries if space runs out. */
    for (size_t i = 0; i < count; i++) {
        size_t index = (next + KEEP_LINES - 1 - i) % KEEP_LINES;
        size_t len = strlen(lines[index]);

        if (used + len >= response_capacity)
            break;

        memcpy(response + used, lines[index], len);
        used += len;
        response[used] = '\0';
    }

    return 0;
}


/* Admin reset: replace one fixed-width credential record while holding the file lock. */
int user_store_reset_password(const char *username, const char *new_password)
{
    if (username == NULL || new_password == NULL) {
        errno = EINVAL;
        return -1;
    }
    char replacement[CREDENTIAL_RECORD_SIZE];
    if (make_record(replacement, username, new_password) == -1)
        return -1;

    int fd = open(CREDENTIAL_FILE, O_RDWR);
    if (fd == -1)
        return -1;
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR) continue;
        int e = errno; close(fd); errno = e; return -1;
    }

    int result = -1;
    struct stat info;
    if (fstat(fd, &info) == -1 || info.st_size < 0 ||
        info.st_size % CREDENTIAL_RECORD_SIZE != 0)
        goto reset_done;
    char existing[CREDENTIAL_RECORD_SIZE];
    size_t count = (size_t)(info.st_size / CREDENTIAL_RECORD_SIZE);
    for (size_t i = 0; i < count; i++) {
        off_t off = (off_t)(i * CREDENTIAL_RECORD_SIZE);
        if (pread_full(fd, existing, sizeof(existing), off) == -1)
            goto reset_done;
        if (record_matches_user(existing, username)) {
            if (pwrite_full(fd, replacement, sizeof(replacement), off) == -1 ||
                fsync(fd) == -1)
                goto reset_done;
            result = 0;
            break;
        }
    }
    if (result == -1) errno = ENOENT;
reset_done:
    {
        int saved = errno;
        lock.l_type = F_UNLCK;
        if (fcntl(fd, F_SETLK, &lock) == -1 && result == 0) {
            saved = errno; result = -1;
        }
        if (close(fd) == -1 && result == 0) {
            saved = errno; result = -1;
        }
        errno = saved;
    }
    return result;
}

/* Remove a credential record. A temporary fixed-record file is renamed atomically. */
int user_store_remove_password(const char *username)
{
    if (username == NULL) { errno = EINVAL; return -1; }
    int fd = open(CREDENTIAL_FILE, O_RDWR);
    if (fd == -1) return -1;

    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET;
    lock.l_start = 0; lock.l_len = 0;
    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR) continue;
        int e = errno; close(fd); errno = e; return -1;
    }

    int temp = -1, result = -1, found = 0;
    struct stat info;
    if (fstat(fd, &info) == -1 || info.st_size < 0 ||
        info.st_size % CREDENTIAL_RECORD_SIZE != 0)
        goto store_remove_done;
    temp = open("credentials.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (temp == -1) goto store_remove_done;

    char record[CREDENTIAL_RECORD_SIZE];
    size_t count = (size_t)(info.st_size / CREDENTIAL_RECORD_SIZE);
    for (size_t i = 0; i < count; i++) {
        off_t off = (off_t)(i * CREDENTIAL_RECORD_SIZE);
        if (pread_full(fd, record, sizeof(record), off) == -1)
            goto store_remove_done;
        if (record_matches_user(record, username)) {
            found = 1;
            continue;
        }
        size_t done = 0;
        while (done < sizeof(record)) {
            ssize_t n = write(temp, record + done, sizeof(record) - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                goto store_remove_done;
            }
            if (n == 0) { errno = EIO; goto store_remove_done; }
            done += (size_t)n;
        }
    }
    if (!found) { errno = ENOENT; goto store_remove_done; }
    if (fsync(temp) == -1) goto store_remove_done;
    if (close(temp) == -1) { temp = -1; goto store_remove_done; }
    temp = -1;
    result = 0;
store_remove_done:
    {
        int saved = errno;
        if (temp >= 0 && close(temp) == -1 && result == 0) {
            saved = errno; result = -1;
        }
        lock.l_type = F_UNLCK;
        if (fcntl(fd, F_SETLK, &lock) == -1 && result == 0) {
            saved = errno; result = -1;
        }
        if (close(fd) == -1 && result == 0) {
            saved = errno; result = -1;
        }
        errno = saved;
    }
    if (result == 0) {
        if (rename("credentials.tmp", CREDENTIAL_FILE) == -1) {
            int e = errno; unlink("credentials.tmp"); errno = e; return -1;
        }
    } else {
        unlink("credentials.tmp");
    }
    return result;
}
