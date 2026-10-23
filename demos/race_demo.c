#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BALANCE_FILE "race_balance.txt"

/*
 * Read the entire small balance file.
 * Returns the balance on success, or -1 on error.
 */
static long read_balance(void)
{
    char buffer[64];
    int fd = open(BALANCE_FILE, O_RDONLY);

    if (fd == -1) {
        perror("ERROR: Cannot open race_balance.txt for reading");
        return -1;
    }

    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);

    if (n == -1) {
        perror("ERROR: Failed to read the current balance");
        close(fd);
        return -1;
    }

    if (n == 0) {
        fprintf(stderr, "ERROR: Balance file is empty\n");
        close(fd);
        return -1;
    }

    buffer[n] = '\0';

    if (close(fd) == -1) {
        perror("ERROR: Failed to close the balance file after reading");
        return -1;
    }

    char *newline = strchr(buffer, '\n');

    if (newline != NULL) {
        *newline = '\0';
    }

    char *end = NULL;
    errno = 0;
    long balance = strtol(buffer, &end, 10);

    if (errno != 0 || end == buffer || *end != '\0' || balance < 0) {
        fprintf(stderr,
                "ERROR: Balance file contains an invalid amount: '%s'\n",
                buffer);
        return -1;
    }

    return balance;
}

/*
 * Replace the balance file with a new balance.
 * Returns 0 on success, or -1 on error.
 */
static int write_balance(long balance)
{
    char buffer[64];
    int length = snprintf(buffer, sizeof(buffer), "%ld\n", balance);

    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        fprintf(stderr, "ERROR: Could not format the new balance\n");
        return -1;
    }

    int fd = open(BALANCE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd == -1) {
        perror("ERROR: Cannot open balance file for writing");
        return -1;
    }

    size_t done = 0;

    while (done < (size_t)length) {
        ssize_t n = write(fd, buffer + done, (size_t)length - done);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("ERROR: Failed to write the new balance");
            close(fd);
            return -1;
        }

        if (n == 0) {
            fprintf(stderr, "ERROR: Balance write made no progress\n");
            close(fd);
            return -1;
        }

        done += (size_t)n;
    }

    if (fsync(fd) == -1) {
        perror("ERROR: Failed to synchronize the balance file");
        close(fd);
        return -1;
    }

    if (close(fd) == -1) {
        perror("ERROR: Failed to close the balance file after writing");
        return -1;
    }

    return 0;
}

/*
 * Each child reads the same starting balance, pauses to make the race
 * easier to observe, then writes its own calculated deposit result.
 *
 * The pause deliberately gives both processes time to read the old value.
 * There is intentionally no lock around the read-modify-write sequence.
 */
static void deposit_without_lock(int child_number)
{
    long balance = read_balance();

    if (balance == -1) {
        _exit(EXIT_FAILURE);
    }

    printf("Process %d (PID %ld) read balance: %ld\n",
           child_number, (long)getpid(), balance);
    fflush(stdout);

    /* Deliberately widen the race window for this experiment. */
    sleep(1);

    long new_balance = balance + 10;

    if (write_balance(new_balance) == -1) {
        _exit(EXIT_FAILURE);
    }

    printf("Process %d (PID %ld) wrote balance: %ld\n",
           child_number, (long)getpid(), new_balance);
    fflush(stdout);

    _exit(EXIT_SUCCESS);
}

int main(void)
{
    /* Start every experiment with a known balance of 100. */
    if (write_balance(100) == -1) {
        fprintf(stderr, "ERROR: Could not initialize the race experiment\n");
        return EXIT_FAILURE;
    }

    printf("Starting balance: 100\n");
    fflush(stdout);

    pid_t first = fork();

    if (first == -1) {
        perror("ERROR: Could not create the first child process");
        return EXIT_FAILURE;
    }

    if (first == 0) {
        deposit_without_lock(1);
    }

    pid_t second = fork();

    if (second == -1) {
        perror("ERROR: Could not create the second child process");
        (void)waitpid(first, NULL, 0);
        return EXIT_FAILURE;
    }

    if (second == 0) {
        deposit_without_lock(2);
    }

    int failed = 0;
    int status;

    if (waitpid(first, &status, 0) == -1) {
        perror("ERROR: Could not wait for the first child process");
        failed = 1;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR: The first deposit process failed\n");
        failed = 1;
    }

    if (waitpid(second, &status, 0) == -1) {
        perror("ERROR: Could not wait for the second child process");
        failed = 1;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR: The second deposit process failed\n");
        failed = 1;
    }

    if (failed) {
        fprintf(stderr,
                "ERROR: At least one child failed; the experiment "
                "result may be incomplete\n");
        return EXIT_FAILURE;
    }

    long final_balance = read_balance();

    if (final_balance == -1) {
        fprintf(stderr, "ERROR: Could not read the final balance\n");
        return EXIT_FAILURE;
    }

    printf("Final balance: %ld\n", final_balance);
    printf("Expected balance if both deposits succeed: 120\n");

    if (final_balance != 120) {
        printf("LOST UPDATE OBSERVED: At least one deposit was overwritten.\n");
    } else {
        printf("Both deposits are reflected in the final balance.\n");
    }

    return EXIT_SUCCESS;
}
