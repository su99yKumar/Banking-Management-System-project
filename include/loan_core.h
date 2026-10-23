
#ifndef LOAN_CORE_H
#define LOAN_CORE_H

#include <stddef.h>

/*
 * Process a loan command.
 *
 * Customer commands:
 *   APPLY_LOAN amount term_months
 *   LOAN_STATUS
 *
 * Manager commands:
 *   APPROVE_LOAN loan_id approved_amount
 *   REJECT_LOAN loan_id
 *
 * Authorization must be enforced by the calling server.
 * The account ID is supplied by the server, not trusted from the command.
 */
int loan_process_command(const char *file_path,
                         const char *account_id,
                         const char *command,
                         char *response,
                         size_t response_capacity);

#endif

