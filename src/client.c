#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_IP "127.0.0.1"
#define PORT 5555
#define REQUEST_SIZE 256
#define RESPONSE_SIZE 512

#define USERNAME_SIZE 20
#define PASSWORD_SIZE 20

static int send_all(int fd, const void* buffer, size_t count) {
  const char* p = buffer;
  size_t sent = 0;

  while (sent < count) {
    ssize_t n = send(fd, p + sent, count - sent, MSG_NOSIGNAL);

    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }

    if (n == 0) {
      return -1;
    }

    sent += (size_t)n;
  }

  return 0;
}

static int recv_all(int fd, void* buffer, size_t count) {
  char* p = buffer;
  size_t received = 0;

  while (received < count) {
    ssize_t n = recv(fd, p + received, count - received, 0);

    if (n == 0) {
      return -1;
    }

    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }

    received += (size_t)n;
  }

  return 0;
}

static const char* get_command(char role, int choice) {
  switch (role) {
    case 'C':
      switch (choice) {
        case 1:
          return "BALANCE";
        case 2:
          return "DEPOSIT";
        case 3:
          return "WITHDRAW";
        case 4:
          return "TRANSFER";
        case 5:
          return "APPLY_LOAN";
        case 6:
          return "HISTORY";
        case 7:
          return "CHANGE_PASSWORD";
        case 8:
          return "FEEDBACK";
        case 0:
          return "LOGOUT";
        case 9:
          return "EXIT";
        default:
          return NULL;
      }

    case 'E':
      switch (choice) {
        case 1:
          return "ADD_CUSTOMER";
        case 2:
          return "MODIFY_CUSTOMER";
        case 3:
          return "VIEW_ASSIGNED_LOANS";
        case 4:
          return "PROCESS_LOAN";
        case 5:
          return "PASSBOOK";
        case 6:
          return "CHANGE_PASSWORD";
        case 0:
          return "LOGOUT";
        case 9:
          return "EXIT";
        default:
          return NULL;
      }

    case 'M':
      switch (choice) {
        case 1:
          return "ACCOUNT_STATUS";
        case 2:
          return "ASSIGN_LOAN";
        case 3:
          return "REVIEW_FEEDBACK";
        case 4:
          return "CHANGE_PASSWORD";
        case 0:
          return "LOGOUT";
        case 9:
          return "EXIT";
        default:
          return NULL;
      }

    case 'A':
      switch (choice) {
        case 1:
          return "ADD_EMPLOYEE";
        case 2:
          return "MODIFY_CUSTOMER";
        case 3:
          return "MODIFY_EMPLOYEE";
        case 4:
          return "MANAGE_ROLES";
        case 5:
          return "CHANGE_PASSWORD";
        case 0:
          return "LOGOUT";
        case 9:
          return "EXIT";
        default:
          return NULL;
      }

    default:
      return NULL;
  }
}

static int read_amount(char* buffer, size_t size) {
  printf("Enter amount: ");

  if (scanf("%31s", buffer) != 1) return -1;

  if (strlen(buffer) >= size) return -1;

  return 0;
}

static int read_integer_choice(int* value);
static int send_request_receive(int sockfd, const char* message, char* response,
                                size_t response_size);

