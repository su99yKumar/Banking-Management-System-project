
#define _GNU_SOURCE

#include "bank_core.h"
#include "loan_core.h"
#include "user_store.h"
#include "user_registry.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>

#define SERVER_PORT 5053
#define MAX_FRAME_SIZE 1024

#define ACCOUNT_FILE "users_network_test.txt"
#define LOAN_FILE "loans_network.txt"
#define SESSION_LOCK_FILE "session_locks.dat"
#define TRANSACTION_LOG "transactions.log"
#define LOAN_ASSIGNMENTS_FILE "loan_assignments.txt"
#define EMPLOYEE_DETAILS_FILE "employee_details.txt"

#define MAX_USERS USER_REGISTRY_MAX
#define DEFAULT_USER_COUNT 5

/*
 * Each configured user has a role and a unique session-lock byte.
 * Account ID 000000 is reserved for non-customer demo identities.
 */
struct User {
    const char *username;
    const char *password;
    const char *account_id;
    const char *role;
    off_t lock_offset;
};

/* Demo credentials used only to initialize a new credential file. */
static const struct User default_users[DEFAULT_USER_COUNT] = {
    {"alice",    "Alice123!",    "000001", "CUSTOMER", 0},
    {"bob",      "Bob123!",      "000002", "CUSTOMER", 1},
    {"manager",  "Manager123!",  "000000", "MANAGER",  2},
    {"employee", "Employee123!", "000000", "EMPLOYEE", 3},
    {"admin",    "Admin123!",    "000000", "ADMIN",    4}
};

static struct RegistryUser registry_users[MAX_USERS];
static struct User users[MAX_USERS];
static size_t user_count = 0;


/* Write a complete buffer, retrying interrupted/partial writes. */
static int profile_write_all(int fd, const char *buffer, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, buffer + done, length - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}

/* Validate display text so the line-based file cannot be corrupted. */
static int profile_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) > 80) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p == '|' || *p == '\n' || *p == '\r' || *p < 32) return 0;
    return 1;
}

/* Safely replace/add one employee display name in employee_details.txt. */
static int employee_details_update(const char *username, const char *display_name)
{
    if (username == NULL || display_name == NULL || !profile_name_valid(display_name)) {
        errno = EINVAL;
        return -1;
    }
    int lock_fd = open("employee_details.lock", O_RDWR | O_CREAT, 0600);
    if (lock_fd == -1) return -1;
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_start = 0; lock.l_len = 0;
    while (fcntl(lock_fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR) continue;
        int saved = errno; close(lock_fd); errno = saved; return -1;
    }

    char names[USER_REGISTRY_MAX][32];
    char values[USER_REGISTRY_MAX][81];
    size_t count = 0;
    int found = 0, result = -1;
    FILE *fp = fopen(EMPLOYEE_DETAILS_FILE, "r");
    if (fp != NULL) {
        char line[160];
        while (fgets(line, sizeof(line), fp) != NULL) {
            size_t len = strlen(line);
            if (len == 0 || line[len - 1] != '\n') { errno = EINVAL; goto update_done; }
            line[len - 1] = '\0';
            char *bar = strchr(line, '|');
            if (bar == NULL || bar == line || strlen(bar + 1) > 80 || count >= USER_REGISTRY_MAX) {
                errno = EINVAL; goto update_done;
            }
            *bar = '\0';
            if (strlen(line) >= sizeof(names[0])) { errno = EINVAL; goto update_done; }
            size_t j;
            for (j = 0; j < count; j++) if (strcmp(names[j], line) == 0) break;
            if (j < count) {
                (void)snprintf(values[j], sizeof(values[j]), "%s", bar + 1);
                continue;
            }
            memcpy(names[count], line, strlen(line) + 1);
            (void)snprintf(values[count], sizeof(values[count]), "%s", bar + 1);
            count++;
        }
        if (ferror(fp)) { errno = EIO; goto update_done; }
        if (fclose(fp) == EOF) { fp = NULL; goto update_done; }
        fp = NULL;
    } else if (errno != ENOENT) {
        goto update_done;
    }

    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], username) == 0) {
            (void)snprintf(values[i], sizeof(values[i]), "%s", display_name);
            found = 1;
            break;
        }
    }
    if (!found) {
        if (count >= USER_REGISTRY_MAX) { errno = ENOSPC; goto update_done; }
        (void)snprintf(names[count], sizeof(names[count]), "%s", username);
        (void)snprintf(values[count], sizeof(values[count]), "%s", display_name);
        count++;
    }

    int temp_fd = open("employee_details.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (temp_fd == -1) goto update_done;
    for (size_t i = 0; i < count; i++) {
        char line[160];
        int n = snprintf(line, sizeof(line), "%s|%s\n", names[i], values[i]);
        if (n < 0 || (size_t)n >= sizeof(line) || profile_write_all(temp_fd, line, (size_t)n) == -1) {
            int saved = errno; close(temp_fd); unlink("employee_details.tmp"); errno = saved; goto update_done;
        }
    }
    if (fsync(temp_fd) == -1) {
        int saved = errno; close(temp_fd); unlink("employee_details.tmp"); errno = saved; goto update_done;
    }
    if (close(temp_fd) == -1) { unlink("employee_details.tmp"); goto update_done; }
    if (rename("employee_details.tmp", EMPLOYEE_DETAILS_FILE) == -1) {
        int saved = errno; unlink("employee_details.tmp"); errno = saved; goto update_done;
    }
    result = 0;

update_done:
    {
        int saved = errno;
        if (fp != NULL) (void)fclose(fp);
        lock.l_type = F_UNLCK;
        if (fcntl(lock_fd, F_SETLK, &lock) == -1 && result == 0) { saved = errno; result = -1; }
        if (close(lock_fd) == -1 && result == 0) { saved = errno; result = -1; }
        errno = saved;
    }
    return result;
}

/* Return a saved display name, or indicate that one has not been set. */
static int employee_details_read(const char *username, char *out, size_t capacity)
{
    if (username == NULL || out == NULL || capacity == 0) { errno = EINVAL; return -1; }
    FILE *fp = fopen(EMPLOYEE_DETAILS_FILE, "r");
    if (fp == NULL && errno == ENOENT) {
        (void)snprintf(out, capacity, "NOT_SET");
        return 0;
    }
    if (fp == NULL) return -1;
    char line[160];
    int result = 0, found = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *bar = strchr(line, '|');
        if (bar == NULL) continue;
        *bar++ = '\0';
        bar[strcspn(bar, "\r\n")] = '\0';
        if (strcmp(line, username) == 0) {
            if (snprintf(out, capacity, "%s", bar) >= (int)capacity) { errno = ENOSPC; result = -1; }
            found = 1;
            break;
        }
    }
    if (ferror(fp)) result = -1;
    if (fclose(fp) == EOF) result = -1;
    if (result == 0 && !found) (void)snprintf(out, capacity, "NOT_SET");
    return result;
}

