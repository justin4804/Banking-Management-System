#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "auth.h"
#include "storage.h"

#define PORT 5555
#define REQUEST_SIZE 256
#define RESPONSE_SIZE 512
#define ACCOUNT_RECORD_SIZE 53

/*
 * Read exactly count bytes.
 * Returns 0 on success and -1 on failure or premature disconnect.
 */
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

/*
 * Send exactly count bytes.
 */
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

static void make_role_menu(char role, char* response, size_t size) {
  switch (role) {
    case 'C':
      snprintf(response, size,
               "========== CUSTOMER DASHBOARD ==========\n"
               "1. Check Balance\n"
               "2. Deposit\n"
               "3. Withdraw\n"
               "4. Transfer\n"
               "5. Apply for Loan\n"
               "6. Transaction History\n"
               "7. Change Password\n"
               "8. Submit Feedback\n"
               "0. Logout\n"
               "9. Exit Application");
      break;

    case 'E':
      snprintf(response, size,
               "========== EMPLOYEE DASHBOARD ==========\n"
               "1. Add Customer\n"
               "2. Modify Customer\n"
               "3. View Assigned Loans\n"
               "4. Process Loan Application\n"
               "5. View Customer Passbook\n"
               "6. Change Password\n"
               "0. Logout\n"
               "9. Exit Application");
      break;

    case 'M':
      snprintf(response, size,
               "========== MANAGER DASHBOARD ==========\n"
               "1. Activate/Deactivate Account\n"
               "2. Assign Loan to Employee\n"
               "3. Review Feedback\n"
               "4. Change Password\n"
               "0. Logout\n"
               "9. Exit Application");
      break;

    case 'A':
      snprintf(response, size,
               "========== ADMINISTRATOR DASHBOARD ==========\n"
               "1. Add Employee\n"
               "2. Modify Customer Details\n"
               "3. Modify Employee Details\n"
               "4. Manage Roles\n"
               "5. Change Password\n"
               "0. Logout\n"
               "9. Exit Application");
      break;

    default:
      snprintf(response, size, "ERR|Invalid role");
  }
}

static int find_customer_account(int fd, const char* user_id,
                                 char account_id[ACCOUNT_ID_SIZE + 1]) {
  char record[ACCOUNT_RECORD_SIZE];
  off_t offset = 0;

  while (1) {
    size_t total = 0;

    while (total < sizeof(record)) {
      ssize_t n = pread(fd, record + total, sizeof(record) - total,
                        offset + (off_t)total);

      if (n == 0) {
        if (total == 0) {
          return -1; /* End of file */
        }
        return -1; /* Incomplete record */
      }

      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return -1;
      }

      total += (size_t)n;
    }

    if (record[52] != '\n') {
      return -1;
    }

    /* Owner user ID occupies bytes 10–15. */
    if (memcmp(record + 10, user_id, USER_ID_SIZE) == 0 && record[31] == 'A') {
      memcpy(account_id, record, ACCOUNT_ID_SIZE);
      account_id[ACCOUNT_ID_SIZE] = '\0';

      return 0;
    }

    offset += ACCOUNT_RECORD_SIZE;
  }
}

#define TRANSACTION_RECORD_SIZE 68

#define DATA_DIR "../data"
#define ACCOUNTS_FILE DATA_DIR "/accounts.txt"
#define TRANSACTIONS_FILE DATA_DIR "/transactions.txt"
#define TRANSACTIONS_LOCK_FILE DATA_DIR "/transactions.lock"

static int set_record_lock(int fd, off_t offset, short lock_type) {
  struct flock lock;

  memset(&lock, 0, sizeof(lock));
  lock.l_type = lock_type;
  lock.l_whence = SEEK_SET;
  lock.l_start = offset;
  lock.l_len = ACCOUNT_RECORD_SIZE;

  int command = (lock_type == F_UNLCK) ? F_SETLK : F_SETLKW;

  while (fcntl(fd, command, &lock) < 0) {
    if (errno == EINTR) continue;
    return -1;
  }

  return 0;
}

/* Lock an arbitrary range in a file. */
static int set_range_lock(int fd, off_t offset, off_t length, short lock_type) {
  struct flock lock;
  memset(&lock, 0, sizeof(lock));

  lock.l_type = lock_type;
  lock.l_whence = SEEK_SET;
  lock.l_start = offset;
  lock.l_len = length;

  int command = (lock_type == F_UNLCK) ? F_SETLK : F_SETLKW;

  while (fcntl(fd, command, &lock) == -1) {
    if (errno == EINTR) continue;
    return -1;
  }

  return 0;
}

/* Read a complete fixed-size record from a file. */
static int read_record_at(int fd, off_t offset, char* record, size_t size) {
  size_t total = 0;

  while (total < size) {
    ssize_t n = pread(fd, record + total, size - total, offset + (off_t)total);

    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }

    if (n == 0) return -1;

    total += (size_t)n;
  }

  return 0;
}

/* Get a timestamp in the existing fixed-width format. */
static void make_timestamp(char output[21]) {
  time_t now = time(NULL);
  struct tm tm_value;

  if (localtime_r(&now, &tm_value) == NULL) {
    memset(output, ' ', 20);
    output[20] = '\0';
    return;
  }

  char temp[32];
  strftime(temp, sizeof(temp), "%Y-%m-%d %H:%M:%S", &tm_value);

  snprintf(output, 21, "%-20.20s", temp);
}

static int read_account_record(int fd, off_t offset,
                               char record[ACCOUNT_RECORD_SIZE]) {
  size_t total = 0;

  while (total < ACCOUNT_RECORD_SIZE) {
    ssize_t n = pread(fd, record + total, ACCOUNT_RECORD_SIZE - total,
                      offset + (off_t)total);

    if (n < 0 && errno == EINTR) continue;

    if (n <= 0) return -1;

    total += (size_t)n;
  }

  if (record[52] != '\n' || record[31] != 'A') return -1;

  return 0;
}

static int parse_amount(const char* text, double* amount) {
  char* end;
  double value;

  if (text == NULL || *text == '\0') return -1;

  errno = 0;
  value = strtod(text, &end);

  if (errno != 0 || end == text || *end != '\0' || !isfinite(value) ||
      value <= 0.0)
    return -1;

  /* Reject amounts with more than two decimal places. */
  double cents = value * 100.0;

  if (!isfinite(cents) || fabs(cents - round(cents)) > 0.000001) return -1;

  /* The account balance field is 15 characters wide. */
  if (value > 999999999999.99) return -1;

  *amount = value;
  return 0;
}

/*
 * Append one completed transaction.
 *
 * Transaction layout:
 * ID(10), type(1), source(10), destination(10),
 * amount(15), timestamp(20), status(1), newline(1).
 *
 * The separate lock file serializes transaction ID generation
 * and appending across server child processes.
 */
static int append_transaction(char type, const char* source,
                              const char* destination, double amount) {
  int lock_fd = -1;
  int tx_fd = -1;
  int result = -1;
  struct flock lock;

  lock_fd = open(TRANSACTIONS_LOCK_FILE, O_RDWR | O_CREAT, 0600);
  if (lock_fd < 0) goto cleanup;

  memset(&lock, 0, sizeof(lock));
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0;

  while (fcntl(lock_fd, F_SETLKW, &lock) < 0) {
    if (errno == EINTR) continue;
    goto cleanup;
  }

  tx_fd = open(TRANSACTIONS_FILE, O_RDWR | O_CREAT, 0600);
  if (tx_fd < 0) goto cleanup;

  off_t file_size = lseek(tx_fd, 0, SEEK_END);
  if (file_size < 0 || file_size % TRANSACTION_RECORD_SIZE != 0) goto cleanup;

  unsigned long long max_id = 0;
  char old_record[TRANSACTION_RECORD_SIZE];

  for (off_t pos = 0; pos < file_size; pos += TRANSACTION_RECORD_SIZE) {
    size_t total = 0;

    while (total < sizeof(old_record)) {
      ssize_t n = pread(tx_fd, old_record + total, sizeof(old_record) - total,
                        pos + (off_t)total);

      if (n < 0 && errno == EINTR) continue;

      if (n <= 0) goto cleanup;

      total += (size_t)n;
    }

    if (old_record[67] != '\n') goto cleanup;

    char id_text[11];
    memcpy(id_text, old_record, 10);
    id_text[10] = '\0';

    char* end;
    errno = 0;
    unsigned long long id = strtoull(id_text, &end, 10);

    if (errno != 0 || end == id_text || *end != '\0') goto cleanup;

    if (id > max_id) max_id = id;
  }

  if (max_id >= 9999999999ULL) goto cleanup;

  char record[TRANSACTION_RECORD_SIZE];
  char id_field[11];
  char amount_field[16];
  char timestamp[20] = {0};
  time_t now = time(NULL);
  struct tm tm_now;

  if (localtime_r(&now, &tm_now) == NULL) goto cleanup;

  if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now) == 0)
    goto cleanup;

  if (snprintf(id_field, sizeof(id_field), "%010llu", max_id + 1) != 10)
    goto cleanup;

  if (snprintf(amount_field, sizeof(amount_field), "%015.2f", amount) != 15)
    goto cleanup;

  memset(record, ' ', sizeof(record));

  memcpy(record, id_field, 10);
  record[10] = type;
  memcpy(record + 11, source, 10);
  memcpy(record + 21, destination, 10);
  memcpy(record + 31, amount_field, 15);
  memcpy(record + 46, timestamp, strlen(timestamp));
  record[66] = 'C';
  record[67] = '\n';

  size_t total = 0;

  while (total < sizeof(record)) {
    ssize_t n = pwrite(tx_fd, record + total, sizeof(record) - total,
                       file_size + (off_t)total);

    if (n < 0 && errno == EINTR) continue;

    if (n <= 0) goto cleanup;

    total += (size_t)n;
  }

  if (fsync(tx_fd) < 0) goto cleanup;

  result = 0;

