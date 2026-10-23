#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    SERVER_PORT = 5051,
    MAX_FRAME_SIZE = 1024,
    LISTEN_BACKLOG = 5
};

/*
 * Send exactly 'length' bytes unless a non-recoverable error occurs.
 * TCP may accept fewer bytes than requested, so keep sending the
 * unsent portion until the entire buffer has been transmitted.
 */
static int send_all(int fd, const void *buffer, size_t length)
{
    const char *data = buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t count = send(fd, data + sent, length - sent, 0);

        if (count < 0) {
            if (errno == EINTR) {
                continue;  /* A signal interrupted send(); try again. */
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

/*
 * Receive exactly 'length' bytes.
 * Returns 1 on success, 0 if the peer closes before any bytes arrive,
 * or -1 on an error or an incomplete buffer.
 */
static int recv_exact(int fd, void *buffer, size_t length)
{
    char *data = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t count = recv(fd, data + received,
                             length - received, 0);

        if (count < 0) {
            if (errno == EINTR) {
                continue;  /* Retry after an interrupted recv(). */
            }

            return -1;
        }

        if (count == 0) {
            if (received == 0) {
                return 0;  /* Clean EOF before this part began. */
            }

            errno = ECONNRESET;  /* A frame ended prematurely. */
            return -1;
        }

        received += (size_t)count;
    }

    return 1;
}

int main(void)
{
    /* Create an IPv4 TCP listening socket. */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("ERROR: Cannot create server socket");
        return EXIT_FAILURE;
    }

    /* Allow quick reuse of this local port after a restart. */
    int reuse_address = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1) {
        perror("ERROR: Cannot configure server socket");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* Configure the server endpoint: 127.0.0.1:5051. */
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERVER_PORT);
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server_fd, (struct sockaddr *)&server_address,
             sizeof(server_address)) == -1) {
        perror("ERROR: Cannot bind to 127.0.0.1:5051");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, LISTEN_BACKLOG) == -1) {
        perror("ERROR: Cannot listen for client connections");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Framed server listening on 127.0.0.1:%d\n",
           SERVER_PORT);
    fflush(stdout);

    /* This learning demo accepts one client connection. */
    int client_fd = accept(server_fd, NULL, NULL);

    if (client_fd == -1) {
        perror("ERROR: Cannot accept client connection");
        close(server_fd);
        return EXIT_FAILURE;
    }

    close(server_fd);

    /*
     * Read the 4-byte header first. It tells us how many payload
     * bytes belong to this message.
     */
    uint32_t network_length;
    int result = recv_exact(client_fd, &network_length,
                            sizeof(network_length));

    if (result == 0) {
        printf("Client closed without sending a frame.\n");
        close(client_fd);
        return EXIT_SUCCESS;
    }

    if (result == -1) {
        perror("ERROR: Failed to receive complete length header");
        close(client_fd);
        return EXIT_FAILURE;
    }

    /* Convert the received network-order length to a host integer. */
    uint32_t message_length = ntohl(network_length);

    /* Validate the claimed size before reading into our buffer. */
    if (message_length > MAX_FRAME_SIZE) {
        fprintf(stderr,
                "ERROR: Frame length %u exceeds the %d-byte limit.\n",
                (unsigned int)message_length, MAX_FRAME_SIZE);
        close(client_fd);
        return EXIT_FAILURE;
    }

    char payload[MAX_FRAME_SIZE];

    /* Read exactly the number of payload bytes in the header. */
    if (recv_exact(client_fd, payload, message_length) != 1) {
        perror("ERROR: Failed to receive complete message payload");
        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Server received a framed message (%u bytes): %.*s\n",
           (unsigned int)message_length, (int)message_length, payload);

    /*
     * Echo the same frame: send the 4-byte length header and then
     * exactly the payload bytes. No C string terminator is required.
     */
    if (send_all(client_fd, &network_length,
                 sizeof(network_length)) == -1) {
        perror("ERROR: Failed to send response length header");
        close(client_fd);
        return EXIT_FAILURE;
    }

    if (send_all(client_fd, payload, message_length) == -1) {
        perror("ERROR: Failed to send response payload");
        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Server echoed the complete frame.\n");

    close(client_fd);
    return EXIT_SUCCESS;
}
