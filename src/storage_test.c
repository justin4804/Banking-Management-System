#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "storage.h"

int main(void)
{
    char account_id[ACCOUNT_ID_SIZE + 1];
    off_t offset;
    double balance;

    printf("Enter account ID: ");

    if (scanf("%10s", account_id) != 1)
    {
        fprintf(stderr, "Invalid input.\n");
        return 1;
    }

    if (account_id[ACCOUNT_ID_SIZE] != '\0')
    {
        fprintf(stderr, "Invalid account ID.\n");
        return 1;
    }

    int fd = open("../data/accounts.txt", O_RDONLY);

    if (fd == -1)
    {
        perror("open");
        return 1;
    }

    if (find_account(fd, account_id, &offset) == -1)
    {
        printf("Account not found or could not be read.\n");
        close(fd);
        return 1;
    }

    if (get_balance(fd, offset, &balance) == -1)
    {
        printf("Could not read account balance.\n");
        close(fd);
        return 1;
    }

    printf("Account found at byte offset: %lld\n", (long long)offset);
    printf("Current balance: Rs. %.2f\n", balance);

    close(fd);
    return 0;
}