cleanup:
  if (tx_fd >= 0) close(tx_fd);

  if (lock_fd >= 0) {
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    fcntl(lock_fd, F_SETLK, &lock);
    close(lock_fd);
  }

  return result;
}

static int write_balance(int fd, off_t offset, double balance) {
  char field[16];

  if (!isfinite(balance) || balance < 0.0 || balance > 999999999999.99)
    return -1;

  if (snprintf(field, sizeof(field), "%015.2f", balance) != 15) return -1;

  size_t total = 0;

  while (total < 15) {
    ssize_t n =
        pwrite(fd, field + total, 15 - total, offset + 16 + (off_t)total);

    if (n < 0 && errno == EINTR) continue;

    if (n <= 0) return -1;

    total += (size_t)n;
  }

  return fsync(fd);
}

static int get_customer_account(int fd, const char* user_id,
                                char account_id[11], off_t* offset) {
  if (find_customer_account(fd, user_id, account_id) != 0) return -1;

  return find_account(fd, account_id, offset);
}

static int handle_deposit_or_withdraw(const char* user_id, char type,
                                      const char* amount_text, char* response,
                                      size_t response_size) {
  double amount;

  if (parse_amount(amount_text, &amount) != 0) {
    snprintf(response, response_size, "ERR|Invalid amount");
    return -1;
  }

  int fd = open(ACCOUNTS_FILE, O_RDWR);

  if (fd < 0) {
    snprintf(response, response_size, "ERR|Unable to open account database");
    return -1;
  }

  char account_id[11];
  off_t offset;
  char record[ACCOUNT_RECORD_SIZE];
  double old_balance;

  if (get_customer_account(fd, user_id, account_id, &offset) != 0) {
    snprintf(response, response_size, "ERR|No active account found");
    close(fd);
    return -1;
  }

  if (set_record_lock(fd, offset, F_WRLCK) != 0) {
    snprintf(response, response_size, "ERR|Unable to lock account");
    close(fd);
    return -1;
  }

  int result = -1;

  /* Always reread after acquiring the lock. */
  if (read_account_record(fd, offset, record) != 0) {
    snprintf(response, response_size, "ERR|Account is inactive or invalid");
    goto done;
  }

  if (memcmp(record + 10, user_id, USER_ID_SIZE) != 0) {
    snprintf(response, response_size, "ERR|Account ownership mismatch");
    goto done;
  }

  if (get_balance(fd, offset, &old_balance) != 0) {
    snprintf(response, response_size, "ERR|Unable to read balance");
    goto done;
  }

  double new_balance;

  if (type == 'D') {
    new_balance = old_balance + amount;

    if (!isfinite(new_balance) || new_balance > 999999999999.99) {
      snprintf(response, response_size, "ERR|Balance limit exceeded");
      goto done;
    }
  } else {
    if (amount > old_balance) {
      snprintf(response, response_size, "ERR|Insufficient balance");
      goto done;
    }

    new_balance = old_balance - amount;
  }

  if (write_balance(fd, offset, new_balance) != 0) {
    snprintf(response, response_size, "ERR|Unable to update balance");
    goto done;
  }

  const char zero_id[] = "0000000000";
  const char* source = (type == 'D') ? zero_id : account_id;
  const char* destination = (type == 'D') ? account_id : zero_id;

  if (append_transaction(type, source, destination, amount) != 0) {
    /*
     * Best-effort rollback if transaction logging fails.
     * A server crash during this sequence still requires recovery
     * support, which we will document in the project.
     */
    if (write_balance(fd, offset, old_balance) == 0) {
      snprintf(response, response_size,
               "ERR|Transaction logging failed; balance rolled back");
    } else {
      snprintf(response, response_size,
               "ERR|Critical error: verify account balance");
    }

    goto done;
  }

  snprintf(response, response_size,
           "OK|%s successful|Account: %s|Balance: %.2f",
           type == 'D' ? "Deposit" : "Withdrawal", account_id, new_balance);

  result = 0;

done:
  set_record_lock(fd, offset, F_UNLCK);
  close(fd);
  return result;
}

static int handle_transfer(const char* user_id, const char* destination_id,
                           const char* amount_text, char* response,
                           size_t response_size) {
  double amount;

  if (strlen(destination_id) != 10) {
    snprintf(response, response_size, "ERR|Invalid destination account ID");
    return -1;
  }

  for (int i = 0; i < 10; i++) {
    if (!isdigit((unsigned char)destination_id[i])) {
      snprintf(response, response_size,
               "ERR|Account ID must contain digits only");
      return -1;
    }
  }

  if (parse_amount(amount_text, &amount) != 0) {
    snprintf(response, response_size, "ERR|Invalid amount");
    return -1;
  }

  int fd = open(ACCOUNTS_FILE, O_RDWR);

  if (fd < 0) {
    snprintf(response, response_size, "ERR|Unable to open account database");
    return -1;
  }

  char source_id[11];
  off_t source_offset, destination_offset;
  char source_record[ACCOUNT_RECORD_SIZE];
  char destination_record[ACCOUNT_RECORD_SIZE];
  double source_balance, destination_balance;

  if (get_customer_account(fd, user_id, source_id, &source_offset) != 0) {
    snprintf(response, response_size, "ERR|No active source account found");
    close(fd);
    return -1;
  }

  if (strcmp(source_id, destination_id) == 0) {
    snprintf(response, response_size,
             "ERR|Source and destination accounts must differ");
    close(fd);
    return -1;
  }

  if (find_account(fd, destination_id, &destination_offset) != 0) {
    snprintf(response, response_size, "ERR|Destination account not found");
    close(fd);
    return -1;
  }

  /*
   * Lock in consistent offset order to avoid transfer deadlocks.
   */
  off_t first =
      source_offset < destination_offset ? source_offset : destination_offset;
  off_t second =
      source_offset < destination_offset ? destination_offset : source_offset;

  if (set_record_lock(fd, first, F_WRLCK) != 0) {
    snprintf(response, response_size, "ERR|Unable to lock accounts");
    close(fd);
    return -1;
  }

  if (set_record_lock(fd, second, F_WRLCK) != 0) {
    set_record_lock(fd, first, F_UNLCK);
    snprintf(response, response_size, "ERR|Unable to lock accounts");
    close(fd);
    return -1;
  }

  int result = -1;

  if (read_account_record(fd, source_offset, source_record) != 0 ||
      read_account_record(fd, destination_offset, destination_record) != 0) {
    snprintf(response, response_size,
             "ERR|Source or destination account is inactive");
    goto transfer_done;
  }

  if (memcmp(source_record + 10, user_id, USER_ID_SIZE) != 0) {
    snprintf(response, response_size, "ERR|Source account ownership mismatch");
    goto transfer_done;
  }

  if (get_balance(fd, source_offset, &source_balance) != 0 ||
      get_balance(fd, destination_offset, &destination_balance) != 0) {
    snprintf(response, response_size, "ERR|Unable to read account balances");
    goto transfer_done;
  }

  if (amount > source_balance) {
    snprintf(response, response_size, "ERR|Insufficient balance");
    goto transfer_done;
  }

  if (!isfinite(destination_balance + amount) ||
      destination_balance + amount > 999999999999.99) {
    snprintf(response, response_size, "ERR|Destination balance limit exceeded");
    goto transfer_done;
  }

  double new_source = source_balance - amount;
  double new_destination = destination_balance + amount;

  if (write_balance(fd, source_offset, new_source) != 0) {
    snprintf(response, response_size, "ERR|Unable to update source balance");
    goto transfer_done;
  }

  if (write_balance(fd, destination_offset, new_destination) != 0) {
    if (write_balance(fd, source_offset, source_balance) != 0) {
      snprintf(response, response_size,
               "ERR|Critical error: verify account balances");
    } else {
      snprintf(response, response_size,
               "ERR|Destination update failed; source balance restored");
    }
    goto transfer_done;
  }

  if (append_transaction('T', source_id, destination_id, amount) != 0) {
    int source_restored = write_balance(fd, source_offset, source_balance) == 0;
    int destination_restored =
        write_balance(fd, destination_offset, destination_balance) == 0;

    if (source_restored && destination_restored) {
      snprintf(response, response_size,
               "ERR|Transaction logging failed; balances restored");
    } else {
      snprintf(response, response_size,
               "ERR|Critical error: verify account balances");
    }

    goto transfer_done;
  }

  snprintf(response, response_size,
           "OK|Transfer successful|From: %s|To: %s|Amount: %.2f", source_id,
           destination_id, amount);

  result = 0;

transfer_done:
  set_record_lock(fd, second, F_UNLCK);
  set_record_lock(fd, first, F_UNLCK);
  close(fd);
  return result;
}

