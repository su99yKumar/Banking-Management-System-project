#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RECORD_SIZE 256
#define USER_FILE "users.txt"

/*
 * Copy a value into a fixed-width field.
 *
 * The record is pre-filled with spaces, so unused bytes remain padded.
 * Returns 0 on success, or -1 if the value is too long.
 */
static int put_field(char *record, size_t offset,
                     size_t width, const char *value)
{
    size_t len = strlen(value);

    if (len > width) {
        fprintf(stderr,
                "ERROR: Field at byte %zu allows %zu bytes, "
                "but the supplied value needs %zu bytes.\n",
                offset, width, len);
        errno = EINVAL;
        return -1;
    }

    memcpy(record + offset, value, len);
    return 0;
}

/*
 * Build one complete 256-byte customer record.
 *
 * Fields contain an ID, role, username, name, password placeholders,
 * balance in cents, account status, padding, and a final newline.
 * Returns 0 on success, or -1 if any field cannot fit.
 */
static int make_record(char *record, const char *id,
                       const char *role, const char *username,
                       const char *name, const char *balance)
{
    char empty_hash[65];

    /* Start with spaces so every record has exactly 256 bytes. */
    memset(record, ' ', RECORD_SIZE);

    /* Use a fixed-width placeholder until password hashing is added. */
    memset(empty_hash, '0', 64);
    empty_hash[64] = '\0';

    /* Place separators and the final newline at fixed byte positions. */
    record[6] = '|';
    record[8] = '|';
    record[33] = '|';
    record[66] = '|';
    record[83] = '|';
    record[148] = '|';
    record[161] = '|';
    record[162] = 'A';
    record[255] = '\n';

    /* Copy each field into its assigned position. */
    if (put_field(record, 0, 6, id) == -1 ||
        put_field(record, 7, 1, role) == -1 ||
        put_field(record, 9, 24, username) == -1 ||
        put_field(record, 34, 32, name) == -1 ||
        put_field(record, 67, 16, "0000000000000000") == -1 ||
        put_field(record, 84, 64, empty_hash) == -1 ||
        put_field(record, 149, 12, balance) == -1) {
        fprintf(stderr,
                "ERROR: Could not build the record for user ID %s. "
                "Check that every field fits its assigned width.\n",
                id);
        return -1;
    }

    return 0;
}

/*
 * Write all requested bytes at a specific file offset.
 *
 * pwrite() can write fewer bytes than requested, so keep writing
 * until the entire buffer is stored. Retry if interrupted by a signal.
 * Returns 0 on success, or -1 on a system-call error.
 */
static int pwrite_all(int fd, const void *buffer,
                      size_t count, off_t offset)
{
    const char *bytes = buffer;
    size_t done = 0;

    while (done < count) {
        ssize_t n = pwrite(fd, bytes + done, count - done,
                           offset + (off_t)done);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr,
                    "ERROR: Failed to write database data at byte "
                    "%lld after writing %zu of %zu bytes.\n",
                    (long long)(offset + (off_t)done), done, count);
            return -1;
        }

        if (n == 0) {
            fprintf(stderr,
                    "ERROR: Database write made no progress at "
                    "byte %lld; the record may be incomplete.\n",
                    (long long)(offset + (off_t)done));
            errno = EIO;
            return -1;
        }

        done += (size_t)n;
    }

    return 0;
}

/*
 * Read exactly one 256-byte record from a specified file offset.
 *
 * Retry interrupted reads. If the file ends before the complete record
 * is read, report an error instead of processing a partial record.
 * Returns 0 on success, or -1 on failure.
 */
static int pread_record(int fd, char *record, off_t offset)
{
    size_t done = 0;

    while (done < RECORD_SIZE) {
        ssize_t n = pread(fd, record + done, RECORD_SIZE - done,
                          offset + (off_t)done);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr,
                    "ERROR: Failed to read a complete record "
                    "starting at byte %lld.\n",
                    (long long)offset);
            return -1;
        }

        if (n == 0) {
            fprintf(stderr,
                    "ERROR: Unexpected end of users.txt at byte "
                    "%lld. The database may contain a truncated "
                    "record.\n",
                    (long long)(offset + (off_t)done));
            errno = EIO;
            return -1;
        }

        done += (size_t)n;
    }

    return 0;
}

/*
 * Print one fixed-width field without its unused trailing spaces.
 *
 * The field itself is not modified, so the original database record
 * remains unchanged.
 */
