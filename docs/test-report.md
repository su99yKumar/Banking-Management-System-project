# Banking Management System — Test Report

Environment: WSL Ubuntu, GCC, C/POSIX sockets and file APIs. ACID crash recovery is explicitly excluded from this final smoke-test pass.

## Previously observed manual integration results

| Area | Evidence recorded during manual testing |
|---|---|
| Login/session rules | Customer and employee logins worked; duplicate active login for the same user was rejected. |
| Balance/deposit/withdraw | Successful operations returned expected balances; excessive withdrawals were rejected without balance reduction. |
| Transfers | Transfers in both directions succeeded; self-transfer and insufficient-funds cases were rejected. |
| Transaction history | Matching transfer-out and transfer-in entries were displayed in log/history. |
| Role restrictions | Employee was denied customer-only balance access and admin-only user listing. |
| Loan workflow | Loan application, manager assignment, employee assigned-loan listing, approval and customer status verification succeeded; rejection was also previously exercised. |
| Concurrent clients | Alice and Bob held concurrent sessions and separate client processes submitted deposits with matching balances/log records. State-changing commands are serialized by the global mutex. |
| Restart persistence | Balances were observed unchanged after restarting the server in the working copy. |

## Reproducible checks

- `make test`: runs `tests/bank_core_test.c` against a disposable copy of `tests/fixtures/users_core_test.txt`; this avoids mutating the tracked fixture.
- `make test-integration`: starts the server against a temporary copy of `data/examples/users_network_test.example.txt` and runs role-based smoke tests through `session_client_ops`. It reports the individual checks and exits nonzero if any check fails.

The finalization script only commits/pushes if the build, unit test, integration smoke tests and whitespace checks all succeed.

## Coverage targeted by integration smoke suite

Customer balance, deposit, withdrawal, transfer, self-transfer and insufficient-funds rejection, transaction history, loan application/status, feedback and password change; manager loan assignment, account activate/deactivate, feedback review and authorization; employee assigned loans, loan approval/rejection, passbook/account lookup, customer creation/modification/deactivation and authorization; administrator employee creation/modification/details/password reset/deletion, role management, customer modification and user listing; concurrent client requests and unique transaction IDs.

## Known limitation

The test suite covers normal operation paths and selected error/authorization cases. It does not prove crash recovery or full ACID atomicity for a transfer that is interrupted between account-file and transaction-log updates.
