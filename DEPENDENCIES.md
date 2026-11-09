# Dependencies and Environment

## Required

-   Linux/Unix operating system (Ubuntu is suitable).
-   GCC C compiler with C11 support.
-   Standard terminal/shell tools.
-   No external package manager or third-party library is required for
    the described implementation.

## APIs Used

  -----------------------------------------------------------------------
  API / headers                       Purpose
  ----------------------------------- -----------------------------------
  Standard C library (`stdio.h`,      Input/output, memory and string
  `stdlib.h`, `string.h`)             operations

  POSIX file I/O (`fcntl.h`,          Opening, reading, writing, seeking
  `unistd.h`, `sys/types.h`)          and closing files

  `fcntl` record locking              Synchronizing access to shared
  (`struct flock`)                    records

  `fsync`                             Requesting that file updates be
                                      flushed to storage

  TCP sockets (`sys/socket.h`,        Client-server communication
  `netinet/in.h`, `arpa/inet.h`)      

  Process APIs (`unistd.h`,           `fork`-based concurrent client
  `sys/wait.h`)                       handling and child-process
                                      management

  `time.h`                            Timestamps, where used
  -----------------------------------------------------------------------

Exact headers vary by source file and implementation.

## Build Commands

Client:

``` sh
gcc -Wall -Wextra -std=c11 client.c -o client
```

Server (adjust source list to match your project):

``` sh
gcc -Wall -Wextra -std=c11 server.c auth.c storage.c -o server
```

Include every `.c` file that defines functions referenced by the server.
Header files declare interfaces but do not replace implementation files.

## Runtime Setup

-   Ensure the data directory and required files exist before launching
    the server.
-   Run from the working directory expected by relative paths such as
    `../data/users.txt`.
-   Ensure the server has read/write permission for the data directory.
-   Ensure the configured TCP port (currently `5555` in the project
    setup) is available and identical in client and server
    configuration.
-   Start the server before starting clients.

## Dependencies Not Required

Based on the described implementation, the project does not require a
database server, Python packages, Node.js, or external C libraries.

## Limitations to Document

-   Passwords are plaintext for coursework simplicity.
-   `fcntl` locks are advisory, so all processes accessing these files
    must follow the same locking protocol.
-   File locks and `fsync` do not guarantee atomic updates across
    multiple files or full recovery after a crash.
