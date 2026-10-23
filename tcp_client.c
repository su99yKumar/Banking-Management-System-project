#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Keep sending until every byte has been accepted by send(). */
static int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t count = send(fd, data + sent, length - sent, 0);

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (count == 0) {
            errno = EPIPE;
            return -1;
        }

        sent += (size_t)count;
    }

    return 0;
}

int main(void)
{
    /* Create a client-side IPv4 TCP socket. */
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (client_fd == -1) {
        perror("ERROR: Cannot create client socket");
        return EXIT_FAILURE;
    }

    /* Describe the server we want to connect to. */
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(5050);

    if (inet_pton(AF_INET, "127.0.0.1",
                  &server_address.sin_addr) != 1) {
        fprintf(stderr, "ERROR: Invalid server IPv4 address.\n");
        close(client_fd);
        return EXIT_FAILURE;
    }

    /* Establish the TCP connection to the server. */
    if (connect(client_fd, (struct sockaddr *)&server_address,
                sizeof(server_address)) == -1) {
        perror("ERROR: Cannot connect to server; start tcp_server first");
        close(client_fd);
        return EXIT_FAILURE;
    }

    const char message[] = "Hello from the client!";
    size_t message_length = sizeof(message) - 1;

    /* Send the message without its terminating C string null byte. */
    if (send_all(client_fd, message, message_length) == -1) {
        perror("ERROR: Failed to send message");
        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Client sent %zu bytes.\n", message_length);

    /*
     * Tell the server that no more request bytes will be sent.
     * The client can still receive data on this same connection.
     */
    if (shutdown(client_fd, SHUT_WR) == -1) {
        perror("ERROR: Cannot finish sending request");
        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Server echoed: ");

    char buffer[256];

    /* Receive until the server closes its sending side. */
    for (;;) {
        ssize_t count = recv(client_fd, buffer, sizeof(buffer), 0);

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("ERROR: Failed while receiving server reply");
            close(client_fd);
            return EXIT_FAILURE;
        }

        if (count == 0) {
            break;
        }

        /* Write exactly the bytes received; no null terminator needed. */
        if (fwrite(buffer, 1, (size_t)count, stdout) != (size_t)count) {
            fprintf(stderr, "\nERROR: Cannot display server reply.\n");
            close(client_fd);
            return EXIT_FAILURE;
        }
    }

    printf("\n");

    close(client_fd);
    return EXIT_SUCCESS;
}
