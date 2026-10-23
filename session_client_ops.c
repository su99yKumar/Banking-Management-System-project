
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 5053
#define MAX_FRAME_SIZE 1024

/* Read one line from the terminal and remove its trailing newline. */
static int read_line(const char *prompt, char *buffer, size_t capacity)
{
    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buffer, (int)capacity, stdin) == NULL)
        return -1;

    size_t length = strlen(buffer);

    if (length > 0 && buffer[length - 1] == '\n') {
        buffer[length - 1] = '\0';
    } else if (!feof(stdin)) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {
            /* Discard the remainder of an overlong input line. */
        }

        fprintf(stderr, "ERROR: Input is too long\n");
        return -1;
    }

    return 0;
}

/* Send all requested bytes, handling partial sends and EINTR. */
static int send_all(int fd, const void *buffer, size_t length)
{
    const char *p = buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, p + sent, length - sent, 0);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

/* Receive exactly length bytes; distinguish EOF from an incomplete read. */
static int recv_exact(int fd, void *buffer, size_t length)
{
    char *p = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t n = recv(fd, p + received, length - received, 0);

        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            return received == 0 ? 0 : -1;
        if (n < 0)
            return -1;

        received += (size_t)n;
    }

    return 1;
}

/* Send a 4-byte network-order length followed by the message. */
static int send_frame(int fd, const char *message)
{
    size_t length = strlen(message);

    if (length == 0 || length > MAX_FRAME_SIZE) {
        fprintf(stderr, "ERROR: Invalid outgoing message length\n");
        return -1;
    }

    uint32_t network_length = htonl((uint32_t)length);

    if (send_all(fd, &network_length, sizeof(network_length)) == -1)
        return -1;

    return send_all(fd, message, length);
}

/* Receive and validate a length-prefixed message. */
static int recv_frame(int fd, char *buffer, size_t capacity)
{
    uint32_t network_length;
    int result = recv_exact(fd, &network_length, sizeof(network_length));

    if (result != 1)
        return result;

    uint32_t length = ntohl(network_length);

    if (length == 0 || length > MAX_FRAME_SIZE ||
        (size_t)length >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    result = recv_exact(fd, buffer, length);

    if (result != 1) {
        errno = ECONNRESET;
        return -1;
    }

    buffer[length] = '\0';
    return 1;
}

int main(void)
{
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (client_fd == -1) {
        perror("ERROR: Cannot create client socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, "127.0.0.1",
                  &address.sin_addr) != 1) {
        fprintf(stderr, "ERROR: Invalid server address\n");
        close(client_fd);
        return EXIT_FAILURE;
    }

    if (connect(client_fd, (struct sockaddr *)&address,
                sizeof(address)) == -1) {
        perror("ERROR: Cannot connect; start bank_session_server first");
        close(client_fd);
        return EXIT_FAILURE;
    }

    char username[32];
    char password[64];
    char request[MAX_FRAME_SIZE + 1];
    char response[MAX_FRAME_SIZE + 1];

    if (read_line("Username: ", username, sizeof(username)) == -1 ||
        read_line("Password: ", password, sizeof(password)) == -1) {
        close(client_fd);
        return EXIT_FAILURE;
    }

    int count = snprintf(request, sizeof(request),
                         "LOGIN %s %s", username, password);

    if (count < 0 || (size_t)count >= sizeof(request)) {
        fprintf(stderr, "ERROR: Login request is too long\n");
        close(client_fd);
        return EXIT_FAILURE;
    }

    if (send_frame(client_fd, request) == -1) {
        perror("ERROR: Cannot send login request");
        close(client_fd);
        return EXIT_FAILURE;
    }

    int result = recv_frame(client_fd, response, sizeof(response));

    if (result != 1) {
        if (result == -1)
            perror("ERROR: Cannot receive login response");
        else
            fprintf(stderr, "ERROR: Server closed the connection\n");

        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Server response: %s\n", response);

    /*
     * Accept LOGIN_OK and role-bearing responses such as
     * LOGIN_OK ROLE=CUSTOMER and LOGIN_OK ROLE=MANAGER.
     */
    if (strncmp(response, "LOGIN_OK", 8) != 0 ||
        (response[8] != '\0' && response[8] != ' ')) {
        close(client_fd);
        return EXIT_SUCCESS;
    }

    const char *role = strstr(response, "ROLE=");
    if (role != NULL)
        printf("Authenticated role: %s\n", role + 5);

    printf("Logged in. Available commands:\n");
    printf("  BALANCE\n");
    printf("  DEPOSIT 25.00\n");
    printf("  WITHDRAW 10.00\n");
    printf("  APPLY_LOAN 5000.00 24\n");
    printf("  LOAN_STATUS\n");
    printf("  APPROVE_LOAN loan_id amount (manager or assigned employee)\n");
    printf("  REJECT_LOAN loan_id (manager or assigned employee)\n  ADD_CUSTOMER username password full_name (employee/admin)\n  ADD_EMPLOYEE username password (admin)\n  ASSIGN_LOAN loan_id employee (manager)\n  ASSIGNED_LOANS (employee)\n  DELETE_CUSTOMER account_id (employee/admin)\n  DELETE_EMPLOYEE username (admin)\n  RESET_PASSWORD username new_password (admin)\n  MODIFY_EMPLOYEE_DETAILS username full_name (admin)\n  VIEW_EMPLOYEE_DETAILS username (admin)\n  EXIT\n");
    printf("  LOGOUT\n");

    for (;;) {
        if (read_line("bank> ", request, sizeof(request)) == -1) {
            fprintf(stderr, "ERROR: Cannot read command\n");
            break;
        }

        if (request[0] == '\0') {
            fprintf(stderr, "ERROR: Enter a command\n");
            continue;
        }

        if (send_frame(client_fd, request) == -1) {
            perror("ERROR: Cannot send command");
            break;
        }

        result = recv_frame(client_fd, response, sizeof(response));

        if (result != 1) {
            if (result == -1)
                perror("ERROR: Cannot receive command response");
            else
                fprintf(stderr, "ERROR: Server closed the connection\n");

            break;
        }

        printf("Server response: %s\n", response);

        if (strcmp(request, "LOGOUT") == 0 || strcmp(request, "EXIT") == 0)
            break;
    }

    if (close(client_fd) == -1) {
        perror("ERROR: Cannot close client socket");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