static int refresh_users(void)
{
    size_t count = 0;
    if (user_registry_load(registry_users, MAX_USERS, &count) == -1)
        return -1;

    for (size_t i = 0; i < count; i++) {
        users[i].username = registry_users[i].username;
        users[i].password = NULL;
        users[i].account_id = registry_users[i].account_id;
        users[i].role = registry_users[i].role;
        users[i].lock_offset = registry_users[i].lock_offset;
    }
    user_count = count;
    return 0;
}

static volatile sig_atomic_t stop_server = 0;
static int transaction_semid = -1;

/* Serialize shared-state command handlers while client sockets remain concurrent. */
static pthread_mutex_t operation_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clients_cond = PTHREAD_COND_INITIALIZER;
static size_t active_clients = 0;

struct client_thread_arg {
    int client_fd;
};

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/* Ask the parent process to stop accepting clients. */
static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_server = 1;
}

/* Install the server's signal handlers. */
static int install_signal_handlers(void)
{
    struct sigaction stop_action;
    memset(&stop_action, 0, sizeof(stop_action));
    stop_action.sa_handler = handle_stop_signal;
    sigemptyset(&stop_action.sa_mask);
    stop_action.sa_flags = 0;

    if (sigaction(SIGINT, &stop_action, NULL) == -1)
        return -1;

    if (sigaction(SIGTERM, &stop_action, NULL) == -1)
        return -1;

    return 0;
}

/* Load persistent identities and roles before each new login. */
static const struct User *find_user(const char *username)
{
    if (refresh_users() == -1) {
        perror("ERROR: Cannot load user registry");
        return NULL;
    }

    for (size_t i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0)
            return &users[i];
    }
    return NULL;
}

