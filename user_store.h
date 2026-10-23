#ifndef USER_STORE_H
#define USER_STORE_H

#include <stddef.h>

/* Create the credential file on first start; preserve it on later starts. */
int user_store_init(const char *const usernames[],
                    const char *const passwords[],
                    size_t count);

/* Return 1 for a match, 0 for wrong credentials, and -1 on an error. */
int user_store_password_matches(const char *username,
                               const char *password);

/* Process CHANGE_PASSWORD current_password new_password. */
int user_store_change_password(const char *username,
                               const char *command,
                               char *response,
                               size_t response_capacity);

/* Add credentials for a new registry user. */
int user_store_add_password(const char *username, const char *password);
int user_store_reset_password(const char *username, const char *new_password);
int user_store_remove_password(const char *username);

/* Append a customer's feedback safely to feedback.log. */
int user_store_append_feedback(const char *username,
                               const char *account_id,
                               const char *feedback,
                               int semaphore_id,
                               char *response,
                               size_t response_capacity);


/* Manager: read the latest customer feedback entries. */
int user_store_read_feedback(int semaphore_id,
                             char *response,
                             size_t response_capacity);

/* Manager: list up to eight feedback summaries starting at 1-based entry ID. */
int user_store_list_feedback(int semaphore_id, unsigned long start_id,
                             char *response, size_t response_capacity);
/* Manager: view one feedback log entry by its 1-based append-order ID. */
int user_store_view_feedback(int semaphore_id, unsigned long feedback_id,
                             char *response, size_t response_capacity);

#endif
