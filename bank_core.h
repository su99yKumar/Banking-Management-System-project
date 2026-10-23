#ifndef BANK_CORE_H
#define BANK_CORE_H

#include <stddef.h>

/*
 * Process a customer's own banking command:
 *   BALANCE
 *   DEPOSIT amount
 *   WITHDRAW amount
 *
 * transaction_semid coordinates transaction-log updates.
 * Returns 0 when a response is prepared, or -1 for invalid
 * arguments or an invalid response buffer.
 */
int bank_process_command(const char *file_path,
                         const char *account_id,
                         const char *command,
                         char *response,
                         size_t response_capacity,
                         int transaction_semid);

/*
 * Read-only lookup of one explicitly specified account.
 * The caller (server) must authorize the user's role.
 * Returns an account ID and balance; never changes the record.
 */
int bank_lookup_account(const char *file_path,
                        const char *account_id,
                        char *response,
                        size_t response_capacity);


/*
 * Transfer money between two account records.
 * Authorization is enforced by the server.
 */
int bank_transfer_command(const char *file_path,
                          const char *from_account_id,
                          const char *command,
                          char *response,
                          size_t response_capacity,
                          int transaction_semid);

/* Return the latest five log entries for one account. */
int bank_transaction_history(const char *log_path,
                             const char *account_id,
                             char *response,
                             size_t response_capacity,
                             int transaction_semid);


/* Manager: activate or deactivate an account (A or D). */
int bank_set_account_status(const char *file_path,
                            const char *account_id,
                            char status,
                            char *response,
                            size_t response_capacity);

/* Employee/admin: modify a customer's display name. */
int bank_modify_customer_name(const char *file_path,
                              const char *account_id,
                              const char *new_name,
                              char *response,
                              size_t response_capacity);


/* Return 1 if active, 0 if deactivated, or -1 on an error. */
int bank_account_is_active(const char *file_path,
                           const char *account_id);


/* Create an active customer account with a zero balance. */
int bank_create_customer(const char *file_path,
                         const char *username,
                         const char *full_name,
                         char account_id_out[7],
                         char *response,
                         size_t response_capacity);

#endif
