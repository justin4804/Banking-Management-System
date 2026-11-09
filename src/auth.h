#ifndef AUTH_H
#define AUTH_H

#define USER_ID_SIZE 6
#define USERNAME_SIZE 20
#define PASSWORD_SIZE 20

/*
 * Returns 0 on success and -1 on failure.
 * On success, fills in the user ID, role, and name.
 */
int authenticate_user(
    const char *username,
    const char *password,
    char user_id[USER_ID_SIZE + 1],
    char *role,
    char first_name[21]
);

#endif