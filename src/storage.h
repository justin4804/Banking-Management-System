#ifndef STORAGE_H
#define STORAGE_H

#include <sys/types.h>

#define ACCOUNT_ID_SIZE 10
#define RECORD_SIZE 53
#define BALANCE_SIZE 15

int read_full(int fd, char *buffer, size_t size);
int write_full(int fd, const char *buffer, size_t size);

int lock_record(int fd, off_t offset);
int unlock_record(int fd, off_t offset);

int find_account(int fd, const char *account_id, off_t *record_offset);
int get_balance(int fd, off_t record_offset, double *balance);

#endif