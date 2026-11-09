#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <math.h>

int main(void)
{
    pid_t p1 = fork();

    if (p1 == -1) {
        perror("fork");
        return 1;
    }

    if (p1 == 0) {
        execl("./storage", "storage",
              "0000000002", "100", "100", (char *)NULL);

        perror("execl");
        _exit(1);
    }

    pid_t p2 = fork();

    if (p2 == -1) {
        perror("fork");
        waitpid(p1, NULL, 0);
        return 1;
    }

    if (p2 == 0) {
        execl("./storage", "storage",
              "0000000002", "100", "100", (char *)NULL);

        perror("execl");
        _exit(1);
    }

    int status1, status2;

    if (waitpid(p1, &status1, 0) == -1 ||
        waitpid(p2, &status2, 0) == -1) {
        perror("waitpid");
        return 1;
    }

    if (!WIFEXITED(status1) || WEXITSTATUS(status1) != 0 ||
        !WIFEXITED(status2) || WEXITSTATUS(status2) != 0) {
        fprintf(stderr, "At least one worker failed.\n");
        return 1;
    }

    printf("Both processes completed successfully.\n");
    printf("Expected total increase: Rs. 20000.00\n");

    return 0;
}