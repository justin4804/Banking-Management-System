# Banking Management System

## Overview

A coursework banking application written in C for Linux. A client
communicates with a server over TCP sockets. The server owns the data
files, authenticates users, enforces role-based operations, and handles
multiple clients using separate processes.

## Roles and Features

-   **Customer:** create account, login, check balance, deposit,
    withdraw, transfer, apply for loans, view transaction history,
    change password, submit feedback.
-   **Employee:** add/modify customer details, view assigned loans,
    process loan decisions, view passbook, change password.
-   **Manager:** activate/deactivate accounts, assign loans to
    employees, review feedback, change password.
-   **Administrator:** add employees, modify customer/employee details,
    manage roles, change password.

## Main Components

-   `client.c`: menus, input handling, request formatting, response
    display.
-   `server.c`: TCP listener, client connections, request dispatch,
    server-side operations.
-   `auth.c` / `auth.h`: authentication and user-status checks.
-   `storage.c` / `storage.h`: fixed-width file-record access and
    locking helpers.
-   Data files: users, accounts, transactions, loans, feedback, and
    session-lock state.
-   Logging: records server activity while redacting sensitive request
    fields such as passwords.

## Data Files

The current source uses relative paths such as `../data/users.txt`. Run
the server from the directory expected by those paths, and initialize
the required data files first.

Typical files: - `users.txt` --- user identities, roles, credentials,
and status. - `accounts.txt` --- account IDs, owner IDs, balances, and
status. - `transactions.txt` --- completed transaction records. -
`loans.txt` --- applications, employee assignment, and loan decisions. -
`feedback.txt` --- feedback messages and review status. -
`sessions.lock` --- coordinates single-session-per-user behavior.

Back up test data before destructive testing. Do not manually edit data
files while the server is running.

## Build and Run

Compile the client:

``` sh
gcc -Wall -Wextra -std=c11 client.c -o client
```

Compile the server using the implementation files present in your
project. For a layout containing `server.c`, `auth.c`, and `storage.c`:

``` sh
gcc -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L     -DSTORAGE_LIBRARY server.c storage.c auth.c -lm -o server
```

Start the server first, then run one or more clients in separate
terminals:

``` sh
./server
./client
```

The client and server port settings must match. The current project
setup has used TCP port `5555`. Adjust commands to match the actual
directory and source-file layout.

## Concurrency and Consistency

-   Separate server processes handle multiple client connections.
-   `fcntl` record locks coordinate access to shared records.
-   Transfers should lock account records in a consistent order to
    reduce deadlock risk.
-   Session locking prevents simultaneous sessions for the same user.
-   Writes call `fsync` where implemented, but there is no complete
    write-ahead journal/recovery mechanism. Therefore, do not claim full
    crash-atomic ACID behavior across multiple file updates.

## Security and Limitations

Passwords are stored in plaintext as a coursework simplification. This
is not suitable for production or real banking data. Use only test data
in a controlled environment. Verify role permissions, input validation,
and concurrency behavior before demonstration.