static void handle_history(const char* user_id, int page, char* response,
                           size_t response_size) {
  int account_fd = open(ACCOUNTS_FILE, O_RDONLY);

  if (account_fd < 0) {
    snprintf(response, response_size, "ERR|Unable to open account database");
    return;
  }

  char account_id[11];
  off_t account_offset;

  if (get_customer_account(account_fd, user_id, account_id, &account_offset) !=
      0) {
    snprintf(response, response_size, "ERR|No active account found");
    close(account_fd);
    return;
  }

  close(account_fd);

  int fd = open(TRANSACTIONS_FILE, O_RDONLY);

  if (fd < 0) {
    if (errno == ENOENT) {
      snprintf(response, response_size, "OK|No transactions found");
    } else {
      snprintf(response, response_size,
               "ERR|Unable to open transaction history");
    }
    return;
  }

  off_t file_size = lseek(fd, 0, SEEK_END);

  if (file_size < 0 || file_size % TRANSACTION_RECORD_SIZE != 0) {
    snprintf(response, response_size,
             "ERR|Transaction database has an invalid format");
    close(fd);
    return;
  }

  /* Each request returns one page of five completed transactions, newest first.
   */
  char matches[5][TRANSACTION_RECORD_SIZE];
  int count = 0;
  int skipped = 0;
  int wanted_skip = page * 5;

  for (off_t pos = file_size - TRANSACTION_RECORD_SIZE; pos >= 0 && count < 5;
       pos -= TRANSACTION_RECORD_SIZE) {
    char record[TRANSACTION_RECORD_SIZE];
    size_t total = 0;

    while (total < sizeof(record)) {
      ssize_t n =
          pread(fd, record + total, sizeof(record) - total, pos + (off_t)total);

      if (n < 0 && errno == EINTR) continue;

      if (n <= 0) {
        snprintf(response, response_size,
                 "ERR|Unable to read transaction history");
        close(fd);
        return;
      }

      total += (size_t)n;
    }

    if (record[67] != '\n') continue;

    /*
     * Deposit: destination is the customer's account.
     * Withdrawal: source is the customer's account.
     * Transfer: either source or destination matches.
     */
    int matches_account = memcmp(record + 11, account_id, 10) == 0 ||
                          memcmp(record + 21, account_id, 10) == 0;

    if (matches_account && record[66] == 'C') {
      if (skipped < wanted_skip) {
        skipped++;
        continue;
      }
      memcpy(matches[count], record, sizeof(record));
      count++;
    }
  }

  close(fd);

  if (count == 0) {
    snprintf(response, response_size,
             "OK|No more completed transactions on page %d for account %s",
             page + 1, account_id);
    return;
  }

  int used =
      snprintf(response, response_size,
               "OK|Transaction history - Page %d (newest first), account %s:\n",
               page + 1, account_id);

  for (int i = 0; i < count && used > 0 && (size_t)used < response_size; i++) {
    char* r = matches[i];

    char id[11], source[11], destination[11];
    char amount[16], timestamp[21];

    memcpy(id, r, 10);
    id[10] = '\0';

    memcpy(source, r + 11, 10);
    source[10] = '\0';

    memcpy(destination, r + 21, 10);
    destination[10] = '\0';

    memcpy(amount, r + 31, 15);
    amount[15] = '\0';

    memcpy(timestamp, r + 46, 20);
    timestamp[20] = '\0';

    /* Remove padding from the fixed-width timestamp. */
    for (int j = 19; j >= 0 && timestamp[j] == ' '; j--) timestamp[j] = '\0';

    const char* type_name;

    switch (r[10]) {
      case 'D':
        type_name = "DEPOSIT";
        break;

      case 'W':
        type_name = "WITHDRAWAL";
        break;

      case 'T':
        type_name = "TRANSFER";
        break;

      case 'L':
        type_name = "LOAN DISBURSEMENT";
        break;

      default:
        type_name = "UNKNOWN";
        break;
    }

    int n = snprintf(response + used, response_size - (size_t)used,
                     "ID:%s Type:%s Amount:%s Date:%s\n", id, type_name, amount,
                     timestamp);

    if (n < 0) break;

    if ((size_t)n >= response_size - (size_t)used) break;

    used += n;
  }
}

#define USER_RECORD_SIZE 89