static int run_authenticated_session(int sockfd, char role,
                                     const char* first_name) {
  char request[REQUEST_SIZE];
  char response[RESPONSE_SIZE];
  printf("Welcome, %s!\n", first_name);

  while (1) {
    /*
     * 1. Request the role-specific menu from the server.
     */
    memset(request, 0, sizeof(request));
    memcpy(request, "MENU", 4);

    if (send_all(sockfd, request, sizeof(request)) < 0) {
      printf("Failed to send menu request.\n");
      break;
    }

    memset(response, 0, sizeof(response));

    if (recv_all(sockfd, response, sizeof(response)) < 0) {
      printf("Server disconnected.\n");
      break;
    }

    /*
     * 2. Display the menu received from the server.
     */
    printf("\n%s\n", response);

    /*
     * 3. Read the user's selection.
     */
    int choice;

    printf("Choose an option: ");

    int input_status = read_integer_choice(&choice);
    if (input_status == -1) {
      printf("\nInput closed. Ending session.\n");
      break;
    }
    if (input_status == 0) {
      printf("Invalid input. Please enter a numeric menu option.\n");
      continue;
    }

    /*
     * 4. Convert the selection into a server command.
     */
    const char* command = get_command(role, choice);

    if (command != NULL && strcmp(command, "EXIT") == 0) {
      memset(request, 0, sizeof(request));
      snprintf(request, sizeof(request), "LOGOUT");
      if (send_all(sockfd, request, sizeof(request)) == 0 &&
          recv_all(sockfd, response, sizeof(response)) == 0) {
        printf("\n%s\n", response);
      }
      return 1; /* Explicit Exit ends the application. */
    }

    if (command == NULL) {
      printf("Invalid option. Please try again.\n");
      continue;
    }

    memset(request, 0, sizeof(request));

    if (strcmp(command, "DEPOSIT") == 0 || strcmp(command, "WITHDRAW") == 0) {
      char amount[32];

      if (read_amount(amount, sizeof(amount)) != 0) {
        printf("Invalid amount input.\n");
        continue;
      }

      snprintf(request, sizeof(request), "%s|%s", command, amount);

    } else if (strcmp(command, "TRANSFER") == 0) {
      char destination[11];
      char amount[32];

      printf("Enter destination account ID (10 digits): ");

      if (scanf("%10s", destination) != 1 || strlen(destination) != 10) {
        printf("Invalid destination account ID.\n");
        continue;
      }

      if (read_amount(amount, sizeof(amount)) != 0) {
        printf("Invalid amount input.\n");
        continue;
      }

      snprintf(request, sizeof(request), "TRANSFER|%s|%s", destination, amount);

    } else if (strcmp(command, "CHANGE_PASSWORD") == 0) {
      char old_password[21];
      char new_password[21];

      printf("Enter current password: ");

      if (scanf("%20s", old_password) != 1) {
        printf("Invalid input.\n");
        continue;
      }

      printf("Enter new password (max 20 characters): ");

      if (scanf("%20s", new_password) != 1) {
        printf("Invalid input.\n");
        continue;
      }

      snprintf(request, sizeof(request), "CHANGE_PASSWORD|%s|%s", old_password,
               new_password);
    } else if (strcmp(command, "FEEDBACK") == 0) {
      char message[201];

      printf("Enter feedback (max 200 characters, one line): ");

      if (scanf(" %200[^\n]", message) != 1) {
        printf("Invalid input.\n");
        continue;
      }

      snprintf(request, sizeof(request), "FEEDBACK|%s", message);
    } else if (strcmp(command, "REVIEW_FEEDBACK") == 0) {
      snprintf(request, sizeof(request), "REVIEW_FEEDBACK");

      if (send_all(sockfd, request, sizeof(request)) < 0) {
        printf("Connection error.\n");
        break;
      }

      memset(response, 0, sizeof(response));

      if (recv_all(sockfd, response, sizeof(response)) < 0) {
        printf("Connection error.\n");
        break;
      }

      printf("\n%s\n", response);

      if (strncmp(response, "ERR|", 4) == 0 ||
          strstr(response, "no pending feedback") != NULL) {
        continue;
      }

      char feedback_id[11];

      printf("Enter feedback ID to mark reviewed (0 to cancel): ");

      if (scanf("%10s", feedback_id) != 1) {
        printf("Invalid input.\n");
        continue;
      }

      if (strcmp(feedback_id, "0") == 0) continue;

      if (strlen(feedback_id) != 10) {
        printf("Feedback ID must be 10 digits.\n");
        continue;
      }

      snprintf(request, sizeof(request), "REVIEW_FEEDBACK|%s", feedback_id);
    } else if (strcmp(command, "ADD_CUSTOMER") == 0 ||
               strcmp(command, "ADD_EMPLOYEE") == 0) {
      char first[21], last[21], new_username[21], new_password[21];
      printf("First name (no spaces, max 20): ");
      if (scanf("%20s", first) != 1) continue;
      printf("Last name (no spaces, max 20): ");
      if (scanf("%20s", last) != 1) continue;
      printf("Username (no spaces, max 20): ");
      if (scanf("%20s", new_username) != 1) continue;
      printf("Initial password (max 20): ");
      if (scanf("%20s", new_password) != 1) continue;
      snprintf(request, sizeof(request), "%s|%s|%s|%s|%s", command, first, last,
               new_username, new_password);
    } else if (strcmp(command, "MODIFY_CUSTOMER") == 0 ||
               strcmp(command, "MODIFY_EMPLOYEE") == 0) {
      char target_id[7], first[21], last[21], new_username[21];
      printf("User ID to modify (6 digits): ");
      if (scanf("%6s", target_id) != 1 || strlen(target_id) != 6) {
        printf("Invalid user ID.\n");
        continue;
      }
      printf("New first name (no spaces): ");
      if (scanf("%20s", first) != 1) continue;
      printf("New last name (no spaces): ");
      if (scanf("%20s", last) != 1) continue;
      printf("New username (no spaces): ");
      if (scanf("%20s", new_username) != 1) continue;
      snprintf(request, sizeof(request), "%s|%s|%s|%s|%s", command, target_id,
               first, last, new_username);
    } else if (strcmp(command, "ACCOUNT_STATUS") == 0) {
      char account_id[11], status;
      printf("Account ID (10 digits): ");
      if (scanf("%10s", account_id) != 1 || strlen(account_id) != 10) {
        printf("Invalid account ID.\n");
        continue;
      }
      printf("Set status A=active, D=deactivated: ");
      if (scanf(" %c", &status) != 1) continue;
      if (status >= 'a' && status <= 'z') status = (char)(status - 'a' + 'A');
      if (status != 'A' && status != 'D') {
        printf("Status must be A or D.\n");
        continue;
      }
      snprintf(request, sizeof(request), "ACCOUNT_STATUS|%s|%c", account_id,
               status);
    } else if (strcmp(command, "MANAGE_ROLES") == 0) {
      char target_id[7], new_role;
      printf("User ID (6 digits): ");
      if (scanf("%6s", target_id) != 1 || strlen(target_id) != 6) {
        printf("Invalid user ID.\n");
        continue;
      }
      printf("New role C=customer, E=employee, M=manager, A=admin: ");
      if (scanf(" %c", &new_role) != 1) continue;
      if (new_role >= 'a' && new_role <= 'z')
        new_role = (char)(new_role - 'a' + 'A');
      if (!strchr("CEMA", new_role)) {
        printf("Invalid role.\n");
        continue;
      }
      snprintf(request, sizeof(request), "MANAGE_ROLES|%s|%c", target_id,
               new_role);
    } else if (strcmp(command, "PASSBOOK") == 0) {
      char customer_id[7];
      printf("Customer user ID (6 digits): ");
      if (scanf("%6s", customer_id) != 1 || strlen(customer_id) != 6) {
        printf("Invalid user ID.\n");
        continue;
      }
      snprintf(request, sizeof(request), "PASSBOOK|%s", customer_id);
    } else if (strcmp(command, "APPLY_LOAN") == 0) {
      char amount[32];

      if (read_amount(amount, sizeof(amount)) != 0) {
        printf("Invalid amount.\n");
        continue;
      }

      snprintf(request, sizeof(request), "APPLY_LOAN|%s", amount);
    } else if (strcmp(command, "ASSIGN_LOAN") == 0) {
      char loan_id[11];
      char employee_id[7];

      printf("Enter loan ID (10 digits): ");
      if (scanf("%10s", loan_id) != 1 || strlen(loan_id) != 10) {
        printf("Invalid loan ID.\n");
        continue;
      }

      printf("Enter employee user ID (6 digits): ");
      if (scanf("%6s", employee_id) != 1 || strlen(employee_id) != 6) {
        printf("Invalid employee ID.\n");
        continue;
      }

      snprintf(request, sizeof(request), "ASSIGN_LOAN|%s|%s", loan_id,
               employee_id);
    } else if (strcmp(command, "PROCESS_LOAN") == 0) {
      char loan_id[11];
      char decision;
      char amount[32] = "0";

      printf("Enter loan ID (10 digits): ");
      if (scanf("%10s", loan_id) != 1 || strlen(loan_id) != 10) {
        printf("Invalid loan ID.\n");
        continue;
      }

      printf("Approve or reject? (A/R): ");
      if (scanf(" %c", &decision) != 1) {
        printf("Invalid decision.\n");
        continue;
      }

      if (decision >= 'a' && decision <= 'z')
        decision = (char)(decision - 'a' + 'A');

      if (decision != 'A' && decision != 'R') {
        printf("Enter A to approve or R to reject.\n");
        continue;
      }

      if (decision == 'A') {
        printf("Enter approved amount: ");
        if (read_amount(amount, sizeof(amount)) != 0) {
          printf("Invalid amount.\n");
          continue;
        }
      }

      snprintf(request, sizeof(request), "PROCESS_LOAN|%s|%c|%s", loan_id,
               decision, amount);
    } else {
      snprintf(request, sizeof(request), "%s", command);
    }

    /* Transaction history and employee passbook are browsed five records at a
     * time. */
    if (strcmp(command, "HISTORY") == 0 || strcmp(command, "PASSBOOK") == 0) {
      int is_passbook = strcmp(command, "PASSBOOK") == 0;
      char customer_id[7] = {0};
      if (is_passbook) {
        if (sscanf(request, "PASSBOOK|%6s", customer_id) != 1 ||
            strlen(customer_id) != 6) {
          printf("Invalid customer ID.\n");
          continue;
        }
      }
      int page = 0;
      int page_empty = 0;
      for (;;) {
        char page_request[64];
        if (is_passbook)
          snprintf(page_request, sizeof(page_request), "PASSBOOK|%s|%d",
                   customer_id, page);
        else
          snprintf(page_request, sizeof(page_request), "HISTORY|%d", page);
        if (send_request_receive(sockfd, page_request, response,
                                 sizeof(response)) < 0) {
          printf("Connection error while reading transaction history.\n");
          return 0;
        }
        printf("\n%s\n", response);
        page_empty =
            (strncmp(response, "ERR|", 4) == 0 ||
             strstr(response, "No more completed transactions") != NULL ||
             strstr(response, "No completed transactions found") != NULL);
        printf("\nN = Next page | P = Previous page | 0 = Back\nChoose: ");
        char nav = '\0';
        if (scanf(" %c", &nav) != 1) return 0;
        if (nav >= 'a' && nav <= 'z') nav = (char)(nav - 'a' + 'A');
        if (nav == '0') break;
        if (nav == 'N') {
          if (page_empty)
            printf("There are no more transactions to display.\n");
          else
            page++;
          continue;
        }
        if (nav == 'P') {
          if (page > 0)
            page--;
          else
            printf("You are already on the first page.\n");
          continue;
        }
        printf("Invalid selection. Enter N, P, or 0.\n");
      }
      continue;
    }

    if (send_all(sockfd, request, sizeof(request)) < 0) {
      printf("Failed to send command.\n");
      break;
    }

    /*
     * 6. Receive and display the server's response.
     */
    memset(response, 0, sizeof(response));

    if (recv_all(sockfd, response, sizeof(response)) < 0) {
      printf("Server disconnected.\n");
      break;
    }

    printf("\n%s\n", response);

    /*
     * 7. End the client session after logout.
     */
    if (strcmp(command, "LOGOUT") == 0) {
      break;
    }
  }

  return 0; /* Logout or disconnection returns to the role-selection screen. */
}