/* Send all bytes, handling partial sends and interrupted calls. */
static int send_all(int fd, const void *buffer, size_t length)
{
    const char *p = buffer;
    size_t total = 0;

    while (total < length) {
        ssize_t n = send(fd, p + total, length - total, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            if (n == 0)
                errno = EPIPE;
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}

/*
 * Receive exactly length bytes.
 * Returns 1 on success, 0 for EOF before any bytes, -1 on error.
 */
static int recv_exact(int fd, void *buffer, size_t length)
{
    char *p = buffer;
    size_t total = 0;

    while (total < length) {
        ssize_t n = recv(fd, p + total, length - total, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n == 0) {
            if (total == 0)
                return 0;

            errno = ECONNRESET;
            return -1;
        }

        if (n < 0)
            return -1;

        total += (size_t)n;
    }

    return 1;
}

/*
 * Send a TCP application frame:
 * 4-byte network-order length followed by the message.
 */
static int send_frame(int fd, const char *message)
{
    size_t length = strlen(message);

    if (length == 0 || length > MAX_FRAME_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    uint32_t network_length = htonl((uint32_t)length);

    if (send_all(fd, &network_length, sizeof(network_length)) == -1)
        return -1;

    return send_all(fd, message, length);
}

/* Receive and validate one length-prefixed text message. */
static int recv_frame(int fd, char *buffer, size_t capacity)
{
    uint32_t network_length;

    int result = recv_exact(fd, &network_length,
                            sizeof(network_length));

    if (result != 1)
        return result;

    uint32_t length = ntohl(network_length);

    if (length == 0 ||
        length > MAX_FRAME_SIZE ||
        (size_t)length >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    result = recv_exact(fd, buffer, length);

    if (result != 1) {
        if (result == 0)
            errno = ECONNRESET;
        return -1;
    }

    /* Commands are text, so embedded NUL bytes are not accepted. */
    if (memchr(buffer, '\0', length) != NULL) {
        errno = EPROTO;
        return -1;
    }

    buffer[length] = '\0';
    return 1;
}

/* Parse exactly: LOGIN username password. */
static int parse_login(const char *message,
                       char *username,
                       char *password)
{
    char operation[16];
    char extra;

    int fields = sscanf(message, "%15s %31s %63s %c",
                        operation, username, password, &extra);

    if (fields != 3 || strcmp(operation, "LOGIN") != 0)
        return -1;

    return 0;
}

/*
 * Acquire this user's non-blocking session lock.
 * Each user owns a separate byte in SESSION_LOCK_FILE.
 */
static int acquire_session_lock(const struct User *user)
{
    int fd = open(SESSION_LOCK_FILE, O_RDWR | O_CREAT, 0600);

    if (fd == -1)
        return -1;

    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = user->lock_offset;
    lock.l_len = 1;

    while (fcntl(fd, F_OFD_SETLK, &lock) == -1) {
        if (errno == EINTR)
            continue;

        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

/* Release this user's lock and close the lock-file descriptor. */
static int release_session_lock(int fd, const struct User *user)
{
    int result = 0;
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = user->lock_offset;
    lock.l_len = 1;

    while (fcntl(fd, F_OFD_SETLK, &lock) == -1) {
        if (errno == EINTR)
            continue;

        result = -1;
        break;
    }

    if (close(fd) == -1)
        result = -1;

    return result;
}

/* Send an error response to a client. */
static int send_error(int client_fd, const char *message)
{
    if (send_frame(client_fd, message) == -1) {
        perror("ERROR: Cannot send error response");
        return -1;
    }

    return 0;
}

/*
 * Build a list of usernames and roles.
 * Never include passwords, account balances, or other account data.
 */
static int build_user_directory(char *response, size_t capacity)
{
    if (refresh_users() == -1)
        return -1;

    int n = snprintf(response, capacity, "USERS %zu", user_count);
    if (n < 0 || (size_t)n >= capacity)
        return -1;

    size_t used = (size_t)n;
    for (size_t i = 0; i < user_count; i++) {
        size_t remaining = capacity - used;
        n = snprintf(response + used, remaining,
                     "\n%s ROLE=%s",
                     users[i].username, users[i].role);
        if (n < 0 || (size_t)n >= remaining)
            return -1;
        used += (size_t)n;
    }
    return 0;
}

/*
 * Handle one authenticated client session.
 * Roles are checked here, on the server, rather than trusted to the client.
 */

/* Read one complete fixed-width loan record by ID. Returns 1 found, 0 absent, -1 error. */
static int server_find_loan(const char *loan_id, char out[128])
{
    int fd = open(LOAN_FILE, O_RDONLY);
    if (fd == -1) return -1;
    struct stat st;
    if (fstat(fd, &st) == -1 || st.st_size < 0 || st.st_size % 128 != 0) {
        close(fd); errno = EINVAL; return -1;
    }
    int found = 0;
    for (off_t off = 0; off < st.st_size; off += 128) {
        size_t done = 0;
        while (done < 128) {
            ssize_t n = pread(fd, out + done, 128 - done, off + (off_t)done);
            if (n < 0) { if (errno == EINTR) continue; close(fd); return -1; }
            if (n == 0) { close(fd); errno = EIO; return -1; }
            done += (size_t)n;
        }
        if (memcmp(out, loan_id, 6) == 0) { found = 1; break; }
    }
    int saved = errno;
    if (close(fd) == -1 && found >= 0) return -1;
    errno = saved;
    return found;
}

/* Find the most recent assignment for a loan. */
static int loan_assignment_matches(const char *loan_id, const char *username)
{
    FILE *fp = fopen(LOAN_ASSIGNMENTS_FILE, "r");
    if (fp == NULL) return errno == ENOENT ? 0 : -1;
    char line[128], id[7], owner[32], latest[32] = "";
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "%6[0-9]|%31[^\n]", id, owner) == 2 &&
            strcmp(id, loan_id) == 0)
            (void)snprintf(latest, sizeof(latest), "%s", owner);
    }
    if (ferror(fp)) { fclose(fp); return -1; }
    if (fclose(fp) == EOF) return -1;
    return strcmp(latest, username) == 0;
}

/* Manager assigns a pending loan. Later assignment entries replace earlier ones. */
static int assign_pending_loan(const char *loan_id, const char *employee)
{
    if (strlen(loan_id) != 6) { errno = EINVAL; return -2; }
    for (int i = 0; i < 6; i++)
        if (loan_id[i] < '0' || loan_id[i] > '9') { errno = EINVAL; return -2; }
    char record[128];
    int found = server_find_loan(loan_id, record);
    if (found <= 0) return found == 0 ? -3 : -1;
    if (record[31] != 'P') return -4;

    int fd = open(LOAN_ASSIGNMENTS_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd == -1) return -1;
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_start = 0; lock.l_len = 0;
    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR) continue;
        int e = errno; close(fd); errno = e; return -1;
    }
    char line[128];
    int n = snprintf(line, sizeof(line), "%s|%s\n", loan_id, employee);
    int result = 0;
    if (n < 0 || (size_t)n >= sizeof(line)) { errno = EOVERFLOW; result = -1; }
    else {
        size_t done = 0;
        while (done < (size_t)n) {
            ssize_t wrote = write(fd, line + done, (size_t)n - done);
            if (wrote < 0) { if (errno == EINTR) continue; result = -1; break; }
            if (wrote == 0) { errno = EIO; result = -1; break; }
            done += (size_t)wrote;
        }
        if (result == 0 && fsync(fd) == -1) result = -1;
    }
    int saved = errno;
    lock.l_type = F_UNLCK;
    if (fcntl(fd, F_SETLK, &lock) == -1 && result == 0) { result = -1; saved = errno; }
    if (close(fd) == -1 && result == 0) { result = -1; saved = errno; }
    errno = saved;
    return result;
}

/* List this employee's currently assigned applications with their key fields. */
static int list_assigned_loans(const char *username, char *response, size_t cap)
{
    FILE *fp = fopen(LOAN_ASSIGNMENTS_FILE, "r");
    char ids[USER_REGISTRY_MAX][7];
    char owners[USER_REGISTRY_MAX][32];
    size_t count = 0;
    if (fp == NULL && errno != ENOENT) return -1;
    if (fp != NULL) {
        char line[128], id[7], owner[32];
        while (fgets(line, sizeof(line), fp) != NULL) {
            if (sscanf(line, "%6[0-9]|%31[^\n]", id, owner) != 2) continue;
            size_t i;
            for (i = 0; i < count; i++) if (strcmp(ids[i], id) == 0) break;
            if (i == count) {
                if (count >= USER_REGISTRY_MAX) { fclose(fp); errno = ENOSPC; return -1; }
                memcpy(ids[count], id, 7); i = count++;
            }
            (void)snprintf(owners[i], sizeof(owners[i]), "%s", owner);
        }
        if (ferror(fp)) { fclose(fp); return -1; }
        if (fclose(fp) == EOF) return -1;
    }

    int n = snprintf(response, cap, "ASSIGNED_LOANS");
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t used = (size_t)n;
    int shown = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(owners[i], username) != 0) continue;
        char loan[128];
        int found = server_find_loan(ids[i], loan);
        if (found != 1) continue;
        long long amount = 0;
        for (int j = 14; j < 26; j++) {
            if (loan[j] < '0' || loan[j] > '9') { amount = -1; break; }
            amount = amount * 10 + (loan[j] - '0');
        }
        const char *status = loan[31] == 'P' ? "PENDING" :
                             loan[31] == 'A' ? "APPROVED" :
                             loan[31] == 'R' ? "REJECTED" : "UNKNOWN";
        n = snprintf(response + used, cap - used,
                     "\nLOAN=%s ACCOUNT=%.6s AMOUNT=%lld.%02lld TERM=%.3s STATUS=%s",
                     ids[i], loan + 7, amount < 0 ? 0 : amount / 100,
                     amount < 0 ? 0 : amount % 100, loan + 27, status);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
        shown++;
    }
    if (shown == 0) {
        n = snprintf(response + used, cap - used, "\nNONE");
        if (n < 0 || (size_t)n >= cap - used) return -1;
    }
    return 0;
}

