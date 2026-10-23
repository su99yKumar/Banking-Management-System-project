#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    SERVER_PORT = 5050,
    BUFFER_SIZE = 1024,
    LISTEN_BACKLOG = 5
};

/* Keep sending until every byte has been accepted by send(). */
static int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t count = send(fd, data + sent, length - sent, 0);

        if (count < 0) {
            if (errno == EINTR) {
                continue;  /* Retry if a signal interrupted send(). */
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
    /* Create an IPv4 TCP socket. */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("ERROR: Cannot create server socket");
        return EXIT_FAILURE;
    }

    /* Allow the port to be reused after a recent server shutdown. */
    int reuse_address = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1) {
        perror("ERROR: Cannot configure server socket");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* Prepare the server's local address. */
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERVER_PORT);
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* Associate the socket with 127.0.0.1:5050. */
    if (bind(server_fd, (struct sockaddr *)&server_address,
             sizeof(server_address)) == -1) {
        perror("ERROR: Cannot bind server to 127.0.0.1:5050");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* Mark this socket as ready to accept client connections. */
    if (listen(server_fd, LISTEN_BACKLOG) == -1) {
        perror("ERROR: Cannot listen for clients");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on 127.0.0.1:%d\n", SERVER_PORT);
    fflush(stdout);

    /* Wait for one client to connect. */
    int client_fd = accept(server_fd, NULL, NULL);

    if (client_fd == -1) {
        perror("ERROR: Cannot accept client connection");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server accepted a client connection.\n");

    /* This demo handles one client, so the listening socket is no
       longer needed after that connection has been accepted. */
    close(server_fd);

    char buffer[BUFFER_SIZE];
    size_t total = 0;

    /*
     * Read until the client says it has finished sending.
     * recv() returning 0 means the peer has closed its write side.
     */
    while (total < sizeof(buffer)) {
        ssize_t count = recv(client_fd, buffer + total,
                             sizeof(buffer) - total, 0);

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("ERROR: Failed while receiving client data");
            close(client_fd);
            return EXIT_FAILURE;
        }

        if (count == 0) {
            break;
        }

        total += (size_t)count;
    }

    /* Refuse to echo a request that filled the whole buffer. */
    if (total == sizeof(buffer)) {
        fprintf(stderr,
                "ERROR: Request reached the %d-byte demo limit.\n",
                BUFFER_SIZE);
        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Server received %zu bytes: %.*s\n",
           total, (int)total, buffer);

    /* Echo exactly the number of bytes that were received. */
    if (send_all(client_fd, buffer, total) == -1) {
        perror("ERROR: Failed to send echo to client");
        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Server echoed %zu bytes.\n", total);

    close(client_fd);
    return EXIT_SUCCESS;
}
