
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

#define PORT 5052
#define MAX_FRAME_SIZE 1024

/* Read exactly len bytes, unless the peer closes or an error occurs. */
static int recv_exact(int fd, void *buffer, size_t len)
{
    size_t total = 0;
    char *p = buffer;

    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, 0);

        if (n == 0)
            return total == 0 ? 0 : -1;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        total += (size_t)n;
    }

    return 1;
}

/* Send all bytes, handling partial sends and interrupted calls. */
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

        if (n == 0)
            return -1;

        total += (size_t)n;
    }

    return 0;
}

/* Reap all children that have exited; do not block the accept loop. */
static void reap_children(int signal_number)
{
    (void)signal_number;

    int saved_errno = errno;

    while (waitpid(-1, NULL, WNOHANG) > 0) {
        /* Collect every exited child currently waiting to be reaped. */
    }

    errno = saved_errno;
}

/* Handle one connected client in the child process. */
static void handle_client(int client_fd)
{
    uint32_t network_length;

    int result = recv_exact(client_fd, &network_length,
                            sizeof(network_length));

    if (result != 1) {
        fprintf(stderr, "Child %ld: failed to read frame header\n",
                (long)getpid());
        return;
    }

    uint32_t message_length = ntohl(network_length);

    if (message_length > MAX_FRAME_SIZE) {
        fprintf(stderr, "Child %ld: frame exceeds %d bytes\n",
                (long)getpid(), MAX_FRAME_SIZE);
        return;
    }

    char message[MAX_FRAME_SIZE + 1];

    result = recv_exact(client_fd, message, message_length);

    if (result != 1) {
        fprintf(stderr, "Child %ld: incomplete frame payload\n",
                (long)getpid());
        return;
    }

    message[message_length] = '\0';

    printf("Child %ld received: %s\n", (long)getpid(), message);
    fflush(stdout);

    if (send_all(client_fd, &network_length, sizeof(network_length)) == -1 ||
        send_all(client_fd, message, message_length) == -1) {
        fprintf(stderr, "Child %ld: failed to send reply\n",
                (long)getpid());
    }
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("ERROR: Cannot install SIGCHLD handler");
        return EXIT_FAILURE;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("ERROR: Cannot create server socket");
        return EXIT_FAILURE;
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1) {
        perror("ERROR: Cannot set SO_REUSEADDR");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server_fd, (struct sockaddr *)&address,
             sizeof(address)) == -1) {
        perror("ERROR: Cannot bind server socket");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 10) == -1) {
        perror("ERROR: Cannot listen on server socket");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Fork server listening on 127.0.0.1:%d\n", PORT);
    fflush(stdout);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd == -1) {
            if (errno == EINTR)
                continue;

            perror("ERROR: Cannot accept client");
            continue;
        }

        pid_t pid = fork();

        if (pid == -1) {
            perror("ERROR: Cannot fork client handler");
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            /* Child: it handles this client, not future connections. */
            close(server_fd);
            handle_client(client_fd);
            close(client_fd);
            _exit(EXIT_SUCCESS);
        }

        /* Parent: it keeps accepting other clients. */
        close(client_fd);
    }
}
