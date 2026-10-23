#define _POSIX_C_SOURCE 200809L

#include "user_registry.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REGISTRY_FILE "user_registry.txt"
#define REGISTRY_LINE_SIZE 128

static const struct RegistryUser defaults[] = {
    {"alice",    "000001", "CUSTOMER", 0},
    {"bob",      "000002", "CUSTOMER", 1},
    {"manager",  "000000", "MANAGER",  2},
    {"employee", "000000", "EMPLOYEE", 3},
    {"admin",    "000000", "ADMIN",    4}
};

static int lock_file(int fd, short type)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    int cmd = type == F_UNLCK ? F_SETLK : F_SETLKW;
    while (fcntl(fd, cmd, &lock) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int valid_username(const char *value)
{
    size_t n;

    if (value == NULL)
        return 0;
    n = strlen(value);
    if (n == 0 || n >= REGISTRY_USERNAME_SIZE)
        return 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!isalnum(c) && c != '_' && c != '-')
            return 0;
    }
    return 1;
}

static int valid_role(const char *role)
{
    return role != NULL &&
           (strcmp(role, "CUSTOMER") == 0 ||
            strcmp(role, "EMPLOYEE") == 0 ||
            strcmp(role, "MANAGER") == 0 ||
            strcmp(role, "ADMIN") == 0);
}

static int write_full(int fd, const char *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t written = write(fd, p + done, n - done);
        if (written == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)written;
    }
    return 0;
}

/* Create the initial directory only if the registry doesn't exist or is empty. */
int user_registry_init(void)
{
    int fd = open(REGISTRY_FILE, O_RDWR | O_CREAT, 0600);
    if (fd == -1)
        return -1;

    int result = -1;
    struct stat info;

    if (lock_file(fd, F_WRLCK) == -1)
        goto done;

    if (fstat(fd, &info) == -1)
        goto unlock;

    if (info.st_size == 0) {
        for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
            char line[REGISTRY_LINE_SIZE];
            int n = snprintf(line, sizeof(line), "%s|%s|%s|%lld\n",
                             defaults[i].username,
                             defaults[i].account_id,
                             defaults[i].role,
                             (long long)defaults[i].lock_offset);
            if (n < 0 || (size_t)n >= sizeof(line) ||
                write_full(fd, line, (size_t)n) == -1)
                goto unlock;
        }
        if (fsync(fd) == -1)
            goto unlock;
    }

    result = 0;

