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
    SERVER_PORT = 5052,
    MAX_FRAME_SIZE = 1024
};

/*
 * Send all requested bytes.
 * A successful send() may transmit fewer bytes than requested,
 * so continue until the entire buffer has been sent.
 */
static int send_all(int fd, const void *buffer, size_t length)
{
    const char *data = buffer;
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

/*
 * Receive exactly the requested number of bytes.
 * Return 1 on success, 0 if the peer closes before any bytes arrive,
 * or -1 if an error or premature end occurs.
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
                continue;
            }

            return -1;
        }

        if (count == 0) {
            if (received == 0) {
                return 0;
            }

            errno = ECONNRESET;
            return -1;
        }

        received += (size_t)count;
    }

    return 1;
}

int main(void)
{
    /* Create an IPv4 TCP socket for the client. */
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (client_fd == -1) {
        perror("ERROR: Cannot create client socket");
        return EXIT_FAILURE;
    }

    /* Describe the server endpoint we want to connect to. */
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, "127.0.0.1",
                  &server_address.sin_addr) != 1) {
        fprintf(stderr, "ERROR: Invalid server IPv4 address.\n");
        close(client_fd);
        return EXIT_FAILURE;
    }

    /* Connect to the framed server. */
    if (connect(client_fd, (struct sockaddr *)&server_address,
                sizeof(server_address)) == -1) {
        perror("ERROR: Cannot connect; start tcp_frame_server first");
        close(client_fd);
        return EXIT_FAILURE;
    }

    /* This sample request has a 7-byte payload. */
    const char message[] = "BALANCE";
    size_t message_length = strlen(message);

    if (message_length > MAX_FRAME_SIZE) {
        fprintf(stderr, "ERROR: Request exceeds the frame size limit.\n");
        close(client_fd);
        return EXIT_FAILURE;
    }

    /*
     * The protocol sends a 4-byte unsigned length before the payload.
     * htonl() converts the length to network byte order.
     */
    uint32_t network_length = htonl((uint32_t)message_length);

    if (send_all(client_fd, &network_length,
                 sizeof(network_length)) == -1) {
        perror("ERROR: Failed to send request length header");
        close(client_fd);
        return EXIT_FAILURE;
    }

    /* Now send exactly the payload bytes, without a null terminator. */
    if (send_all(client_fd, message, message_length) == -1) {
        perror("ERROR: Failed to send request payload");
        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Client sent framed request: %s (%zu bytes)\n",
           message, message_length);

    /* Read the response's 4-byte length header first. */
    uint32_t response_network_length;
    int result = recv_exact(client_fd, &response_network_length,
                            sizeof(response_network_length));

    if (result == 0) {
        fprintf(stderr, "ERROR: Server closed without replying.\n");
        close(client_fd);
        return EXIT_FAILURE;
    }

    if (result == -1) {
        perror("ERROR: Failed to receive response length header");
        close(client_fd);
        return EXIT_FAILURE;
    }

    uint32_t response_length = ntohl(response_network_length);

    /* Validate the server's length before using it as a buffer size. */
    if (response_length > MAX_FRAME_SIZE) {
        fprintf(stderr,
                "ERROR: Server response exceeds the %d-byte limit.\n",
                MAX_FRAME_SIZE);
        close(client_fd);
        return EXIT_FAILURE;
    }

    char response[MAX_FRAME_SIZE];

    /* Read exactly the number of response bytes specified in the header. */
    result = recv_exact(client_fd, response, response_length);

    if (result != 1) {
        if (result == -1) {
            perror("ERROR: Failed to receive complete response payload");
        } else {
            fprintf(stderr, "ERROR: Server closed before sending its reply.\n");
        }

        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Client received framed reply (%u bytes): %.*s\n",
           (unsigned int)response_length, (int)response_length, response);

    close(client_fd);
    return EXIT_SUCCESS;
}
