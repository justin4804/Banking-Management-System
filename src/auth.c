#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "auth.h"

#define USER_RECORD_SIZE 89

static int read_exact(int fd, char *buffer, size_t count)
{
    size_t total = 0;

    while (total < count) {
        ssize_t n = read(fd, buffer + total, count - total);

        if (n == 0) {
            return total == 0 ? 0 : -1;
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        total += (size_t)n;
    }

    return 1;
}

/* Copy a fixed-width field and remove trailing spaces. */
static void copy_field(char *dest, size_t dest_size,
                       const char *src, size_t field_size)
{
    size_t len = field_size;

    while (len > 0 && src[len - 1] == ' ') {
        len--;
    }

    if (len >= dest_size) {
        len = dest_size - 1;
    }

    memcpy(dest, src, len);
    dest[len] = '\0';
}

int authenticate_user(
    const char *username,
    const char *password,
    char user_id[USER_ID_SIZE + 1],
    char *role,
    char first_name[21])
{
    int fd = open("../data/users.txt", O_RDONLY);

    if (fd < 0) {
        perror("open users.txt");
        return -1;
    }

    char record[USER_RECORD_SIZE];

    while (1) {
        int result = read_exact(fd, record, sizeof(record));

        if (result == 0) {
            break;  /* End of file */
        }

        if (result < 0) {
            fprintf(stderr, "Error reading users.txt\n");
            close(fd);
            return -1;
        }

        if (record[88] != '\n') {
            fprintf(stderr, "Invalid user record format\n");
            close(fd);
            return -1;
        }

        char stored_username[21];
        char stored_password[21];

        copy_field(stored_username, sizeof(stored_username),
                   record + 47, 20);

        if (strcmp(stored_username, username) != 0) {
            continue;
        }

        /* Reject inactive users before granting access. */
        if (record[87] != 'A') {
            close(fd);
            return -1;
        }

        copy_field(stored_password, sizeof(stored_password),
                   record + 67, 20);

        if (strcmp(stored_password, password) != 0) {
            close(fd);
            return -1;
        }

        memcpy(user_id, record, USER_ID_SIZE);
        user_id[USER_ID_SIZE] = '\0';

        *role = record[6];

        copy_field(first_name, 21, record + 7, 20);

        close(fd);
        return 0;
    }

    close(fd);
    return -1;
}