unlock:
    {
        int saved = errno;
        if (lock_file(fd, F_UNLCK) == -1 && result == 0) {
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

static int parse_line(char *line, struct RegistryUser *user)
{
    char *save = NULL;
    char *username = strtok_r(line, "|\n", &save);
    char *account_id = strtok_r(NULL, "|\n", &save);
    char *role = strtok_r(NULL, "|\n", &save);
    char *offset_text = strtok_r(NULL, "|\n", &save);
    char *extra = strtok_r(NULL, "|\n", &save);

    if (username == NULL || account_id == NULL || role == NULL ||
        offset_text == NULL || extra != NULL ||
        !valid_username(username) || strlen(account_id) != 6 ||
        !valid_role(role))
        return -1;

    for (int i = 0; i < 6; i++) {
        if (!isdigit((unsigned char)account_id[i]))
            return -1;
    }

    char *end = NULL;
    errno = 0;
    long long offset = strtoll(offset_text, &end, 10);
    if (errno != 0 || end == offset_text || *end != '\0' || offset < 0)
        return -1;

    memset(user, 0, sizeof(*user));
    memcpy(user->username, username, strlen(username) + 1);
    memcpy(user->account_id, account_id, 7);
    memcpy(user->role, role, strlen(role) + 1);
    user->lock_offset = (off_t)offset;
    return 0;
}

int user_registry_load(struct RegistryUser users[],
                       size_t capacity,
                       size_t *count)
{
    if (users == NULL || count == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    FILE *fp = fopen(REGISTRY_FILE, "r");
    if (fp == NULL)
        return -1;

    *count = 0;
    int result = 0;
    char line[REGISTRY_LINE_SIZE];

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);

        if (len == 0 || line[len - 1] != '\n') {
            fprintf(stderr, "ERROR: User registry line is too long or truncated\n");
            result = -1;
            break;
        }

        if (*count >= capacity) {
            fprintf(stderr, "ERROR: User registry capacity exceeded\n");
            result = -1;
            break;
        }

        if (parse_line(line, &users[*count]) == -1) {
            fprintf(stderr, "ERROR: Invalid user registry entry\n");
            result = -1;
            break;
        }

        for (size_t j = 0; j < *count; j++) {
            if (strcmp(users[j].username, users[*count].username) == 0) {
                fprintf(stderr, "ERROR: Duplicate username in registry\n");
                result = -1;
                break;
            }
        }
        if (result == -1)
            break;

        (*count)++;
    }

    if (ferror(fp))
        result = -1;
    if (fclose(fp) == EOF)
        result = -1;

    return result;
}

/* Add a user if the name is not already present. */
int user_registry_add(const char *username,
                      const char *account_id,
                      const char *role,
                      off_t lock_offset)
{
    if (!valid_username(username) || account_id == NULL ||
        strlen(account_id) != 6 || !valid_role(role) || lock_offset < 0) {
        errno = EINVAL;
        return -1;
    }

    struct RegistryUser users[USER_REGISTRY_MAX];
    size_t count = 0;

    if (user_registry_load(users, USER_REGISTRY_MAX, &count) == -1)
        return -1;

    for (size_t i = 0; i < count; i++) {
        if (strcmp(users[i].username, username) == 0) {
            errno = EEXIST;
            return -1;
        }
        if (users[i].lock_offset == lock_offset) {
            errno = EEXIST;
            return -1;
        }
    }

    if (count >= USER_REGISTRY_MAX) {
        errno = ENOSPC;
        return -1;
    }

    int fd = open(REGISTRY_FILE, O_WRONLY | O_APPEND);
    if (fd == -1)
        return -1;

    int result = -1;
    if (lock_file(fd, F_WRLCK) == -1)
        goto done;

    char line[REGISTRY_LINE_SIZE];
    int n = snprintf(line, sizeof(line), "%s|%s|%s|%lld\n",
                     username, account_id, role, (long long)lock_offset);

    if (n >= 0 && (size_t)n < sizeof(line) &&
        write_full(fd, line, (size_t)n) == 0 && fsync(fd) == 0)
        result = 0;

    if (lock_file(fd, F_UNLCK) == -1)
        result = -1;

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


/* Remove a user from the login registry using an atomic temp-file replacement.
 * Account transaction history is preserved; this only removes login access.
 */
int user_registry_remove(const char *username)
{
    if (!valid_username(username)) {
        errno = EINVAL;
        return -1;
    }

    struct RegistryUser users[USER_REGISTRY_MAX];
    size_t count = 0;
    if (user_registry_load(users, USER_REGISTRY_MAX, &count) == -1)
        return -1;

    size_t remove_index = count;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(users[i].username, username) == 0) {
            remove_index = i;
            break;
        }
    }
    if (remove_index == count) {
        errno = ENOENT;
        return -1;
    }

    int fd = open("user_registry.delete.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd == -1)
        return -1;

    int result = -1;
    for (size_t i = 0; i < count; i++) {
        if (i == remove_index)
            continue;
        char line[REGISTRY_LINE_SIZE];
        int n = snprintf(line, sizeof(line), "%s|%s|%s|%lld\n",
                         users[i].username, users[i].account_id,
                         users[i].role, (long long)users[i].lock_offset);
        if (n < 0 || (size_t)n >= sizeof(line) ||
            write_full(fd, line, (size_t)n) == -1)
            goto remove_done;
    }
    if (fsync(fd) == -1)
        goto remove_done;
    result = 0;
remove_done:
    {
        int saved = errno;
        if (close(fd) == -1 && result == 0) {
            saved = errno;
            result = -1;
        }
        errno = saved;
    }
    if (result == -1) {
        (void)unlink("user_registry.delete.tmp");
        return -1;
    }
    if (rename("user_registry.delete.tmp", REGISTRY_FILE) == -1) {
        int saved = errno;
        (void)unlink("user_registry.delete.tmp");
        errno = saved;
        return -1;
    }
    return 0;
}

/* Rewrite the target role while preserving all other directory entries. */
int user_registry_set_role(const char *username, const char *role)
{
    if (!valid_username(username) || !valid_role(role)) {
        errno = EINVAL;
        return -1;
    }

    struct RegistryUser users[USER_REGISTRY_MAX];
    size_t count = 0;
    if (user_registry_load(users, USER_REGISTRY_MAX, &count) == -1)
        return -1;

    int found = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(users[i].username, username) == 0) {
            memcpy(users[i].role, role, strlen(role) + 1);
            found = 1;
            break;
        }
    }
    if (!found) {
        errno = ENOENT;
        return -1;
    }

    int fd = open("user_registry.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd == -1)
        return -1;

    int result = 0;
    for (size_t i = 0; i < count; i++) {
        char line[REGISTRY_LINE_SIZE];
        int n = snprintf(line, sizeof(line), "%s|%s|%s|%lld\n",
                         users[i].username, users[i].account_id,
                         users[i].role, (long long)users[i].lock_offset);
        if (n < 0 || (size_t)n >= sizeof(line) ||
            write_full(fd, line, (size_t)n) == -1) {
            result = -1;
            break;
        }
    }

    if (result == 0 && fsync(fd) == -1)
        result = -1;
    if (close(fd) == -1)
        result = -1;

    if (result == 0 && rename("user_registry.tmp", REGISTRY_FILE) == -1)
        result = -1;
    if (result == -1)
        (void)unlink("user_registry.tmp");

    return result;
}
