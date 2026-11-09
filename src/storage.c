#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include "storage.h"

#define RECORD_SIZE 53
#define ACCOUNT_ID_SIZE 10
#define BALANCE_OFFSET 16
#define BALANCE_SIZE 15

/* Read exactly the requested number of bytes. */
int read_full(int fd, char *buffer, size_t size)
{
    size_t total = 0;

    while (total < size)
    {
        ssize_t n = read(fd, buffer + total, size - total);

        if (n == -1)
        {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0)
            return total == 0 ? 0 : -1;

        total += n;
    }

    return total;
}

/* Write all bytes unless an error occurs. */
int write_full(int fd, const char *buffer, size_t size)
{
    size_t total = 0;

    while (total < size)
    {
        ssize_t n = write(fd, buffer + total, size - total);

        if (n == -1)
        {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0)
            return -1;

        total += n;
    }

    return 0;
}

/* Acquire or release a lock on one account record. */
int record_lock(int fd, off_t offset, short type)
{
    struct flock lock = {0};

    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = offset;
    lock.l_len = RECORD_SIZE;

    int command = (type == F_UNLCK) ? F_SETLK : F_SETLKW;

    return fcntl(fd, command, &lock);
}

int find_account(int fd, const char *account_id, off_t *record_offset)
{
    char record[RECORD_SIZE];
    off_t offset = 0;

    while (1)
    {
        if (lseek(fd, offset, SEEK_SET) == -1)
        {
            perror("lseek");
            return -1;
        }

        ssize_t bytes_read = read_full(fd, record, RECORD_SIZE);

        if (bytes_read == 0)
        {
            /* Reached the end of the file without finding the account. */
            return -1;
        }

        if (bytes_read == -1)
        {
            perror("read");
            return -1;
        }

        if (record[RECORD_SIZE - 1] != '\n')
        {
            fprintf(stderr, "Invalid account record at offset %lld\n",
                    (long long)offset);
            return -1;
        }

        if (memcmp(record, account_id, 10) == 0)
        {
            *record_offset = offset;
            return 0;
        }

        offset += RECORD_SIZE;
    }
}

int get_balance(int fd, off_t record_offset, double *balance)
{
    char record[RECORD_SIZE];
    char balance_text[16];

    if (lseek(fd, record_offset, SEEK_SET) == -1)
    {
        perror("lseek");
        return -1;
    }

    ssize_t bytes_read = read_full(fd, record, RECORD_SIZE);

    if (bytes_read != RECORD_SIZE)
    {
        fprintf(stderr, "Could not read the complete account record\n");
        return -1;
    }

    if (record[RECORD_SIZE - 1] != '\n')
    {
        fprintf(stderr, "Invalid account record\n");
        return -1;
    }

    /* The balance starts at byte 16 and occupies 15 bytes. */
    memcpy(balance_text, record + 16, 15);
    balance_text[15] = '\0';

    char *end;
    double value = strtod(balance_text, &end);

    if (end == balance_text || *end != '\0' ||
        !isfinite(value) || value < 0)
    {
        fprintf(stderr, "Invalid balance in account record\n");
        return -1;
    }

    *balance = value;

    return 0;
}

int lock_record(int fd, off_t offset)
{
    struct flock lock;

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = offset;
    lock.l_len = RECORD_SIZE;
    lock.l_pid = 0;

    if (fcntl(fd, F_SETLKW, &lock) == -1)
    {
        perror("fcntl lock");
        return -1;
    }

    return 0;
}

int unlock_record(int fd, off_t offset)
{
    struct flock lock;

    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = offset;
    lock.l_len = RECORD_SIZE;
    lock.l_pid = 0;

    if (fcntl(fd, F_SETLK, &lock) == -1)
    {
        perror("fcntl unlock");
        return -1;
    }

    return 0;
}

#ifndef STORAGE_LIBRARY