static void print_field(const char *record,
                        size_t offset, size_t width)
{
    size_t len = width;

    while (len > 0 && record[offset + len - 1] == ' ') {
        len--;
    }

    (void)fwrite(record + offset, 1, len, stdout);
}

/*
 * Read and display every record in the database.
 *
 * Each record must be exactly 256 bytes and end with a newline.
 * Returns 0 if all records are valid and readable, or -1 on error.
 */
static int show_records(int fd, off_t file_size)
{
    char record[RECORD_SIZE];
    int number = 1;

    for (off_t offset = 0; offset < file_size;
         offset += RECORD_SIZE, number++) {

        if (pread_record(fd, record, offset) == -1) {
            fprintf(stderr,
                    "ERROR: Could not display database record %d.\n",
                    number);
            return -1;
        }

        if (record[RECORD_SIZE - 1] != '\n') {
            fprintf(stderr,
                    "ERROR: Record %d, starting at byte %lld, "
                    "does not end with a newline. "
                    "The record layout may be corrupted.\n",
                    number, (long long)offset);
            return -1;
        }

        printf("Record %d, byte offset %lld: ID=",
               number, (long long)offset);

        print_field(record, 0, 6);
        printf(" role=");
        print_field(record, 7, 1);
        printf(" username=");
        print_field(record, 9, 24);
        printf(" name=");
        print_field(record, 34, 32);
        printf(" balance_cents=");
        print_field(record, 149, 12);
        printf(" status=");
        print_field(record, 162, 1);
        printf("\n");
    }

    return 0;
}

/*
 * Open users.txt, initialize two demo customers if the file is empty,
 * synchronize the initial records, validate the file size, and display
 * the records. The file is created with owner-only permissions.
 *
 * Returns EXIT_SUCCESS on success and EXIT_FAILURE on error.
 */
int main(void)
{
    int fd = open(USER_FILE, O_RDWR | O_CREAT, 0600);
    struct stat st;

    if (fd == -1) {
        perror("ERROR: Cannot open or create users.txt");
        fprintf(stderr,
                "Check that ~/bank exists and you have permission "
                "to access it.\n");
        return EXIT_FAILURE;
    }

    if (fstat(fd, &st) == -1) {
        perror("ERROR: Cannot inspect the size of users.txt");
        close(fd);
        return EXIT_FAILURE;
    }

    /* Create sample records only when the database is empty. */
    if (st.st_size == 0) {
        char record[RECORD_SIZE];

        if (make_record(record, "000001", "C", "alice",
                        "Alice Smith", "000000010000") == -1 ||
            pwrite_all(fd, record, RECORD_SIZE, 0) == -1) {
            fprintf(stderr,
                    "ERROR: Could not initialize customer Alice "
                    "in users.txt.\n");
            close(fd);
            return EXIT_FAILURE;
        }

        if (make_record(record, "000002", "C", "bob",
                        "Bob Jones", "000000050000") == -1 ||
            pwrite_all(fd, record, RECORD_SIZE, RECORD_SIZE) == -1) {
            fprintf(stderr,
                    "ERROR: Could not initialize customer Bob "
                    "in users.txt. The database may be incomplete.\n");
            close(fd);
            return EXIT_FAILURE;
        }

        /* Request that both initialized records reach persistent storage. */
        if (fsync(fd) == -1) {
            perror("ERROR: Could not synchronize users.txt to storage");
            close(fd);
            return EXIT_FAILURE;
        }

        printf("SUCCESS: Initialized users.txt with 2 records "
               "(256 bytes each).\n");

        if (fstat(fd, &st) == -1) {
            perror("ERROR: Could not verify users.txt after initialization");
            close(fd);
            return EXIT_FAILURE;
        }
    } else {
        printf("INFO: Found existing users.txt (%lld bytes).\n",
               (long long)st.st_size);
    }

    /* Reject a file whose size cannot contain complete fixed-width records. */
    if (st.st_size % RECORD_SIZE != 0) {
        fprintf(stderr,
                "ERROR: users.txt is %lld bytes long, which is not "
                "a multiple of %d. A record may be truncated or "
                "extra bytes may have been appended.\n",
                (long long)st.st_size, RECORD_SIZE);
        close(fd);
        return EXIT_FAILURE;
    }

    int result = show_records(fd, st.st_size);

    if (close(fd) == -1) {
        perror("ERROR: Failed to close users.txt");
        return EXIT_FAILURE;
    }

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