static void handle_change_password(const char* user_id, const char* payload,
                                   char response[RESPONSE_SIZE]) {
  char old_password[21] = {0};
  char new_password[21] = {0};

  /*
   * Expected payload:
   * CHANGE_PASSWORD|old_password|new_password
   */
  if (sscanf(payload, "%20[^|]|%20s", old_password, new_password) != 2) {
    snprintf(response, RESPONSE_SIZE, "ERR|Invalid password-change request.");
    return;
  }

  size_t new_len = strlen(new_password);

  if (new_len == 0 || new_len > 20 || strchr(new_password, '|') != NULL ||
      strchr(new_password, '\n') != NULL ||
      strchr(new_password, '\r') != NULL) {
    snprintf(response, RESPONSE_SIZE,
             "ERR|New password must contain 1-20 characters.");
    return;
  }

  int fd = open("../data/users.txt", O_RDWR);

  if (fd == -1) {
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open users file.");
    return;
  }

  struct stat st;

  if (fstat(fd, &st) == -1) {
    close(fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot inspect users file.");
    return;
  }

  off_t offset;
  int found = 0;

  for (offset = 0; offset + USER_RECORD_SIZE <= st.st_size;
       offset += USER_RECORD_SIZE) {
    char record[USER_RECORD_SIZE];

    if (read_record_at(fd, offset, record, sizeof(record)) == -1) continue;

    if (record[88] != '\n') continue;

    if (memcmp(record, user_id, 6) == 0) {
      found = 1;
      break;
    }
  }

  if (!found) {
    close(fd);
    snprintf(response, RESPONSE_SIZE, "ERR|User record not found.");
    return;
  }

  /*
   * Lock only the matching user's record.
   * Read and validate the password again after locking.
   */
  if (set_range_lock(fd, offset, USER_RECORD_SIZE, F_WRLCK) == -1) {
    close(fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Could not lock user record.");
    return;
  }

  char record[USER_RECORD_SIZE];

  if (read_record_at(fd, offset, record, sizeof(record)) == -1 ||
      record[88] != '\n') {
    set_range_lock(fd, offset, USER_RECORD_SIZE, F_UNLCK);
    close(fd);

    snprintf(response, RESPONSE_SIZE, "ERR|Could not read user record.");
    return;
  }

  if (memcmp(record, user_id, 6) != 0 || record[87] != 'A') {
    set_range_lock(fd, offset, USER_RECORD_SIZE, F_UNLCK);
    close(fd);

    snprintf(response, RESPONSE_SIZE, "ERR|User is inactive or invalid.");
    return;
  }

  char stored_password[21] = {0};
  memcpy(stored_password, record + 67, 20);

  /* Remove fixed-width padding from the stored password. */
  for (int i = 19; i >= 0 && stored_password[i] == ' '; i--)
    stored_password[i] = '\0';

  if (strcmp(stored_password, old_password) != 0) {
    set_range_lock(fd, offset, USER_RECORD_SIZE, F_UNLCK);
    close(fd);

    snprintf(response, RESPONSE_SIZE, "ERR|Current password is incorrect.");
    return;
  }

  memset(record + 67, ' ', 20);
  memcpy(record + 67, new_password, new_len);

  ssize_t written = pwrite(fd, record + 67, 20, offset + 67);

  int success = (written == 20 && fsync(fd) == 0);

  set_range_lock(fd, offset, USER_RECORD_SIZE, F_UNLCK);
  close(fd);

  if (!success) {
    snprintf(response, RESPONSE_SIZE, "ERR|Could not save new password.");
    return;
  }

  snprintf(response, RESPONSE_SIZE, "OK|Password changed successfully.");
}

#define FEEDBACK_RECORD_SIZE 238

static void handle_submit_feedback(const char* user_id, const char* message,
                                   char response[RESPONSE_SIZE]) {
  size_t length = strlen(message);

  if (length == 0 || length > 200) {
    snprintf(response, RESPONSE_SIZE,
             "ERR|Feedback must contain 1-200 characters.");
    return;
  }

  for (size_t i = 0; i < length; i++) {
    if (message[i] == '\n' || message[i] == '\r') {
      snprintf(response, RESPONSE_SIZE, "ERR|Feedback must be a single line.");
      return;
    }
  }

  int lock_fd = open("../data/feedback.lock", O_RDWR | O_CREAT, 0666);

  if (lock_fd == -1) {
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open feedback lock.");
    return;
  }

  if (set_range_lock(lock_fd, 0, 0, F_WRLCK) == -1) {
    close(lock_fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot lock feedback file.");
    return;
  }

  int fd = open("../data/feedback.txt", O_RDWR | O_CREAT, 0666);

  if (fd == -1) {
    set_range_lock(lock_fd, 0, 0, F_UNLCK);
    close(lock_fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open feedback file.");
    return;
  }

  struct stat st;
  int success = 0;
  unsigned long long max_id = 0;

  if (fstat(fd, &st) == -1 || st.st_size % FEEDBACK_RECORD_SIZE != 0) {
    goto feedback_cleanup;
  }

  for (off_t offset = 0; offset < st.st_size; offset += FEEDBACK_RECORD_SIZE) {
    char record[FEEDBACK_RECORD_SIZE];

    if (read_record_at(fd, offset, record, sizeof(record)) == -1 ||
        record[237] != '\n') {
      goto feedback_cleanup;
    }

    char id_text[11] = {0};
    memcpy(id_text, record, 10);

    unsigned long long id = strtoull(id_text, NULL, 10);

    if (id > max_id) max_id = id;
  }

  if (max_id >= 9999999999ULL) goto feedback_cleanup;

  char record[FEEDBACK_RECORD_SIZE];
  memset(record, ' ', sizeof(record));

  char id_text[11];
  snprintf(id_text, sizeof(id_text), "%010llu", max_id + 1);
  memcpy(record, id_text, 10);

  memcpy(record + 10, user_id, 6);
  memcpy(record + 16, message, length);
  record[216] = 'N';

  char timestamp[21];
  make_timestamp(timestamp);
  memcpy(record + 217, timestamp, 20);

  record[237] = '\n';

  if (pwrite(fd, record, sizeof(record), st.st_size) ==
          (ssize_t)sizeof(record) &&
      fsync(fd) == 0) {
    success = 1;
  }

feedback_cleanup:
  close(fd);
  set_range_lock(lock_fd, 0, 0, F_UNLCK);
  close(lock_fd);

  if (success) {
    snprintf(response, RESPONSE_SIZE, "OK|Feedback submitted successfully.");
  } else {
    snprintf(response, RESPONSE_SIZE, "ERR|Could not save feedback.");
  }
}

static void handle_review_feedback(const char* payload,
                                   char response[RESPONSE_SIZE]) {
  unsigned long long requested_id = 0;

  /*
   * REVIEW_FEEDBACK|0000000001
   * A blank ID lists feedback awaiting review.
   */
  if (payload != NULL && payload[0] != '\0') {
    char* end = NULL;
    requested_id = strtoull(payload, &end, 10);

    if (end == payload || *end != '\0' || requested_id == 0) {
      snprintf(response, RESPONSE_SIZE, "ERR|Invalid feedback ID.");
      return;
    }
  }

  int fd = open("../data/feedback.txt", O_RDWR);

  if (fd == -1) {
    snprintf(response, RESPONSE_SIZE, "ERR|No feedback records found.");
    return;
  }

  struct stat st;

  if (fstat(fd, &st) == -1 || st.st_size % FEEDBACK_RECORD_SIZE != 0) {
    close(fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Feedback file format is invalid.");
    return;
  }

  if (requested_id != 0) {
    int found = 0;

    for (off_t offset = 0; offset < st.st_size;
         offset += FEEDBACK_RECORD_SIZE) {
      char record[FEEDBACK_RECORD_SIZE];

      if (read_record_at(fd, offset, record, sizeof(record)) == -1) continue;

      char id_text[11] = {0};
      memcpy(id_text, record, 10);

      if (strtoull(id_text, NULL, 10) != requested_id) continue;

      if (set_range_lock(fd, offset, FEEDBACK_RECORD_SIZE, F_WRLCK) == -1) {
        close(fd);
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Could not lock feedback record.");
        return;
      }

      if (read_record_at(fd, offset, record, sizeof(record)) == 0 &&
          record[237] == '\n' && strtoull(id_text, NULL, 10) == requested_id) {
        if (record[216] == 'N') {
          char reviewed = 'R';

          if (pwrite(fd, &reviewed, 1, offset + 216) == 1 && fsync(fd) == 0) {
            found = 1;
          }
        } else if (record[216] == 'R') {
          set_range_lock(fd, offset, FEEDBACK_RECORD_SIZE, F_UNLCK);
          close(fd);
          snprintf(response, RESPONSE_SIZE,
                   "OK|Feedback was already reviewed.");
          return;
        }
      }

      set_range_lock(fd, offset, FEEDBACK_RECORD_SIZE, F_UNLCK);
      break;
    }

    close(fd);

    if (found) {
      snprintf(response, RESPONSE_SIZE, "OK|Feedback marked as reviewed.");
    } else {
      snprintf(response, RESPONSE_SIZE,
               "ERR|Feedback not found or could not be updated.");
    }

    return;
  }

  size_t used = 0;
  used += (size_t)snprintf(response + used, RESPONSE_SIZE - used,
                           "Pending feedback:\n");

  int found = 0;

  for (off_t offset = 0; offset < st.st_size && used < RESPONSE_SIZE - 100;
       offset += FEEDBACK_RECORD_SIZE) {
    char record[FEEDBACK_RECORD_SIZE];

    if (read_record_at(fd, offset, record, sizeof(record)) == -1 ||
        record[237] != '\n' || record[216] != 'N')
      continue;

    char id_text[11] = {0};
    char customer_id[7] = {0};
    char message[201] = {0};

    memcpy(id_text, record, 10);
    memcpy(customer_id, record + 10, 6);
    memcpy(message, record + 16, 200);

    for (int i = 199; i >= 0 && message[i] == ' '; i--) message[i] = '\0';

    int written = snprintf(response + used, RESPONSE_SIZE - used,
                           "\nID: %s | Customer: %s\n%s\n", id_text,
                           customer_id, message);

    if (written < 0) break;

    used += (size_t)written;
    found = 1;
  }

  if (!found) {
    snprintf(response, RESPONSE_SIZE, "OK|There is no pending feedback.");
  }

  close(fd);
}

#define LOAN_RECORD_SIZE 74

#define LOANS_FILE "../data/loans.txt"
#define LOANS_LOCK_FILE "../data/loans.lock"

/*
 * Append a new loan application.
 * loans.lock serializes ID generation and appending.
 */
static void handle_apply_loan(const char* user_id, const char* amount_text,
                              char response[RESPONSE_SIZE]) {
  double amount;

  if (parse_amount(amount_text, &amount) != 0) {
    snprintf(response, RESPONSE_SIZE, "ERR|Invalid loan amount.");
    return;
  }

  int lock_fd = open(LOANS_LOCK_FILE, O_RDWR | O_CREAT, 0666);
  if (lock_fd == -1) {
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open loan lock.");
    return;
  }

  if (set_range_lock(lock_fd, 0, 0, F_WRLCK) == -1) {
    close(lock_fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot lock loan file.");
    return;
  }

  int fd = open(LOANS_FILE, O_RDWR | O_CREAT, 0666);
  if (fd == -1) {
    set_range_lock(lock_fd, 0, 0, F_UNLCK);
    close(lock_fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open loan file.");
    return;
  }

  struct stat st;
  int success = 0;
  unsigned long long max_id = 0;

  if (fstat(fd, &st) == -1 || st.st_size % LOAN_RECORD_SIZE != 0)
    goto loan_apply_cleanup;

  for (off_t offset = 0; offset < st.st_size; offset += LOAN_RECORD_SIZE) {
    char record[LOAN_RECORD_SIZE];

    if (read_record_at(fd, offset, record, sizeof(record)) == -1 ||
        record[73] != '\n')
      goto loan_apply_cleanup;

    char id_text[11] = {0};
    memcpy(id_text, record, 10);

    unsigned long long id = strtoull(id_text, NULL, 10);
    if (id > max_id) max_id = id;
  }

  if (max_id >= 9999999999ULL) goto loan_apply_cleanup;

  char record[LOAN_RECORD_SIZE];
  memset(record, ' ', sizeof(record));

  char id_text[11];
  snprintf(id_text, sizeof(id_text), "%010llu", max_id + 1);
  memcpy(record, id_text, 10);
  memcpy(record + 10, user_id, 6);

  /* No employee assigned yet. */
  memcpy(record + 16, "000000", 6);

  char amount_field[16];
  snprintf(amount_field, sizeof(amount_field), "%015.2f", amount);
  memcpy(record + 22, amount_field, 15);

  /* Approved amount starts at zero. */
  memcpy(record + 37, "000000000000.00", 15);

  record[52] = 'P';

  char timestamp[21];
  make_timestamp(timestamp);
  memcpy(record + 53, timestamp, 20);
  record[73] = '\n';

  if (pwrite(fd, record, sizeof(record), st.st_size) ==
          (ssize_t)sizeof(record) &&
      fsync(fd) == 0)
    success = 1;

loan_apply_cleanup:
  close(fd);
  set_range_lock(lock_fd, 0, 0, F_UNLCK);
  close(lock_fd);

  if (success) {
    snprintf(response, RESPONSE_SIZE,
             "OK|Loan application submitted. Loan ID: %010llu", max_id + 1);
  } else {
    snprintf(response, RESPONSE_SIZE, "ERR|Could not save loan application.");
  }
}

/* Manager: assign a pending loan to an active employee. */
static void handle_assign_loan(const char* loan_id, const char* employee_id,
                               char response[RESPONSE_SIZE]) {
  if (strlen(loan_id) != 10 || strlen(employee_id) != 6) {
    snprintf(response, RESPONSE_SIZE, "ERR|Invalid loan ID or employee ID.");
    return;
  }

  /* Verify that the employee exists, has role E, and is active. */
  int users_fd = open("../data/users.txt", O_RDONLY);
  int employee_valid = 0;

  if (users_fd == -1) {
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open users file.");
    return;
  }

  struct stat users_st;

  if (fstat(users_fd, &users_st) == 0 &&
      users_st.st_size % USER_RECORD_SIZE == 0) {
    for (off_t off = 0; off < users_st.st_size; off += USER_RECORD_SIZE) {
      char user[USER_RECORD_SIZE];

      if (read_record_at(users_fd, off, user, sizeof(user)) == -1) continue;

      if (memcmp(user, employee_id, 6) == 0 && user[6] == 'E' &&
          user[87] == 'A' && user[88] == '\n') {
        employee_valid = 1;
        break;
      }
    }
  }

  close(users_fd);

  if (!employee_valid) {
    snprintf(response, RESPONSE_SIZE,
             "ERR|Employee ID is invalid or inactive.");
    return;
  }

  int fd = open(LOANS_FILE, O_RDWR);
  if (fd == -1) {
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open loan file.");
    return;
  }

  struct stat st;
  if (fstat(fd, &st) == -1 || st.st_size % LOAN_RECORD_SIZE != 0) {
    close(fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Loan file format is invalid.");
    return;
  }

  for (off_t off = 0; off < st.st_size; off += LOAN_RECORD_SIZE) {
    char record[LOAN_RECORD_SIZE];

    if (read_record_at(fd, off, record, sizeof(record)) == -1) continue;

    if (memcmp(record, loan_id, 10) != 0) continue;

    if (set_range_lock(fd, off, LOAN_RECORD_SIZE, F_WRLCK) == -1) {
      close(fd);
      snprintf(response, RESPONSE_SIZE, "ERR|Could not lock loan record.");
      return;
    }

    int ok = 0;

    if (read_record_at(fd, off, record, sizeof(record)) == 0 &&
        record[73] == '\n' && record[52] == 'P') {
      memcpy(record + 16, employee_id, 6);
      record[52] = 'S';

      if (pwrite(fd, record, sizeof(record), off) == (ssize_t)sizeof(record) &&
          fsync(fd) == 0)
        ok = 1;
    }

    set_range_lock(fd, off, LOAN_RECORD_SIZE, F_UNLCK);
    close(fd);

    snprintf(response, RESPONSE_SIZE, "%s",
             ok ? "OK|Loan assigned to employee."
                : "ERR|Loan is not pending or could not be updated.");
    return;
  }

  close(fd);
  snprintf(response, RESPONSE_SIZE, "ERR|Loan ID not found.");
}

/* Employee: list applications assigned to this employee. */
static void handle_view_assigned_loans(const char* employee_id,
                                       char response[RESPONSE_SIZE]) {
  int fd = open(LOANS_FILE, O_RDONLY);

  if (fd == -1) {
    snprintf(response, RESPONSE_SIZE, "OK|No loan applications found.");
    return;
  }

  struct stat st;
  if (fstat(fd, &st) == -1 || st.st_size % LOAN_RECORD_SIZE != 0) {
    close(fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Loan file format is invalid.");
    return;
  }

  size_t used = (size_t)snprintf(response, RESPONSE_SIZE,
                                 "Your assigned loan applications:\n");
  int found = 0;

  for (off_t off = 0; off < st.st_size && used < RESPONSE_SIZE - 100;
       off += LOAN_RECORD_SIZE) {
    char record[LOAN_RECORD_SIZE];

    if (read_record_at(fd, off, record, sizeof(record)) == -1 ||
        record[73] != '\n')
      continue;

    if (memcmp(record + 16, employee_id, 6) != 0 || record[52] != 'S') continue;

    char id[11] = {0};
    char amount[16] = {0};
    memcpy(id, record, 10);
    memcpy(amount, record + 22, 15);

    int n = snprintf(response + used, RESPONSE_SIZE - used,
                     "\nLoan ID: %s | Customer: %.6s | Requested: %s\n", id,
                     record + 10, amount);
    if (n < 0) break;

    used += (size_t)n;
    found = 1;
  }

  close(fd);

  if (!found)
    snprintf(response, RESPONSE_SIZE,
             "OK|No pending loans are assigned to you.");
}

/*
 * Employee: approve or reject an assigned application.
 * Decision: A = approve; R = reject.
 *
 * Approval credits the customer's first active account.
 * A loan can only be processed while its status is S, so an
 * already-approved/rejected loan cannot be processed a second time.
 */
static void handle_process_loan(const char* employee_id, const char* loan_id,
                                char decision, const char* approved_text,
                                char response[RESPONSE_SIZE]) {
  if (strlen(loan_id) != 10 || (decision != 'A' && decision != 'R')) {
    snprintf(response, RESPONSE_SIZE, "ERR|Invalid loan-processing request.");
    return;
  }

  double approved_amount = 0.0;

  if (decision == 'A') {
    if (parse_amount(approved_text, &approved_amount) != 0) {
      snprintf(response, RESPONSE_SIZE, "ERR|Invalid approved amount.");
      return;
    }
  }

  int loan_fd = open(LOANS_FILE, O_RDWR);
  if (loan_fd == -1) {
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open loan file.");
    return;
  }

  struct stat loan_st;
  if (fstat(loan_fd, &loan_st) == -1 ||
      loan_st.st_size % LOAN_RECORD_SIZE != 0) {
    close(loan_fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Loan file format is invalid.");
    return;
  }

  for (off_t loan_off = 0; loan_off < loan_st.st_size;
       loan_off += LOAN_RECORD_SIZE) {
    char loan[LOAN_RECORD_SIZE];

    if (read_record_at(loan_fd, loan_off, loan, sizeof(loan)) == -1) continue;

    if (memcmp(loan, loan_id, 10) != 0) continue;

    if (set_range_lock(loan_fd, loan_off, LOAN_RECORD_SIZE, F_WRLCK) == -1) {
      close(loan_fd);
      snprintf(response, RESPONSE_SIZE, "ERR|Could not lock loan record.");
      return;
    }

    int result = 0;
    char customer_id[7] = {0};

    if (read_record_at(loan_fd, loan_off, loan, sizeof(loan)) == 0 &&
        loan[73] == '\n' && loan[52] == 'S' &&
        memcmp(loan + 16, employee_id, 6) == 0) {
      memcpy(customer_id, loan + 10, 6);

      if (decision == 'R') {
        loan[52] = 'R';
        memcpy(loan + 37, "000000000000.00", 15);

        if (pwrite(loan_fd, loan, sizeof(loan), loan_off) ==
                (ssize_t)sizeof(loan) &&
            fsync(loan_fd) == 0)
          result = 1;
      } else {
        /*
         * Locate the customer's active account.
         * Do not open/close another descriptor for accounts.txt
         * while holding its record lock.
         */
        int account_fd = open("../data/accounts.txt", O_RDWR);

        if (account_fd != -1) {
          char account_id[ACCOUNT_ID_SIZE + 1] = {0};
          off_t account_off;

          if (find_customer_account(account_fd, customer_id, account_id) == 0 &&
              find_account(account_fd, account_id, &account_off) == 0 &&
              set_record_lock(account_fd, account_off, F_WRLCK) == 0) {
            char account[ACCOUNT_RECORD_SIZE];
            double balance = 0.0;

            if (read_account_record(account_fd, account_off, account) == 0 &&
                memcmp(account + 10, customer_id, 6) == 0 &&
                get_balance(account_fd, account_off, &balance) == 0 &&
                balance + approved_amount <= 999999999999.99) {
              if (write_balance(account_fd, account_off,
                                balance + approved_amount) == 0) {
                char amount_field[16];
                snprintf(amount_field, sizeof(amount_field), "%015.2f",
                         approved_amount);
                memcpy(loan + 37, amount_field, 15);
                loan[52] = 'A';

                if (pwrite(loan_fd, loan, sizeof(loan), loan_off) ==
                        (ssize_t)sizeof(loan) &&
                    fsync(loan_fd) == 0) {
                  /*
                   * Record the loan disbursement in transaction history.
                   * Source account is zero because the funds come from the
                   * bank.
                   */
                  if (append_transaction('L', "0000000000", account_id,
                                         approved_amount) == 0) {
                    result = 1;

                  } else {
                    /*
                     * Best-effort rollback if the transaction could not be
                     * recorded. Restore the loan to assigned status so it can
                     * be retried.
                     */
                    loan[52] = 'S';
                    memcpy(loan + 37, "000000000000.00", 15);

                    (void)pwrite(loan_fd, loan, sizeof(loan), loan_off);
                    (void)fsync(loan_fd);

                    (void)write_balance(account_fd, account_off, balance);
                  }

                } else {
                  /* Loan record update failed: restore the previous balance. */
                  (void)write_balance(account_fd, account_off, balance);
                }
              }
            }

            set_record_lock(account_fd, account_off, F_UNLCK);
          }

          close(account_fd);
        }
      }
    }

    set_range_lock(loan_fd, loan_off, LOAN_RECORD_SIZE, F_UNLCK);
    close(loan_fd);

    if (result) {
      snprintf(response, RESPONSE_SIZE, "%s",
               decision == 'A' ? "OK|Loan approved and amount credited."
                               : "OK|Loan application rejected.");
    } else {
      snprintf(response, RESPONSE_SIZE,
               "ERR|Loan not assigned to you, already processed, "
               "or the operation failed.");
    }
    return;
  }

  close(loan_fd);
  snprintf(response, RESPONSE_SIZE, "ERR|Loan ID not found.");
}

/* Management helpers for fixed-width users.txt and accounts.txt records. */
#define MANAGEMENT_USERS_FILE "../data/users.txt"
#define MANAGEMENT_USERS_LOCK "../data/users.lock"
#define MANAGEMENT_ACCOUNTS_LOCK "../data/accounts.lock"
#define MANAGEMENT_USER_SIZE 89

static int valid_field(const char* s, size_t max, int allow_spaces) {
  size_t n;
  if (!s || (n = strlen(s)) == 0 || n > max || strchr(s, '|') ||
      strchr(s, '\n') || strchr(s, '\r'))
    return 0;
  if (!allow_spaces && strpbrk(s, " \t")) return 0;
  return 1;
}

static void put_field(char* record, size_t offset, size_t width,
                      const char* s) {
  memset(record + offset, ' ', width);
  size_t n = strlen(s);
  if (n > width) n = width;
  memcpy(record + offset, s, n);
}

static int lock_whole_file(const char* path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) return -1;
  if (set_range_lock(fd, 0, 0, F_WRLCK) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int username_exists(const char* username) {
  int fd = open(MANAGEMENT_USERS_FILE, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0) {
    close(fd);
    return -1;
  }
  for (off_t off = 0; off + MANAGEMENT_USER_SIZE <= st.st_size;
       off += MANAGEMENT_USER_SIZE) {
    char r[MANAGEMENT_USER_SIZE], u[21];
    if (read_record_at(fd, off, r, sizeof(r)) < 0 || r[88] != '\n') continue;
    memcpy(u, r + 47, 20);
    u[20] = '\0';
    for (int i = 19; i >= 0 && u[i] == ' '; --i) u[i] = '\0';
    if (strcmp(u, username) == 0) {
      close(fd);
      return 1;
    }
  }
  close(fd);
  return 0;
}

static int next_numeric_id(const char* path, size_t record_size,
                           size_t id_width, unsigned long* next) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0) {
    close(fd);
    return -1;
  }
  unsigned long max_id = 0;
  char rec[256];
  if (record_size > sizeof(rec)) {
    close(fd);
    return -1;
  }
  for (off_t off = 0; off + (off_t)record_size <= st.st_size;
       off += (off_t)record_size) {
    if (read_record_at(fd, off, rec, record_size) < 0) continue;
    char id[32];
    if (id_width >= sizeof(id)) {
      close(fd);
      return -1;
    }
    memcpy(id, rec, id_width);
    id[id_width] = '\0';
    char* end = NULL;
    unsigned long value = strtoul(id, &end, 10);
    if (end && *end == '\0' && value > max_id) max_id = value;
  }
  close(fd);
  *next = max_id + 1;
  return 0;
}

static int create_user_record(char role, const char* first, const char* last,
                              const char* username, const char* password,
                              char new_user_id[7]) {
  if (!valid_field(first, 20, 0) || !valid_field(last, 20, 0) ||
      !valid_field(username, 20, 0) || !valid_field(password, 20, 0))
    return -1;
  int lockfd = lock_whole_file(MANAGEMENT_USERS_LOCK);
  if (lockfd < 0) return -1;
  int exists = username_exists(username);
  unsigned long id = 0;
  int result = -1;
  if (exists == 0 &&
      next_numeric_id(MANAGEMENT_USERS_FILE, MANAGEMENT_USER_SIZE, 6, &id) ==
          0 &&
      id <= 999999) {
    char rec[MANAGEMENT_USER_SIZE];
    memset(rec, ' ', sizeof(rec));
    snprintf(new_user_id, 7, "%06lu", id);
    memcpy(rec, new_user_id, 6);
    rec[6] = role;
    put_field(rec, 7, 20, first);
    put_field(rec, 27, 20, last);
    put_field(rec, 47, 20, username);
    put_field(rec, 67, 20, password);
    rec[87] = 'A';
    rec[88] = '\n';
    int fd = open(MANAGEMENT_USERS_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
      ssize_t n = write(fd, rec, sizeof(rec));
      if (n == (ssize_t)sizeof(rec) && fsync(fd) == 0) result = 0;
      close(fd);
    }
  }
  set_range_lock(lockfd, 0, 0, F_UNLCK);
  close(lockfd);
  return result;
}

static int create_customer_account(const char* user_id, char account_id[11]) {
  int lockfd = lock_whole_file(MANAGEMENT_ACCOUNTS_LOCK);
  if (lockfd < 0) return -1;
  unsigned long id = 0;
  int result = -1;
  if (next_numeric_id(ACCOUNTS_FILE, ACCOUNT_RECORD_SIZE, 10, &id) == 0 &&
      id <= 9999999999UL) {
    char rec[ACCOUNT_RECORD_SIZE];
    memset(rec, ' ', sizeof(rec));
    snprintf(account_id, 11, "%010lu", id);
    memcpy(rec, account_id, 10);
    memcpy(rec + 10, user_id, 6);
    memcpy(rec + 16, "000000000000.00", 15);
    rec[31] = 'A';
    char ts[21];
    make_timestamp(ts);
    memcpy(rec + 32, ts, 20);
    rec[52] = '\n';
    int fd = open(ACCOUNTS_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
      ssize_t n = write(fd, rec, sizeof(rec));
      if (n == (ssize_t)sizeof(rec) && fsync(fd) == 0) result = 0;
      close(fd);
    }
  }
  set_range_lock(lockfd, 0, 0, F_UNLCK);
  close(lockfd);
  return result;
}

static void handle_add_person(char role, const char* payload, char* response) {
  char first[21] = {0}, last[21] = {0}, username[21] = {0}, password[21] = {0},
       extra;
  if (sscanf(payload, "%20[^|]|%20[^|]|%20[^|]|%20[^|]%c", first, last,
             username, password, &extra) != 4 ||
      !valid_field(first, 20, 0) || !valid_field(last, 20, 0) ||
      !valid_field(username, 20, 0) || !valid_field(password, 20, 0)) {
    snprintf(response, RESPONSE_SIZE,
             "ERR|Format: command|first_name|last_name|username|password (no "
             "spaces in fields)");
    return;
  }
  char uid[7] = {0};
  if (create_user_record(role, first, last, username, password, uid) != 0) {
    snprintf(response, RESPONSE_SIZE,
             "ERR|Could not create user; username may already exist or storage "
             "failed.");
    return;
  }
  if (role == 'C') {
    char aid[11] = {0};
    if (create_customer_account(uid, aid) != 0) {
      /* Best-effort rollback: mark the just-created user inactive. */
      int fd = open(MANAGEMENT_USERS_FILE, O_RDWR);
      if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0)
          for (off_t off = 0; off + MANAGEMENT_USER_SIZE <= st.st_size;
               off += MANAGEMENT_USER_SIZE) {
            char r[MANAGEMENT_USER_SIZE];
            if (read_record_at(fd, off, r, sizeof(r)) == 0 &&
                memcmp(r, uid, 6) == 0) {
              pwrite(fd, "D", 1, off + 87);
              fsync(fd);
              break;
            }
          }
        close(fd);
      }
      snprintf(response, RESPONSE_SIZE,
               "ERR|Customer user created but account creation failed; user "
               "was deactivated.");
      return;
    }
    snprintf(response, RESPONSE_SIZE,
             "OK|Customer created. User ID: %s Account ID: %s", uid, aid);
  } else
    snprintf(response, RESPONSE_SIZE, "OK|Employee created. User ID: %s", uid);
}

static void handle_modify_person(const char* payload, char expected_role,
                                 char* response) {
  char uid[7] = {0}, first[21] = {0}, last[21] = {0}, username[21] = {0}, extra;
  if (sscanf(payload, "%6[^|]|%20[^|]|%20[^|]|%20[^|]%c", uid, first, last,
             username, &extra) != 4 ||
      strlen(uid) != 6 || !valid_field(first, 20, 0) ||
      !valid_field(last, 20, 0) || !valid_field(username, 20, 0)) {
    snprintf(response, RESPONSE_SIZE,
             "ERR|Format: command|user_id|first_name|last_name|username");
    return;
  }
  int lockfd = lock_whole_file(MANAGEMENT_USERS_LOCK);
  if (lockfd < 0) {
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot lock users file.");
    return;
  }
  int duplicate = username_exists(username);
  int fd = open(MANAGEMENT_USERS_FILE, O_RDWR);
  int result = -1;
  if (fd >= 0) {
    struct stat st;
    if (fstat(fd, &st) == 0)
      for (off_t off = 0; off + MANAGEMENT_USER_SIZE <= st.st_size;
           off += MANAGEMENT_USER_SIZE) {
        char r[MANAGEMENT_USER_SIZE];
        if (read_record_at(fd, off, r, sizeof(r)) < 0 || memcmp(r, uid, 6) != 0)
          continue;
        if (r[6] != expected_role || r[87] != 'A') break;
        char old[21];
        memcpy(old, r + 47, 20);
        old[20] = '\0';
        for (int i = 19; i >= 0 && old[i] == ' '; --i) old[i] = '\0';
        if (duplicate == 1 && strcmp(old, username) != 0) break;
        put_field(r, 7, 20, first);
        put_field(r, 27, 20, last);
        put_field(r, 47, 20, username);
        if (set_range_lock(fd, off, MANAGEMENT_USER_SIZE, F_WRLCK) == 0) {
          if (pwrite(fd, r, sizeof(r), off) == (ssize_t)sizeof(r) &&
              fsync(fd) == 0)
            result = 0;
          set_range_lock(fd, off, MANAGEMENT_USER_SIZE, F_UNLCK);
        }
        break;
      }
    close(fd);
  }
  set_range_lock(lockfd, 0, 0, F_UNLCK);
  close(lockfd);
  snprintf(response, RESPONSE_SIZE,
           result == 0 ? "OK|User details updated."
                       : "ERR|User not found, wrong role, duplicate username, "
                         "or storage error.");
}

static void handle_account_status(const char* payload, char* response) {
  char aid[11] = {0}, status = 0, extra;
  if (sscanf(payload, "%10[^|]|%c%c", aid, &status, &extra) < 2 ||
      strlen(aid) != 10 || (status != 'A' && status != 'D')) {
    snprintf(response, RESPONSE_SIZE,
             "ERR|Format: ACCOUNT_STATUS|account_id|A-or-D");
    return;
  }
  int fd = open(ACCOUNTS_FILE, O_RDWR);
  if (fd < 0) {
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open accounts file.");
    return;
  }
  struct stat st;
  off_t off = -1;
  if (fstat(fd, &st) == 0)
    for (off_t pos = 0; pos + ACCOUNT_RECORD_SIZE <= st.st_size;
         pos += ACCOUNT_RECORD_SIZE) {
      char r[ACCOUNT_RECORD_SIZE];
      if (read_record_at(fd, pos, r, sizeof(r)) == 0 && r[52] == '\n' &&
          memcmp(r, aid, 10) == 0) {
        off = pos;
        break;
      }
    }
  if (off < 0) {
    close(fd);
    snprintf(response, RESPONSE_SIZE, "ERR|Account not found.");
    return;
  }
  int result = -1;
  if (set_record_lock(fd, off, F_WRLCK) == 0) {
    char r[ACCOUNT_RECORD_SIZE];
    if (read_record_at(fd, off, r, sizeof(r)) == 0 && r[52] == '\n' &&
        memcmp(r, aid, 10) == 0) {
      if (pwrite(fd, &status, 1, off + 31) == 1 && fsync(fd) == 0) result = 0;
    }
    set_record_lock(fd, off, F_UNLCK);
  }
  close(fd);
  snprintf(response, RESPONSE_SIZE,
           result == 0 ? "OK|Account status updated."
                       : "ERR|Could not update account status.");
}

static void handle_manage_role(const char* payload, char* response) {
  char uid[7] = {0}, role = 0, extra;
  if (sscanf(payload, "%6[^|]|%c%c", uid, &role, &extra) < 2 ||
      strlen(uid) != 6 || !strchr("CE MA", role) || role == ' ') {
    snprintf(response, RESPONSE_SIZE,
             "ERR|Format: MANAGE_ROLES|user_id|C/E/M/A");
    return;
  }
  int fd = open(MANAGEMENT_USERS_FILE, O_RDWR);
  if (fd < 0) {
    snprintf(response, RESPONSE_SIZE, "ERR|Cannot open users file.");
    return;
  }
  struct stat st;
  int result = -1;
  if (fstat(fd, &st) == 0)
    for (off_t off = 0; off + MANAGEMENT_USER_SIZE <= st.st_size;
         off += MANAGEMENT_USER_SIZE) {
      char r[MANAGEMENT_USER_SIZE];
      if (read_record_at(fd, off, r, sizeof(r)) < 0 || memcmp(r, uid, 6) != 0)
        continue;
      if (r[87] != 'A') break;
      if (set_range_lock(fd, off, MANAGEMENT_USER_SIZE, F_WRLCK) == 0) {
        if (pwrite(fd, &role, 1, off + 6) == 1 && fsync(fd) == 0) result = 0;
        set_range_lock(fd, off, MANAGEMENT_USER_SIZE, F_UNLCK);
      }
      break;
    }
  close(fd);
  snprintf(response, RESPONSE_SIZE,
           result == 0 ? "OK|User role updated."
                       : "ERR|Active user not found or update failed.");
}

static void handle_passbook(const char* payload, char* response,
                            int response_size) {
  char customer_id[7] = {0};
  int page = 0;
  char extra;
  int fields = sscanf(payload, "%6[^|]|%d%c", customer_id, &page, &extra);
  if (fields < 1 || strlen(customer_id) != 6 || page < 0 || page > 100000) {
    snprintf(response, (size_t)response_size,
             "ERR|Format: PASSBOOK|customer_user_id|page");
    return;
  }
  int afd = open(ACCOUNTS_FILE, O_RDONLY);
  if (afd < 0) {
    snprintf(response, (size_t)response_size, "ERR|Cannot open accounts file.");
    return;
  }
  char aid[11] = {0};
  off_t ao = 0;
  struct stat st;
  int found = 0;
  if (fstat(afd, &st) == 0)
    for (off_t off = 0; off + ACCOUNT_RECORD_SIZE <= st.st_size;
         off += ACCOUNT_RECORD_SIZE) {
      char r[ACCOUNT_RECORD_SIZE];
      if (read_record_at(afd, off, r, sizeof(r)) == 0 &&
          memcmp(r + 10, customer_id, 6) == 0 && r[31] == 'A') {
        memcpy(aid, r, 10);
        aid[10] = '\0';
        ao = off;
        found = 1;
        break;
      }
    }
  double balance = 0;
  char ar[ACCOUNT_RECORD_SIZE];
  if (!found || read_account_record(afd, ao, ar) != 0 ||
      get_balance(afd, ao, &balance) != 0) {
    close(afd);
    snprintf(response, (size_t)response_size,
             "ERR|Customer account not found or inactive.");
    return;
  }
  close(afd);
  int tfd = open(TRANSACTIONS_FILE, O_RDONLY);
  int used = snprintf(response, (size_t)response_size,
                      "OK|Passbook user %s | Account %s | Balance %.2f\nPage "
                      "%d (newest first):\n",
                      customer_id, aid, balance, page + 1);
  if (tfd < 0) {
    if (used < 0 || (size_t)used >= (size_t)response_size)
      snprintf(response, (size_t)response_size,
               "OK|Passbook user %s | Account %s | Balance %.2f", customer_id,
               aid, balance);
    return;
  }
  struct stat tst;
  int shown = 0, skipped = 0, wanted_skip = page * 5;
  if (fstat(tfd, &tst) == 0) {
    for (off_t off = tst.st_size - TRANSACTION_RECORD_SIZE;
         off >= 0 && shown < 5; off -= TRANSACTION_RECORD_SIZE) {
      char tr[TRANSACTION_RECORD_SIZE];
      if (read_record_at(tfd, off, tr, sizeof(tr)) < 0 || tr[67] != '\n' ||
          tr[66] != 'C')
        continue;
      if (memcmp(tr + 11, aid, 10) != 0 && memcmp(tr + 21, aid, 10) != 0)
        continue;
      if (skipped < wanted_skip) {
        skipped++;
        continue;
      }
      char typ[24];
      switch (tr[10]) {
        case 'D':
          snprintf(typ, sizeof(typ), "DEPOSIT");
          break;
        case 'W':
          snprintf(typ, sizeof(typ), "WITHDRAWAL");
          break;
        case 'T':
          snprintf(typ, sizeof(typ), "TRANSFER");
          break;
        case 'L':
          snprintf(typ, sizeof(typ), "LOAN DISBURSEMENT");
          break;
        default:
          snprintf(typ, sizeof(typ), "OTHER");
      }
      char amt[16], ts[21], tid[11];
      memcpy(tid, tr, 10);
      tid[10] = '\0';
      memcpy(amt, tr + 31, 15);
      amt[15] = '\0';
      memcpy(ts, tr + 46, 20);
      ts[20] = '\0';
      while (strlen(ts) > 0 && ts[strlen(ts) - 1] == ' ')
        ts[strlen(ts) - 1] = '\0';
      if (used > 0 && (size_t)used < (size_t)response_size) {
        int n = snprintf(response + used, (size_t)response_size - (size_t)used,
                         "#%s %s Rs.%s %s\n", tid, typ, amt, ts);
        if (n < 0 || (size_t)n >= (size_t)response_size - (size_t)used) break;
        used += n;
      }
      shown++;
    }
  }
  close(tfd);
  if (used > 0 && (size_t)used < (size_t)response_size && shown == 0)
    snprintf(response + used, (size_t)response_size - (size_t)used,
             "No more completed transactions on this page.");
}

/*
 * Enforce one active login per user across forked client-handler processes.
 * Each user ID owns one byte-range lock in sessions.lock. The lock is held
 * until logout, disconnect, or process termination; the OS releases it if the
 * child crashes. Open this file only once per client child: POSIX record locks
 * can be dropped when that process closes another descriptor for the same file.
 */
static int acquire_user_session(const char* user_id) {
  char* end = NULL;
  errno = 0;
  long id = strtol(user_id, &end, 10);
  if (errno != 0 || end == user_id || *end != '\0' || id <= 0) {
    errno = EINVAL;
    return -1;
  }

  int fd = open("../data/sessions.lock", O_CREAT | O_RDWR, 0600);
  if (fd < 0) return -1;

  struct flock lock;
  memset(&lock, 0, sizeof(lock));
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = (off_t)id;
  lock.l_len = 1;

  /* Non-blocking: reject a second login instead of making it wait. */
  if (fcntl(fd, F_SETLK, &lock) < 0) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  return fd;
}

static void log_request_event(const char* request) {
  char command[40] = {0};
  const char* sep = strchr(request, '|');
  size_t n = sep ? (size_t)(sep - request) : strcspn(request, "\r\n");
  if (n >= sizeof(command)) n = sizeof(command) - 1;
  memcpy(command, request, n);
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[32] = "unknown-time";
  if (localtime_r(&now, &tm_now))
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);

  /* Never write credentials or password-change payloads to the server console.
   */
  if (!strcmp(command, "LOGIN") || !strcmp(command, "REGISTER_CUSTOMER") ||
      !strcmp(command, "CHANGE_PASSWORD") || !strcmp(command, "ADD_CUSTOMER") ||
      !strcmp(command, "ADD_EMPLOYEE")) {
    printf("[%s] pid=%ld request=%s (sensitive fields redacted)\n", stamp,
           (long)getpid(), command);
  } else if (sep) {
    printf("[%s] pid=%ld request=%s details=%s\n", stamp, (long)getpid(),
           command, sep + 1);
  } else {
    printf("[%s] pid=%ld request=%s\n", stamp, (long)getpid(), command);
  }
  fflush(stdout);
}

static void log_response_event(const char* response) {
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[32] = "unknown-time";
  if (localtime_r(&now, &tm_now))
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);
  printf("[%s] pid=%ld result=%s\n", stamp, (long)getpid(), response);
  fflush(stdout);
}

static void handle_client(int client_fd) {
  char request[REQUEST_SIZE];
  char response[RESPONSE_SIZE];

  char user_id[USER_ID_SIZE + 1] = {0};
  char first_name[21] = {0};
  char role = '\0';

  int authenticated = 0;
  int session_lock_fd = -1;

  while (1) {
    memset(request, 0, sizeof(request));
    memset(response, 0, sizeof(response));

    if (recv_all(client_fd, request, sizeof(request)) < 0) {
      break;
    }
    request[sizeof(request) - 1] = '\0';
    log_request_event(request);

    /* Authenticate before accepting other commands. */
    if (!authenticated) {
      if (strncmp(request, "REGISTER_CUSTOMER|", 18) == 0) {
        handle_add_person('C', request + 18, response);
        log_response_event(response);
        if (send_all(client_fd, response, sizeof(response)) < 0) break;
        continue;
      }
      if (strncmp(request, "LOGIN|", 6) != 0) {
        snprintf(response, sizeof(response), "ERR|Please log in first");

        log_response_event(response);
        if (send_all(client_fd, response, sizeof(response)) < 0) {
          break;
        }

        continue;
      }

      char* username = request + 6;
      char* separator = strchr(username, '|');

      if (separator == NULL) {
        snprintf(response, sizeof(response), "ERR|Invalid login request");
      } else {
        *separator = '\0';
        char* password = separator + 1;

        if (strlen(username) == 0 || strlen(username) > USERNAME_SIZE ||
            strlen(password) == 0 || strlen(password) > PASSWORD_SIZE) {
          snprintf(response, sizeof(response),
                   "ERR|Invalid username or password");
        } else if (authenticate_user(username, password, user_id, &role,
                                     first_name) != 0) {
          snprintf(response, sizeof(response), "ERR|Login failed");
        } else {
          session_lock_fd = acquire_user_session(user_id);
          if (session_lock_fd < 0) {
            if (errno == EACCES || errno == EAGAIN) {
              snprintf(response, sizeof(response),
                       "ERR|This user is already logged in");
            } else {
              perror("acquire user session");
              snprintf(response, sizeof(response),
                       "ERR|Unable to establish login session");
            }
          } else {
            authenticated = 1;
            snprintf(response, sizeof(response), "OK|LOGIN|%s|%c|%s", user_id,
                     role, first_name);
          }
        }
      }

      log_response_event(response);
      if (send_all(client_fd, response, sizeof(response)) < 0) {
        break;
      }

      /*
       * Failed login leaves the connection unauthenticated.
       * The client may retry or disconnect.
       */
      continue;
    }

    /* From here onward, this connection is authenticated. */

    if (strncmp(request, "MENU", 4) == 0) {
      make_role_menu(role, response, sizeof(response));
    } else if (strncmp(request, "BALANCE", 7) == 0) {
      if (role != 'C') {
        snprintf(response, sizeof(response),
                 "ERR|Only customers can use this operation");
      } else {
        int account_fd = open("../data/accounts.txt", O_RDONLY);

        if (account_fd < 0) {
          perror("open accounts.txt");

          snprintf(response, sizeof(response),
                   "ERR|Unable to open account database");
        } else {
          char account_id[ACCOUNT_ID_SIZE + 1];
          off_t record_offset;
          double balance;

          if (find_customer_account(account_fd, user_id, account_id) != 0) {
            snprintf(response, sizeof(response), "ERR|No active account found");
          } else if (find_account(account_fd, account_id, &record_offset) !=
                     0) {
            snprintf(response, sizeof(response),
                     "ERR|Unable to locate account");
          } else if (get_balance(account_fd, record_offset, &balance) != 0) {
            snprintf(response, sizeof(response), "ERR|Unable to read balance");
          } else {
            snprintf(response, sizeof(response), "OK|Account: %s|Balance: %.2f",
                     account_id, balance);
          }

          close(account_fd);
        }
      }
    } else if (strncmp(request, "DEPOSIT|", 8) == 0) {
      if (role != 'C') {
        snprintf(response, sizeof(response), "ERR|Only customers can deposit");
      } else {
        char* amount = request + 8;

        if (strchr(amount, '|') != NULL) {
          snprintf(response, sizeof(response), "ERR|Invalid deposit request");
        } else {
          handle_deposit_or_withdraw(user_id, 'D', amount, response,
                                     sizeof(response));
        }
      }

    } else if (strncmp(request, "WITHDRAW|", 9) == 0) {
      if (role != 'C') {
        snprintf(response, sizeof(response), "ERR|Only customers can withdraw");
      } else {
        char* amount = request + 9;

        if (strchr(amount, '|') != NULL) {
          snprintf(response, sizeof(response),
                   "ERR|Invalid withdrawal request");
        } else {
          handle_deposit_or_withdraw(user_id, 'W', amount, response,
                                     sizeof(response));
        }
      }

    } else if (strncmp(request, "TRANSFER|", 9) == 0) {
      if (role != 'C') {
        snprintf(response, sizeof(response),
                 "ERR|Only customers can transfer funds");
      } else {
        char* destination = request + 9;
        char* separator = strchr(destination, '|');

        if (separator == NULL) {
          snprintf(response, sizeof(response), "ERR|Invalid transfer request");
        } else {
          *separator = '\0';
          char* amount = separator + 1;

          if (strchr(amount, '|') != NULL) {
            snprintf(response, sizeof(response),
                     "ERR|Invalid transfer request");
          } else {
            handle_transfer(user_id, destination, amount, response,
                            sizeof(response));
          }
        }
      }

    } else if (strncmp(request, "HISTORY", 7) == 0 &&
               (request[7] == '\0' || request[7] == '|')) {
      if (role != 'C') {
        snprintf(response, sizeof(response),
                 "ERR|Only customers can view their transaction history");
      } else {
        int page = 0;
        if (request[7] == '|' && (sscanf(request + 8, "%d", &page) != 1 ||
                                  page < 0 || page > 100000)) {
          snprintf(response, sizeof(response),
                   "ERR|Invalid transaction-history page");
        } else {
          handle_history(user_id, page, response, sizeof(response));
        }
      }

    } else if (strncmp(request, "CHANGE_PASSWORD|", 16) == 0) {
      if (role == 'C' || role == 'E' || role == 'M' || role == 'A') {
        handle_change_password(user_id, request + 16, response);
      } else {
        snprintf(response, RESPONSE_SIZE, "ERR|Permission denied.");
      }
    } else if (strncmp(request, "FEEDBACK|", 9) == 0) {
      if (role != 'C') {
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only customers can submit feedback.");
      } else {
        handle_submit_feedback(user_id, request + 9, response);
      }
    } else if (strcmp(request, "REVIEW_FEEDBACK") == 0) {
      if (role != 'M') {
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only managers can review feedback.");
      } else {
        handle_review_feedback("", response);
      }
    } else if (strncmp(request, "REVIEW_FEEDBACK|", 16) == 0) {
      if (role != 'M') {
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only managers can review feedback.");
      } else {
        handle_review_feedback(request + 16, response);
      }
    } else if (strncmp(request, "APPLY_LOAN|", 11) == 0) {
      if (role != 'C') {
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only customers can apply for loans.");
      } else {
        handle_apply_loan(user_id, request + 11, response);
      }
    } else if (strncmp(request, "ASSIGN_LOAN|", 12) == 0) {
      if (role != 'M') {
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only managers can assign loans.");
      } else {
        char loan_id[11] = {0};
        char employee_id[7] = {0};
        char extra;

        if (sscanf(request + 12, "%10[^|]|%6s%c", loan_id, employee_id,
                   &extra) < 2 ||
            strlen(loan_id) != 10 || strlen(employee_id) != 6) {
          snprintf(response, RESPONSE_SIZE,
                   "ERR|Format: ASSIGN_LOAN|loan_id|employee_id");
        } else {
          handle_assign_loan(loan_id, employee_id, response);
        }
      }
    } else if (strcmp(request, "VIEW_ASSIGNED_LOANS") == 0) {
      if (role != 'E') {
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only employees can view assigned loans.");
      } else {
        handle_view_assigned_loans(user_id, response);
      }
    } else if (strncmp(request, "PROCESS_LOAN|", 13) == 0) {
      if (role != 'E') {
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only employees can process loans.");
      } else {
        char loan_id[11] = {0};
        char decision_text[2] = {0};
        char amount[32] = {0};
        char extra;

        int fields = sscanf(request + 13, "%10[^|]|%1[^|]|%31[^|]%c", loan_id,
                            decision_text, amount, &extra);

        if (fields < 3 || strlen(loan_id) != 10 || strlen(decision_text) != 1 ||
            (decision_text[0] != 'A' && decision_text[0] != 'R')) {
          snprintf(response, RESPONSE_SIZE,
                   "ERR|Format: PROCESS_LOAN|loan_id|A/R|amount");
        } else {
          handle_process_loan(user_id, loan_id, decision_text[0], amount,
                              response);
        }
      }
    } else if (strncmp(request, "ADD_CUSTOMER|", 13) == 0) {
      if (role != 'E')
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only employees can add customers.");
      else
        handle_add_person('C', request + 13, response);
    } else if (strncmp(request, "ADD_EMPLOYEE|", 13) == 0) {
      if (role != 'A')
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only administrators can add employees.");
      else
        handle_add_person('E', request + 13, response);
    } else if (strncmp(request, "MODIFY_CUSTOMER|", 16) == 0) {
      if (role != 'E' && role != 'A')
        snprintf(response, RESPONSE_SIZE, "ERR|Permission denied.");
      else
        handle_modify_person(request + 16, 'C', response);
    } else if (strncmp(request, "MODIFY_EMPLOYEE|", 16) == 0) {
      if (role != 'A')
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only administrators can modify employees.");
      else
        handle_modify_person(request + 16, 'E', response);
    } else if (strncmp(request, "ACCOUNT_STATUS|", 15) == 0) {
      if (role != 'M')
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only managers can change account status.");
      else
        handle_account_status(request + 15, response);
    } else if (strncmp(request, "MANAGE_ROLES|", 13) == 0) {
      if (role != 'A')
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only administrators can manage roles.");
      else
        handle_manage_role(request + 13, response);
    } else if (strncmp(request, "PASSBOOK|", 9) == 0) {
      if (role != 'E')
        snprintf(response, RESPONSE_SIZE,
                 "ERR|Only employees can view passbooks.");
      else
        handle_passbook(request + 9, response, RESPONSE_SIZE);
    } else if (strncmp(request, "LOGOUT", 6) == 0) {
      snprintf(response, sizeof(response), "OK|Logged out successfully");
      log_response_event(response);

      if (send_all(client_fd, response, sizeof(response)) < 0) {
        break;
      }

      break;
    } else {
      snprintf(response, sizeof(response), "ERR|Unknown command");
    }

    log_response_event(response);
    if (send_all(client_fd, response, sizeof(response)) < 0) {
      break;
    }
  }

  printf("[%ld] client connection closed; authenticated_user=%s\n",
         (long)getpid(), authenticated ? user_id : "none");
  fflush(stdout);
  /* Closing the session-lock descriptor releases this user's login slot. */
  if (session_lock_fd >= 0) close(session_lock_fd);

  /* The caller closes client_fd after handle_client returns. */
}

int main(void) {
  int server_fd;
  setvbuf(stdout, NULL, _IOLBF, 0);
  struct sockaddr_in server_addr;

  /*
   * Let the OS reap child processes automatically so terminated
   * clients do not accumulate as zombies.
   */
  signal(SIGCHLD, SIG_IGN);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (server_fd < 0) {
    perror("socket");
    return 1;
  }

  int reuse = 1;

  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    perror("setsockopt");
    close(server_fd);
    return 1;
  }

  memset(&server_addr, 0, sizeof(server_addr));

  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(PORT);

  if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) <
      0) {
    perror("bind");
    close(server_fd);
    return 1;
  }

  if (listen(server_fd, 10) < 0) {
    perror("listen");
    close(server_fd);
    return 1;
  }

  printf("Banking server listening on port %d...\n", PORT);

  while (1) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd =
        accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }

      perror("accept");
      continue;
    }

    printf("Accepted client connection from %s:%u\n",
           inet_ntoa(client_addr.sin_addr),
           (unsigned)ntohs(client_addr.sin_port));
    fflush(stdout);

    pid_t pid = fork();

    if (pid < 0) {
      perror("fork");
      close(client_fd);
      continue;
    }

    if (pid == 0) {
      /* Child process handles this client. */
      close(server_fd);

      handle_client(client_fd);

      close(client_fd);
      _exit(0);
    }

    /* Parent continues accepting new clients. */
    close(client_fd);
  }

  close(server_fd);
  return 0;
}