static void handle_client(int client_fd)
{
    char message[MAX_FRAME_SIZE + 1];
    char response[MAX_FRAME_SIZE + 1];
    char username[32];
    char password[64];

    const struct User *user = NULL;
    struct User session_user;
    char session_username[32] = {0};
    char session_account_id[7] = {0};
    char session_role[16] = {0};
    int session_lock_fd = -1;

    int result = recv_frame(client_fd, message, sizeof(message));

    if (result != 1) {
        if (result == -1)
            fprintf(stderr, "ERROR: Could not receive login request\n");
        goto cleanup;
    }

    if (parse_login(message, username, password) == -1) {
        send_error(client_fd, "ERROR INVALID_LOGIN_REQUEST");
        goto cleanup;
    }

    /* Copy the identity while protected; do not retain pointers into shared registry arrays. */
    pthread_mutex_lock(&operation_mutex);
    const struct User *loaded_user = find_user(username);
    if (loaded_user != NULL) {
        (void)snprintf(session_username, sizeof(session_username), "%s",
                       loaded_user->username);
        (void)snprintf(session_account_id, sizeof(session_account_id), "%s",
                       loaded_user->account_id);
        (void)snprintf(session_role, sizeof(session_role), "%s",
                       loaded_user->role);
        session_user.username = session_username;
        session_user.password = NULL;
        session_user.account_id = session_account_id;
        session_user.role = session_role;
        session_user.lock_offset = loaded_user->lock_offset;
        user = &session_user;
    }
    int password_ok = user != NULL &&
                      user_store_password_matches(username, password) == 1;
    pthread_mutex_unlock(&operation_mutex);

    if (!password_ok) {
        send_error(client_fd, "ERROR INVALID_CREDENTIALS");
        goto cleanup;
    }

    if (strcmp(user->role, "CUSTOMER") == 0) {
        pthread_mutex_lock(&operation_mutex);
        int active = bank_account_is_active(ACCOUNT_FILE, user->account_id);
        pthread_mutex_unlock(&operation_mutex);
        if (active != 1) {
            if (active == 0)
                (void)send_error(client_fd, "ERROR ACCOUNT_DEACTIVATED");
            else {
                perror("ERROR: Cannot verify account status");
                (void)send_error(client_fd, "ERROR ACCOUNT_STATUS_UNAVAILABLE");
            }
            goto cleanup;
        }
    }

    if (strcmp(user->role, "CUSTOMER") == 0) {
        pthread_mutex_lock(&operation_mutex);
        int active = bank_account_is_active(ACCOUNT_FILE, user->account_id);
        pthread_mutex_unlock(&operation_mutex);
        if (active != 1) {
            if (active == 0)
                (void)send_error(client_fd, "ERROR ACCOUNT_DEACTIVATED");
            else {
                perror("ERROR: Cannot verify account status");
                (void)send_error(client_fd, "ERROR ACCOUNT_STATUS_UNAVAILABLE");
            }
            goto cleanup;
        }
    }

    session_lock_fd = acquire_session_lock(user);

    if (session_lock_fd == -1) {
        if (errno == EACCES || errno == EAGAIN) {
            send_error(client_fd, "ERROR SESSION_ALREADY_ACTIVE");
        } else {
            perror("ERROR: Cannot acquire session lock");
            send_error(client_fd, "ERROR SESSION_LOCK_FAILED");
        }
        goto cleanup;
    }

    int n = snprintf(response, sizeof(response),
                     "LOGIN_OK ROLE=%s", user->role);

    if (n < 0 || (size_t)n >= sizeof(response)) {
        send_error(client_fd, "ERROR SERVER_ERROR");
        goto cleanup;
    }

    if (send_frame(client_fd, response) == -1) {
        perror("ERROR: Cannot send login response");
        goto cleanup;
    }

    printf("User '%s' logged in with role %s (thread %lu)\n",
           user->username, user->role, (unsigned long)pthread_self());
    fflush(stdout);

    for (;;) {
        result = recv_frame(client_fd, message, sizeof(message));

        if (result == 0)
            break;

        if (result == -1) {
            fprintf(stderr, "Thread %lu: failed to receive command\n",
                    (unsigned long)pthread_self());
            break;
        }

        if (strcmp(message, "LOGOUT") == 0 || strcmp(message, "EXIT") == 0) {
            if (send_frame(client_fd, "BYE") == -1)
                perror("ERROR: Cannot send logout response");
            break;
        }

        /* Protect registry and file-backed command processing from other threads. */
        pthread_mutex_lock(&operation_mutex);

        int is_apply =
            strncmp(message, "APPLY_LOAN ", 11) == 0;
        int is_status =
            strcmp(message, "LOAN_STATUS") == 0;
        int is_approve =
            strncmp(message, "APPROVE_LOAN ", 13) == 0;
        int is_reject =
            strncmp(message, "REJECT_LOAN ", 12) == 0;
        int is_lookup =
            strncmp(message, "LOOKUP_ACCOUNT ", 15) == 0;
        int is_list_users =
            strcmp(message, "LIST_USERS") == 0;
        int is_modify_employee_details = strncmp(message, "MODIFY_EMPLOYEE_DETAILS ", 24) == 0;
        int is_view_employee_details = strncmp(message, "VIEW_EMPLOYEE_DETAILS ", 22) == 0;
        int is_add_employee = strncmp(message, "ADD_EMPLOYEE ", 13) == 0;
        int is_assign_loan = strncmp(message, "ASSIGN_LOAN ", 12) == 0;
        int is_assigned_loans = strcmp(message, "ASSIGNED_LOANS") == 0;
        int is_delete_employee = strncmp(message, "DELETE_EMPLOYEE ", 16) == 0;
        int is_reset_password = strncmp(message, "RESET_PASSWORD ", 15) == 0;
        int is_delete_customer = strncmp(message, "DELETE_CUSTOMER ", 16) == 0;
        int is_add_customer = strncmp(message, "ADD_CUSTOMER ", 13) == 0;
        int is_set_user_role =
            strncmp(message, "SET_USER_ROLE ", 14) == 0;
        int is_transfer =
            strncmp(message, "TRANSFER ", 9) == 0;
        int is_transaction_history =
            strcmp(message, "TRANSACTION_HISTORY") == 0;
        int is_passbook =
            strncmp(message, "PASSBOOK ", 9) == 0;
        int is_change_password =
            strncmp(message, "CHANGE_PASSWORD ", 16) == 0;
        int is_feedback =
            strncmp(message, "FEEDBACK ", 9) == 0;
        int is_review_feedback =
            strcmp(message, "REVIEW_FEEDBACK") == 0;
        int is_list_feedback = strncmp(message, "LIST_FEEDBACK", 13) == 0 &&
            (message[13] == '\0' || message[13] == ' ');
        int is_view_feedback = strncmp(message, "VIEW_FEEDBACK ", 14) == 0;
        int is_set_account_status =
            strncmp(message, "SET_ACCOUNT_STATUS ", 19) == 0;
        int is_modify_customer_name =
            strncmp(message, "MODIFY_CUSTOMER_NAME ", 21) == 0;

        /*
         * ADMIN-ONLY:
         * List configured usernames and their roles.
         * The response intentionally contains no passwords or balances.
         */
        if (is_modify_employee_details) {
            if (strcmp(user->role, "ADMIN") != 0) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                char target[32] = {0}, display_name[81] = {0};
                int fields = sscanf(message + 24, "%31s %80[^\n]", target, display_name);
                const struct User *target_user = fields == 2 ? find_user(target) : NULL;
                if (fields != 2 || !profile_name_valid(display_name)) {
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND_OR_NAME");
                } else if (target_user == NULL || strcmp(target_user->role, "EMPLOYEE") != 0) {
                    (void)snprintf(response, sizeof(response), "ERROR EMPLOYEE_NOT_FOUND");
                } else if (employee_details_update(target, display_name) == -1) {
                    perror("ERROR: Cannot update employee details");
                    (void)snprintf(response, sizeof(response), "ERROR EMPLOYEE_DETAILS_UPDATE_FAILED");
                } else {
                    (void)snprintf(response, sizeof(response), "EMPLOYEE_DETAILS_UPDATED %s", target);
                }
            }
        } else if (is_view_employee_details) {
            if (strcmp(user->role, "ADMIN") != 0) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                char target[32] = {0}, display_name[81] = {0}, extra;
                int fields = sscanf(message + 22, "%31s %c", target, &extra);
                const struct User *target_user = fields == 1 ? find_user(target) : NULL;
                if (fields != 1) {
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND");
                } else if (target_user == NULL || strcmp(target_user->role, "EMPLOYEE") != 0) {
                    (void)snprintf(response, sizeof(response), "ERROR EMPLOYEE_NOT_FOUND");
                } else if (employee_details_read(target, display_name, sizeof(display_name)) == -1) {
                    (void)snprintf(response, sizeof(response), "ERROR EMPLOYEE_DETAILS_READ_FAILED");
                } else {
                    (void)snprintf(response, sizeof(response), "EMPLOYEE_DETAILS USER=%s NAME=%s", target, display_name);
                }
            }
        } else if (is_add_employee) {
            if (strcmp(user->role, "ADMIN") != 0) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                char uname[32] = {0}, passwd[64] = {0}, extra;
                int fields = sscanf(message + 13, "%31s %63s %c", uname, passwd, &extra);
                if (fields != 2) {
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND");
                } else if (refresh_users() == -1) {
                    (void)snprintf(response, sizeof(response), "ERROR USER_REGISTRY_READ_FAILED");
                } else if (find_user(uname) != NULL) {
                    (void)snprintf(response, sizeof(response), "ERROR USERNAME_ALREADY_EXISTS");
                } else {
                    off_t new_offset = 0;
                    for (size_t i = 0; i < user_count; i++)
                        if (users[i].lock_offset >= new_offset) new_offset = users[i].lock_offset + 1;
                    if (user_store_add_password(uname, passwd) == -1) {
                        (void)snprintf(response, sizeof(response), "ERROR CREDENTIAL_CREATE_FAILED");
                    } else if (user_registry_add(uname, "000000", "EMPLOYEE", new_offset) == -1) {
                        (void)user_store_remove_password(uname);
                        (void)snprintf(response, sizeof(response), "ERROR USER_REGISTRY_CREATE_FAILED");
                    } else if (refresh_users() == -1) {
                        (void)snprintf(response, sizeof(response), "ERROR USER_REGISTRY_REFRESH_FAILED");
                    } else {
                        (void)snprintf(response, sizeof(response), "EMPLOYEE_CREATED USER=%s", uname);
                    }
                }
            }
        } else if (is_assign_loan) {
            if (strcmp(user->role, "MANAGER") != 0) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                char loan_id[7] = {0}, employee_name[32] = {0}, extra;
                int fields = sscanf(message + 12, "%6s %31s %c", loan_id, employee_name, &extra);
                const struct User *employee = fields == 2 ? find_user(employee_name) : NULL;
                if (fields != 2) {
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND");
                } else if (employee == NULL || strcmp(employee->role, "EMPLOYEE") != 0) {
                    (void)snprintf(response, sizeof(response), "ERROR EMPLOYEE_NOT_FOUND");
                } else {
                    int assigned = assign_pending_loan(loan_id, employee_name);
                    if (assigned == 0)
                        (void)snprintf(response, sizeof(response), "LOAN_ASSIGNED ID=%s EMPLOYEE=%s", loan_id, employee_name);
                    else if (assigned == -2)
                        (void)snprintf(response, sizeof(response), "ERROR INVALID_LOAN_ID");
                    else if (assigned == -3)
                        (void)snprintf(response, sizeof(response), "ERROR LOAN_NOT_FOUND");
                    else if (assigned == -4)
                        (void)snprintf(response, sizeof(response), "ERROR LOAN_NOT_PENDING");
                    else
                        (void)snprintf(response, sizeof(response), "ERROR LOAN_ASSIGNMENT_FAILED");
                }
            }
        } else if (is_assigned_loans) {
            if (strcmp(user->role, "EMPLOYEE") != 0) {
                (void)snprintf(response, sizeof(response), "ERROR EMPLOYEE_ONLY_COMMAND");
            } else if (list_assigned_loans(user->username, response, sizeof(response)) == -1) {
                (void)snprintf(response, sizeof(response), "ERROR ASSIGNED_LOANS_READ_FAILED");
            }
        } else if (is_delete_employee) {
            if (strcmp(user->role, "ADMIN") != 0) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                char target[32] = {0}, extra;
                int fields = sscanf(message + 16, "%31s %c", target, &extra);
                const struct User *victim = fields == 1 ? find_user(target) : NULL;
                if (fields != 1) {
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND");
                } else if (victim == NULL || strcmp(victim->role, "EMPLOYEE") != 0) {
                    (void)snprintf(response, sizeof(response), "ERROR EMPLOYEE_NOT_FOUND");
                } else if (strcmp(target, user->username) == 0) {
                    (void)snprintf(response, sizeof(response), "ERROR CANNOT_DELETE_SELF");
                } else if (user_registry_remove(target) == -1) {
                    (void)snprintf(response, sizeof(response), "ERROR EMPLOYEE_DELETE_FAILED");
                } else {
                    (void)refresh_users();
                    (void)user_store_remove_password(target);
                    (void)snprintf(response, sizeof(response), "EMPLOYEE_DELETED %s", target);
                }
            }
        } else if (is_reset_password) {
            if (strcmp(user->role, "ADMIN") != 0) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                char target[32] = {0}, passwd[64] = {0}, extra;
                int fields = sscanf(message + 15, "%31s %63s %c", target, passwd, &extra);
                const struct User *target_user = fields == 2 ? find_user(target) : NULL;
                if (fields != 2) {
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND");
                } else if (target_user == NULL) {
                    (void)snprintf(response, sizeof(response), "ERROR USER_NOT_FOUND");
                } else if (user_store_reset_password(target, passwd) == -1) {
                    (void)snprintf(response, sizeof(response), "ERROR PASSWORD_RESET_FAILED");
                } else {
                    (void)snprintf(response, sizeof(response), "EMPLOYEE_PASSWORD_RESET %s", target);
                }
            }
        } else if (is_delete_customer) {
            int allowed = strcmp(user->role, "EMPLOYEE") == 0 || strcmp(user->role, "ADMIN") == 0;
            if (!allowed) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                char target_id[7] = {0}, extra;
                int fields = sscanf(message + 16, "%6s %c", target_id, &extra);
                if (fields != 1) {
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND");
                } else if (bank_set_account_status(ACCOUNT_FILE, target_id, 'D', response, sizeof(response)) == -1) {
                    (void)snprintf(response, sizeof(response), "ERROR CUSTOMER_DELETE_FAILED");
                } else if (strncmp(response, "ACCOUNT_STATUS_UPDATED", 22) == 0) {
                    (void)snprintf(response, sizeof(response), "CUSTOMER_DEACTIVATED %s", target_id);
                }
            }
        } else if (is_add_customer) {
            if (strcmp(user->role, "ADMIN") != 0 &&
                strcmp(user->role, "EMPLOYEE") != 0) {
                (void)snprintf(response, sizeof(response),
                               "ERROR PERMISSION_DENIED");
            } else {
                char new_username[25] = {0};
                char new_password[64] = {0};
                char full_name[33] = {0};
                char extra;
                int fields = sscanf(message + 13, "%24s %63s %32[^\n]%c",
                                    new_username, new_password,
                                    full_name, &extra);

                if (fields != 3) {
                    (void)snprintf(response, sizeof(response),
                                   "ERROR INVALID_COMMAND");
                } else if (refresh_users() == -1) {
                    (void)snprintf(response, sizeof(response),
                                   "ERROR USER_REGISTRY_READ_FAILED");
                } else if (find_user(new_username) != NULL) {
                    (void)snprintf(response, sizeof(response),
                                   "ERROR USERNAME_ALREADY_EXISTS");
                } else {
                    off_t next_lock_offset = 0;
                    for (size_t i = 0; i < user_count; i++) {
                        if (users[i].lock_offset >= next_lock_offset)
                            next_lock_offset = users[i].lock_offset + 1;
                    }

                    char new_account_id[7] = {0};

                    if (bank_create_customer(
                            ACCOUNT_FILE, new_username, full_name,
                            new_account_id, response,
                            sizeof(response)) != 0) {
                        /* bank_create_customer prepared the error response. */
                    } else if (user_store_add_password(
                                   new_username, new_password) == -1) {
                        perror("ERROR: Cannot store new customer credentials");
                        (void)snprintf(response, sizeof(response),
                                       "ERROR PARTIAL_CREATION_ACCOUNT_CREATED");
                    } else if (user_registry_add(
                                   new_username, new_account_id, "CUSTOMER",
                                   next_lock_offset) == -1) {
                        perror("ERROR: Cannot register new customer");
                        (void)snprintf(response, sizeof(response),
                                       "ERROR PARTIAL_CREATION_CREDENTIALS_STORED");
                    } else if (refresh_users() == -1) {
                        (void)snprintf(response, sizeof(response),
                                       "ERROR USER_REGISTRY_REFRESH_FAILED");
                    } else {
                        (void)snprintf(response, sizeof(response),
                                       "CUSTOMER_CREATED USER=%s ACCOUNT=%s",
                                       new_username, new_account_id);
                    }
                }
            }
        } else if (is_set_user_role) {
            if (strcmp(user->role, "ADMIN") != 0) {
                (void)snprintf(response, sizeof(response),
                               "ERROR PERMISSION_DENIED");
            } else {
                char target_username[32];
                char target_role[16];
                char extra;
                int fields = sscanf(message + 14, "%31s %15s %c",
                                    target_username, target_role, &extra);

                if (fields != 2) {
                    (void)snprintf(response, sizeof(response),
                                   "ERROR INVALID_COMMAND");
                } else if (user_registry_set_role(
                               target_username, target_role) == -1) {
                    if (errno == ENOENT)
                        (void)snprintf(response, sizeof(response),
                                       "ERROR USER_NOT_FOUND");
                    else if (errno == EINVAL)
                        (void)snprintf(response, sizeof(response),
                                       "ERROR INVALID_ROLE_OR_USERNAME");
                    else {
                        perror("ERROR: Cannot update user role");
                        (void)snprintf(response, sizeof(response),
                                       "ERROR ROLE_UPDATE_FAILED");
                    }
                } else {
                    (void)snprintf(response, sizeof(response),
                                   "USER_ROLE_UPDATED %s %s",
                                   target_username, target_role);
                }
            }
        }
        else if (is_list_users) {
            if (strcmp(user->role, "ADMIN") != 0) {
                if (send_error(client_fd,
                               "ERROR PERMISSION_DENIED") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }

            if (build_user_directory(response, sizeof(response)) == -1) {
                if (send_error(client_fd, "ERROR SERVER_ERROR") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }
        }

        /* Any authenticated role may change its own password. */
        else if (is_change_password) {
            if (user_store_change_password(user->username, message,
                                           response, sizeof(response)) == -1) {
                fprintf(stderr, "ERROR: Password-change processing failed\\n");
                (void)snprintf(response, sizeof(response),
                               "ERROR PASSWORD_CHANGE_FAILED");
            }
        }

        /* Only customers may submit feedback. */
        else if (is_feedback) {
            if (strcmp(user->role, "CUSTOMER") != 0) {
                (void)snprintf(response, sizeof(response),
                               "ERROR CUSTOMER_ONLY_COMMAND");
            } else if (user_store_append_feedback(
                           user->username, user->account_id, message + 9,
                           transaction_semid, response,
                           sizeof(response)) == -1) {
                fprintf(stderr, "ERROR: Feedback processing failed\\n");
                (void)snprintf(response, sizeof(response),
                               "ERROR FEEDBACK_FAILED");
            }
        }

        /* MANAGER-ONLY: list feedback summaries, eight per page. */
        else if (is_list_feedback) {
            if (strcmp(user->role, "MANAGER") != 0) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                unsigned long start_id = 1; char extra;
                int fields = 1;
                if (message[13] == ' ') fields = sscanf(message + 14, "%lu %c", &start_id, &extra);
                if (fields != 1 || start_id == 0)
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND");
                else if (user_store_list_feedback(transaction_semid, start_id, response, sizeof(response)) == -1)
                    (void)snprintf(response, sizeof(response), "ERROR FEEDBACK_LIST_FAILED");
            }
        }

        /* MANAGER-ONLY: view one entry listed by LIST_FEEDBACK. */
        else if (is_view_feedback) {
            if (strcmp(user->role, "MANAGER") != 0) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                unsigned long feedback_id = 0; char extra;
                if (sscanf(message + 14, "%lu %c", &feedback_id, &extra) != 1 || feedback_id == 0)
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND");
                else if (user_store_view_feedback(transaction_semid, feedback_id, response, sizeof(response)) == -1)
                    (void)snprintf(response, sizeof(response), "ERROR FEEDBACK_VIEW_FAILED");
            }
        }

        /* MANAGER-ONLY: review the most recent customer feedback. */
        else if (is_review_feedback) {
            if (strcmp(user->role, "MANAGER") != 0) {
                (void)snprintf(response, sizeof(response),
                               "ERROR PERMISSION_DENIED");
            } else if (user_store_read_feedback(transaction_semid,
                                                response,
                                                sizeof(response)) == -1) {
                (void)snprintf(response, sizeof(response),
                               "ERROR FEEDBACK_READ_FAILED");
            }
        }

        /* MANAGER-ONLY: activate (A) or deactivate (D) an account. */
        else if (is_set_account_status) {
            if (strcmp(user->role, "MANAGER") != 0) {
                (void)snprintf(response, sizeof(response),
                               "ERROR PERMISSION_DENIED");
            } else {
                char target_id[7];
                char status;
                char extra[2];

                int fields = sscanf(message + 19, "%6s %c %1s",
                                    target_id, &status, extra);
                if (fields != 2) {
                    (void)snprintf(response, sizeof(response),
                                   "ERROR INVALID_COMMAND");
                } else if (bank_set_account_status(
                               ACCOUNT_FILE, target_id, status,
                               response, sizeof(response)) == -1) {
                    (void)snprintf(response, sizeof(response),
                                   "ERROR ACCOUNT_STATUS_UPDATE_FAILED");
                }
            }
        }

        /* EMPLOYEE/ADMIN: modify a customer's display name. */
        else if (is_modify_customer_name) {
            int allowed = strcmp(user->role, "EMPLOYEE") == 0 ||
                          strcmp(user->role, "ADMIN") == 0;
            if (!allowed) {
                (void)snprintf(response, sizeof(response),
                               "ERROR PERMISSION_DENIED");
            } else {
                const char *args = message + 21;
                size_t args_length = strlen(args);

                if (args_length < 8 || args[6] != ' ') {
                    (void)snprintf(response, sizeof(response),
                                   "ERROR INVALID_COMMAND");
                } else {
                    char target_id[7];
                    memcpy(target_id, args, 6);
                    target_id[6] = '\0';

                    if (bank_modify_customer_name(
                            ACCOUNT_FILE, target_id, args + 7,
                            response, sizeof(response)) == -1) {
                        (void)snprintf(response, sizeof(response),
                                       "ERROR CUSTOMER_UPDATE_FAILED");
                    }
                }
            }
        }

        /*
         * EMPLOYEE / MANAGER / ADMIN:
         * Look up one explicitly requested account.
         * Customers must use their own BALANCE command instead.
         */
        else if (is_lookup) {
            int lookup_allowed =
                strcmp(user->role, "EMPLOYEE") == 0 ||
                strcmp(user->role, "MANAGER") == 0 ||
                strcmp(user->role, "ADMIN") == 0;

            if (!lookup_allowed) {
                if (send_error(client_fd,
                               "ERROR PERMISSION_DENIED") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }

            /* Prefix includes the space after LOOKUP_ACCOUNT. */
            const char *target_account_id = message + 15;

            if (bank_lookup_account(ACCOUNT_FILE,
                                    target_account_id,
                                    response,
                                    sizeof(response)) == -1) {
                fprintf(stderr,
                        "ERROR: Account lookup processing failed\n");
                if (send_error(client_fd, "ERROR SERVER_ERROR") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }
        }

        /*
         * CUSTOMER-ONLY: Transfer from the authenticated customer's account.
         */
        else if (is_transfer) {
            if (strcmp(user->role, "CUSTOMER") != 0) {
                if (send_error(client_fd, "ERROR CUSTOMER_ONLY_COMMAND") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }

            if (bank_transfer_command(ACCOUNT_FILE,
                                      user->account_id,
                                      message,
                                      response,
                                      sizeof(response),
                                      transaction_semid) == -1) {
                fprintf(stderr, "ERROR: Transfer processing failed\\n");
                if (send_error(client_fd, "ERROR SERVER_ERROR") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }
        }

        /*
         * Customers can see their own transaction history.
         * Employees/managers/admins use PASSBOOK account_id for an
         * explicitly selected account.
         */
        else if (is_transaction_history || is_passbook) {
            const char *target_account_id = NULL;

            if (is_transaction_history) {
                if (strcmp(user->role, "CUSTOMER") != 0) {
                    if (send_error(client_fd,
                                   "ERROR CUSTOMER_ONLY_COMMAND") == -1) {
                        pthread_mutex_unlock(&operation_mutex);
                        break;
                    }
                    pthread_mutex_unlock(&operation_mutex);
                    continue;
                }
                target_account_id = user->account_id;
            } else {
                int passbook_allowed =
                    strcmp(user->role, "EMPLOYEE") == 0 ||
                    strcmp(user->role, "MANAGER") == 0 ||
                    strcmp(user->role, "ADMIN") == 0;

                if (!passbook_allowed) {
                    if (send_error(client_fd, "ERROR PERMISSION_DENIED") == -1) {
                        pthread_mutex_unlock(&operation_mutex);
                        break;
                    }
                    pthread_mutex_unlock(&operation_mutex);
                    continue;
                }
                target_account_id = message + 9;
            }

            if (bank_transaction_history(TRANSACTION_LOG,
                                         target_account_id,
                                         response,
                                         sizeof(response),
                                         transaction_semid) == -1) {
                fprintf(stderr, "ERROR: Transaction history processing failed\\n");
                if (send_error(client_fd, "ERROR SERVER_ERROR") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }
        }

        /*
         * MANAGER-ONLY:
         * Reject unauthorized roles before invoking the loan module.
         */
        else if (is_approve || is_reject) {
            int is_manager = strcmp(user->role, "MANAGER") == 0;
            int is_employee = strcmp(user->role, "EMPLOYEE") == 0;
            if (!is_manager && !is_employee) {
                (void)snprintf(response, sizeof(response), "ERROR PERMISSION_DENIED");
            } else {
                char loan_id[7] = {0};
                if (sscanf(message + (is_approve ? 13 : 12), "%6s", loan_id) != 1) {
                    (void)snprintf(response, sizeof(response), "ERROR INVALID_COMMAND");
                } else if (is_employee && loan_assignment_matches(loan_id, user->username) != 1) {
                    (void)snprintf(response, sizeof(response), "ERROR LOAN_NOT_ASSIGNED_TO_YOU");
                } else if (loan_process_command(LOAN_FILE, user->account_id, message,
                                               response, sizeof(response)) == -1) {
                    perror("ERROR: Loan decision processing failed");
                    (void)snprintf(response, sizeof(response), "ERROR LOAN_PROCESSING_FAILED");
                }
            }
        }

        else if (is_apply || is_status) {
            if (strcmp(user->role, "CUSTOMER") != 0) {
                if (send_error(client_fd,
                               "ERROR CUSTOMER_ONLY_COMMAND") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }

            if (loan_process_command(LOAN_FILE,
                                     user->account_id,
                                     message,
                                     response,
                                     sizeof(response)) == -1) {
                fprintf(stderr, "ERROR: Customer loan operation failed\n");
                if (send_error(client_fd, "ERROR SERVER_ERROR") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }
        }

        /*
         * EXISTING CUSTOMER BANKING:
         * BALANCE, DEPOSIT and WITHDRAW.
         * Non-customer roles do not use this customer banking interface.
         */
        else {
            if (strcmp(user->role, "CUSTOMER") != 0) {
                if (send_error(client_fd,
                               "ERROR CUSTOMER_ONLY_COMMAND") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }

            if (bank_process_command(ACCOUNT_FILE,
                                     user->account_id,
                                     message,
                                     response,
                                     sizeof(response),
                                     transaction_semid) == -1) {
                fprintf(stderr, "ERROR: Banking command processing failed\n");
                if (send_error(client_fd, "ERROR SERVER_ERROR") == -1) {
                    pthread_mutex_unlock(&operation_mutex);
                    break;
                }
                pthread_mutex_unlock(&operation_mutex);
                continue;
            }
        }

        pthread_mutex_unlock(&operation_mutex);

        if (send_frame(client_fd, response) == -1) {
            perror("ERROR: Cannot send command response");
            break;
        }
    }

cleanup:
    if (session_lock_fd >= 0 && user != NULL) {
        if (release_session_lock(session_lock_fd, user) == -1)
            perror("ERROR: Cannot release session lock");
    }
}

/* One detached worker owns and closes exactly one client socket. */
static void *client_thread_main(void *argument)
{
    struct client_thread_arg *client = argument;
    int client_fd = client->client_fd;
    free(client);

    handle_client(client_fd);
    close(client_fd);

    pthread_mutex_lock(&clients_mutex);
    if (active_clients > 0)
        active_clients--;
    if (active_clients == 0)
        pthread_cond_broadcast(&clients_cond);
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

/* Create the shared transaction-log semaphore with initial value 1. */
static int create_transaction_semaphore(void)
{
    int semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);

    if (semid == -1)
        return -1;

    union semun argument;
    argument.val = 1;

    if (semctl(semid, 0, SETVAL, argument) == -1) {
        int saved_errno = errno;
        semctl(semid, 0, IPC_RMID);
        errno = saved_errno;
        return -1;
    }

    return semid;
}

int main(void)
{
    if (install_signal_handlers() == -1) {
        perror("ERROR: Cannot install signal handlers");
        return EXIT_FAILURE;
    }

    /* Seed the five demo credentials only when the credential file is new. */
    const char *default_usernames[DEFAULT_USER_COUNT];
    const char *default_passwords[DEFAULT_USER_COUNT];

    for (size_t i = 0; i < DEFAULT_USER_COUNT; i++) {
        default_usernames[i] = default_users[i].username;
        default_passwords[i] = default_users[i].password;
    }

    if (user_store_init(default_usernames, default_passwords,
                        DEFAULT_USER_COUNT) == -1) {
        perror("ERROR: Cannot initialize credential storage");
        return EXIT_FAILURE;
    }

    if (user_registry_init() == -1 || refresh_users() == -1) {
        perror("ERROR: Cannot initialize/load user registry");
        return EXIT_FAILURE;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("ERROR: Cannot create server socket");
        return EXIT_FAILURE;
    }

    int reuse_address = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1) {
        perror("ERROR: Cannot configure socket reuse");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(SERVER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server_fd, (struct sockaddr *)&address,
             sizeof(address)) == -1) {
        perror("ERROR: Cannot bind server socket");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 16) == -1) {
        perror("ERROR: Cannot listen on server socket");
        close(server_fd);
        return EXIT_FAILURE;
    }

    transaction_semid = create_transaction_semaphore();

    if (transaction_semid == -1) {
        perror("ERROR: Cannot create transaction-log semaphore");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Banking session server listening on 127.0.0.1:%d\n",
           SERVER_PORT);
    printf("Account data file: %s\n", ACCOUNT_FILE);
    printf("Loan file: %s\n", LOAN_FILE);
    printf("Transaction log: %s\n", TRANSACTION_LOG);
    printf("Transaction semaphore ID: %d\n", transaction_semid);
    fflush(stdout);

    int exit_status = EXIT_SUCCESS;

    while (!stop_server) {
        struct sockaddr_in client_address;
        socklen_t client_length = sizeof(client_address);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_address,
                               &client_length);

        if (client_fd == -1) {
            if (errno == EINTR) {
                if (stop_server)
                    break;
                continue;
            }

            perror("ERROR: Cannot accept client connection");
            exit_status = EXIT_FAILURE;
            break;
        }

        struct client_thread_arg *client = malloc(sizeof(*client));
        if (client == NULL) {
            perror("ERROR: Cannot allocate client thread argument");
            close(client_fd);
            continue;
        }
        client->client_fd = client_fd;

        pthread_mutex_lock(&clients_mutex);
        active_clients++;
        pthread_mutex_unlock(&clients_mutex);

        pthread_t thread;
        int thread_error = pthread_create(&thread, NULL,
                                          client_thread_main, client);
        if (thread_error != 0) {
            errno = thread_error;
            perror("ERROR: Cannot create client thread");
            free(client);
            close(client_fd);
            pthread_mutex_lock(&clients_mutex);
            active_clients--;
            if (active_clients == 0)
                pthread_cond_broadcast(&clients_cond);
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }

        thread_error = pthread_detach(thread);
        if (thread_error != 0) {
            errno = thread_error;
            perror("ERROR: Cannot detach client thread");
            (void)pthread_join(thread, NULL);
        }
    }

    close(server_fd);

    /* Keep shared IPC alive until every client worker has finished. */
    pthread_mutex_lock(&clients_mutex);
    while (active_clients != 0)
        pthread_cond_wait(&clients_cond, &clients_mutex);
    pthread_mutex_unlock(&clients_mutex);

    /*
     * Remove the shared semaphore when the parent server stops.
     * As in this educational server, stop active clients before shutting
     * down the parent so no client operation is still using the semaphore.
     */
    if (transaction_semid >= 0) {
        if (semctl(transaction_semid, 0, IPC_RMID) == -1 &&
            errno != EIDRM && errno != EINVAL) {
            perror("ERROR: Cannot remove transaction semaphore");
            exit_status = EXIT_FAILURE;
        }

        transaction_semid = -1;
    }

    printf("Banking session server stopped.\n");
    return exit_status;
}
