#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define PORT 5053
#define MAX_FRAME_SIZE 1024

/* Prompt for one line of input and remove its newline. */
static int read_line(const char *prompt, char *buffer, size_t capacity)
{
    fputs(prompt, stdout);
    fflush(stdout);

    if (fgets(buffer, capacity, stdin) == NULL)
        return -1;

    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 0;
}

/* Receive exactly the requested number of bytes. */
static int recv_exact(int fd, void *buffer, size_t len)
{
    size_t total = 0;
    char *p = buffer;

    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, 0);

        if (n == 0) {
            if (total == 0)
                return 0;
            errno = ECONNRESET;
            return -1;
        }

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        total += (size_t)n;
    }

    return 1;
}

/* Send the complete buffer, handling partial sends. */
static int send_all(int fd, const void *buffer, size_t len)
{
    size_t total = 0;
    const char *p = buffer;

    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0) {
            errno = EPIPE;
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}

/* Send a text message with a four-byte network-order length prefix. */
static int send_frame(int fd, const char *message)
{
    size_t length = strlen(message);

    if (length > MAX_FRAME_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    uint32_t network_length = htonl((uint32_t)length);

    if (send_all(fd, &network_length, sizeof(network_length)) == -1)
        return -1;

    return send_all(fd, message, length);
}

/* Receive one complete length-prefixed text message. */
static int recv_frame(int fd, char *buffer, size_t capacity)
{
    uint32_t network_length;
    int result = recv_exact(fd, &network_length, sizeof(network_length));

    if (result != 1)
        return result;

    uint32_t length = ntohl(network_length);

    if (length > MAX_FRAME_SIZE || (size_t)length >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    if (length > 0) {
        result = recv_exact(fd, buffer, length);

        if (result != 1) {
            if (result == 0)
                errno = ECONNRESET;
            return -1;
        }
    }

    buffer[length] = '\0';
    return 1;
}

int main(void)
{
    /* A failed send to a closed socket should return an error, not kill us. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("ERROR: Cannot configure SIGPIPE");
        return EXIT_FAILURE;
    }

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (client_fd == -1) {
        perror("ERROR: Cannot create client socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        fprintf(stderr, "ERROR: Invalid server address\n");
        close(client_fd);
        return EXIT_FAILURE;
    }

    if (connect(client_fd, (struct sockaddr *)&address,
                sizeof(address)) == -1) {
        perror("ERROR: Cannot connect; start session_server first");
        close(client_fd);
        return EXIT_FAILURE;
    }

    char username[32];
    char password[64];
    char request[MAX_FRAME_SIZE + 1];
    char response[MAX_FRAME_SIZE + 1];

    if (read_line("Username: ", username, sizeof(username)) == -1 ||
        read_line("Password: ", password, sizeof(password)) == -1) {
        fprintf(stderr, "ERROR: Cannot read credentials\n");
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

    /* A rejected login should end here, not enter an active session. */
    if (strcmp(response, "LOGIN_OK") != 0) {
        close(client_fd);
        return EXIT_SUCCESS;
    }

    printf("Session is active. Keep this terminal open.\n");

    char ignored[8];

    if (read_line("Press ENTER to log out: ",
                  ignored, sizeof(ignored)) == -1) {
        fprintf(stderr, "ERROR: Cannot read logout input\n");
        close(client_fd);
        return EXIT_FAILURE;
    }

    if (send_frame(client_fd, "LOGOUT") == -1) {
        perror("ERROR: Cannot send logout request");
        close(client_fd);
        return EXIT_FAILURE;
    }

    result = recv_frame(client_fd, response, sizeof(response));

    if (result == 1)
        printf("Server response: %s\n", response);
    else if (result == -1)
        perror("ERROR: Cannot receive logout response");

    close(client_fd);
    return result == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
}