int main(int argc, char *argv[])
{
    if (argc == 3 && strcmp(argv[1], "balance") == 0)
    {
        const char *account_id = argv[2];
        off_t record_offset;
        double balance;

        int fd = open("../data/accounts.txt", O_RDONLY);

        if (fd == -1)
        {
            perror("open");
            return 1;
        }

        if (find_account(fd, account_id, &record_offset) == -1)
        {
            fprintf(stderr, "Account not found.\n");
            close(fd);
            return 1;
        }

        if (get_balance(fd, record_offset, &balance) == -1)
        {
            printf("Could not retrieve account balance.\n");
            close(fd);
            return 1;
        }

        printf("Account ID: %s\n", account_id);
        printf("Balance: Rs. %.2f\n", balance);

        close(fd);
        return 0;
    }

    if (argc != 4)
    {
        fprintf(stderr,
                "Usage: %s ACCOUNT_ID AMOUNT ITERATIONS\n",
                argv[0]);
        return 1;
    }

    const char *account_id = argv[1];

    if (strlen(account_id) != ACCOUNT_ID_SIZE)
    {
        fprintf(stderr, "Account ID must contain 10 digits.\n");
        return 1;
    }

    for (int i = 0; i < ACCOUNT_ID_SIZE; i++)
    {
        if (account_id[i] < '0' || account_id[i] > '9')
        {
            fprintf(stderr, "Account ID must contain digits only.\n");
            return 1;
        }
    }

    char *end;
    errno = 0;
    double amount = strtod(argv[2], &end);

    if (errno != 0 || *argv[2] == '\0' || *end != '\0' ||
        amount <= 0)
    {
        fprintf(stderr, "Invalid amount.\n");
        return 1;
    }

    errno = 0;
    long iterations = strtol(argv[3], &end, 10);

    if (errno != 0 || *argv[3] == '\0' || *end != '\0' ||
        iterations <= 0 || iterations > 100000)
    {
        fprintf(stderr, "Invalid iteration count.\n");
        return 1;
    }

    int fd = open("../data/accounts.txt", O_RDWR);

    if (fd == -1)
    {
        perror("open");
        return 1;
    }

    char record[RECORD_SIZE];
    off_t offset = 0;
    int found = 0;

    /* Find the account by scanning fixed-width records. */
    while (1)
    {
        ssize_t result = read_full(fd, record, RECORD_SIZE);

        if (result == 0)
            break;

        if (result == -1)
        {
            fprintf(stderr, "Error reading account records.\n");
            close(fd);
            return 1;
        }

        if (record[RECORD_SIZE - 1] != '\n')
        {
            fprintf(stderr, "Malformed account record.\n");
            close(fd);
            return 1;
        }

        if (memcmp(record, account_id, ACCOUNT_ID_SIZE) == 0)
        {
            found = 1;
            break;
        }

        offset += RECORD_SIZE;
    }

    if (!found)
    {
        fprintf(stderr, "Account not found.\n");
        close(fd);
        return 1;
    }

    /* Update the same account repeatedly. */
    for (long i = 0; i < iterations; i++)
    {

        /* Lock the account record. */
        if (lock_record(fd, offset) == -1)
        {
            close(fd);
            return 1;
        }

        int success = 0;

        /* Always re-read the record after locking. */
        if (lseek(fd, offset, SEEK_SET) == -1)
        {
            perror("lseek");
            goto iteration_error;
        }

        if (read_full(fd, record, RECORD_SIZE) < 1)
        {
            fprintf(stderr, "Could not read complete account.\n");
            goto iteration_error;
        }

        if (memcmp(record, account_id, ACCOUNT_ID_SIZE) != 0)
        {
            fprintf(stderr, "Account ID mismatch.\n");
            goto iteration_error;
        }

        if (record[31] != 'A')
        {
            fprintf(stderr, "Account is not active.\n");
            goto iteration_error;
        }

        /* Extract and validate the existing balance. */
        char balance_text[BALANCE_SIZE + 1];

        memcpy(balance_text, record + BALANCE_OFFSET, BALANCE_SIZE);
        balance_text[BALANCE_SIZE] = '\0';

        char *balance_end;
        errno = 0;
        double balance = strtod(balance_text, &balance_end);

        if (errno != 0 || balance_end == balance_text ||
            *balance_end != '\0')
        {
            fprintf(stderr, "Invalid balance in account record.\n");
            goto iteration_error;
        }

        /* Calculate the new balance. */
        double new_balance = balance + amount;

        if (new_balance < 0 || new_balance >= 1000000000000.0)
        {
            fprintf(stderr, "Balance exceeds supported range.\n");
            goto iteration_error;
        }

        char new_balance_text[BALANCE_SIZE + 1];

        int length = snprintf(new_balance_text,
                              sizeof(new_balance_text),
                              "%015.2f", new_balance);

        if (length != BALANCE_SIZE)
        {
            fprintf(stderr, "New balance does not fit.\n");
            goto iteration_error;
        }

        memcpy(record + BALANCE_OFFSET,
               new_balance_text, BALANCE_SIZE);

        /* Write the complete updated record at the same offset. */
        if (lseek(fd, offset, SEEK_SET) == -1)
        {
            perror("lseek");
            goto iteration_error;
        }

        if (write_full(fd, record, RECORD_SIZE) == -1)
        {
            perror("write");
            goto iteration_error;
        }

        /* Flush the update before releasing the lock. */
        if (fsync(fd) == -1)
        {
            perror("fsync");
            goto iteration_error;
        }

        success = 1;

    iteration_error:
        /* Release the lock on success or failure. */
        if (unlock_record(fd, offset) == -1)
        {
            close(fd);
            return 1;
        }

        if (!success)
        {
            close(fd);
            return 1;
        }
    }

    printf("Process %ld completed %ld updates.\n",
           (long)getpid(), iterations);

    close(fd);
    return 0;
}

#endif