static int send_request_receive(int sockfd, const char* message, char* response,
                                size_t response_size) {
  char request[REQUEST_SIZE];
  memset(request, 0, sizeof(request));
  if (snprintf(request, sizeof(request), "%s", message) >= (int)sizeof(request))
    return -1;
  if (send_all(sockfd, request, sizeof(request)) < 0) return -1;
  memset(response, 0, response_size);
  return recv_all(sockfd, response, response_size);
}

/* Read one integer choice and discard the rest of that input line.
 * Returns 1 for an integer, 0 for invalid text, and -1 on EOF. */
static int read_integer_choice(int* value) {
  int result = scanf("%d", value);
  int ch;

  if (result == EOF) return -1;
  if (result != 1) {
    while ((ch = getchar()) != '\n' && ch != EOF) {
    }
    return ch == EOF ? -1 : 0;
  }

  while ((ch = getchar()) != '\n' && ch != EOF) {
  }
  return 1;
}

int main(void) {
  for (;;) {
    int role_choice;
    printf(
        "\n========== BANKING MANAGEMENT SYSTEM ==========\n"
        "1. Customer\n2. Employee\n3. Manager\n4. Administrator\n5. Exit\n"
        "Choose role: ");
    int input_status = read_integer_choice(&role_choice);
    if (input_status == -1) {
      printf("\nInput closed. Exiting application. Goodbye!\n");
      return 0;
    }
    if (input_status == 0) {
      printf("Invalid input. Please enter a number from 1 to 5.\n");
      continue;
    }
    if (role_choice == 5) {
      printf("Exiting application. Goodbye!\n");
      return 0;
    }
    if (role_choice < 1 || role_choice > 4) {
      printf("Invalid role selection.\n");
      continue;
    }

    int customer = (role_choice == 1);
    int customer_choice = 1;
    if (customer) {
      printf(
          "\n========== CUSTOMER ACCESS ==========\n1. Login\n2. Create New "
          "Account\n0. Back\n9. Exit Application\nChoose an option: ");
      input_status = read_integer_choice(&customer_choice);
      if (input_status == -1) {
        printf("\nInput closed. Exiting application. Goodbye!\n");
        return 0;
      }
      if (input_status == 0) {
        printf("Invalid input. Please enter a valid option.\n");
        continue;
      }
      if (customer_choice == 9) {
        printf("Exiting application. Goodbye!\n");
        return 0;
      }
      if (customer_choice == 0) continue;
      if (customer_choice != 1 && customer_choice != 2) {
        printf("Invalid option.\n");
        continue;
      }
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
      perror("socket");
      return 1;
    }
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) != 1 ||
        connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) <
            0) {
      perror("connect");
      close(sockfd);
      printf("Unable to connect to server.\n");
      continue;
    }

    if (customer && customer_choice == 2) {
      char first[21], last[21], username[21], password[21];
      printf("First name (no spaces, max 20): ");
      if (scanf("%20s", first) != 1) {
        close(sockfd);
        return 1;
      }
      printf("Last name (no spaces, max 20): ");
      if (scanf("%20s", last) != 1) {
        close(sockfd);
        return 1;
      }
      printf("Choose username (no spaces, max 20): ");
      if (scanf("%20s", username) != 1) {
        close(sockfd);
        return 1;
      }
      printf("Choose password (max 20 characters): ");
      if (scanf("%20s", password) != 1) {
        close(sockfd);
        return 1;
      }
      char message[REQUEST_SIZE], response[RESPONSE_SIZE];
      int n =
          snprintf(message, sizeof(message), "REGISTER_CUSTOMER|%s|%s|%s|%s",
                   first, last, username, password);
      if (n < 0 || n >= (int)sizeof(message) ||
          send_request_receive(sockfd, message, response, sizeof(response)) <
              0) {
        printf("Registration communication failed.\n");
        close(sockfd);
        continue;
      }
      printf("\n%s\n", response);
      printf("You can now log in from the Customer menu.\n");
      close(sockfd);
      continue;
    }

    char username[USERNAME_SIZE + 1], password[PASSWORD_SIZE + 1];
    printf("Username: ");
    if (scanf("%20s", username) != 1) {
      close(sockfd);
      return 1;
    }
    printf("Password: ");
    if (scanf("%20s", password) != 1) {
      close(sockfd);
      return 1;
    }
    char message[REQUEST_SIZE], response[RESPONSE_SIZE];
    int n =
        snprintf(message, sizeof(message), "LOGIN|%s|%s", username, password);
    if (n < 0 || n >= (int)sizeof(message) ||
        send_request_receive(sockfd, message, response, sizeof(response)) < 0) {
      printf("Login communication failed.\n");
      close(sockfd);
      continue;
    }
    printf("\n%s\n", response);
    if (strncmp(response, "OK|LOGIN|", 9) != 0) {
      close(sockfd);
      continue;
    }

    char user_id[7], first_name[21], actual_role = '\0';
    if (sscanf(response, "OK|LOGIN|%6[^|]|%c|%20[^\n]", user_id, &actual_role,
               first_name) != 3) {
      printf("Invalid login response.\n");
      close(sockfd);
      continue;
    }
    char expected_role = role_choice == 1   ? 'C'
                         : role_choice == 2 ? 'E'
                         : role_choice == 3 ? 'M'
                                            : 'A';
    if (actual_role != expected_role) {
      printf(
          "This account does not belong to the selected role. Please choose "
          "the correct role.\n");
      (void)send_request_receive(sockfd, "LOGOUT", response, sizeof(response));
      close(sockfd);
      continue;
    }

    int should_exit =
        run_authenticated_session(sockfd, actual_role, first_name);
    close(sockfd);
    if (should_exit) {
      printf("Exiting application. Goodbye!\n");
      return 0;
    }
    /* Logout returns to the four-role landing menu; application stays open. */
  }
}
