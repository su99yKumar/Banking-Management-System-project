# Banking Management System

CS-513 System Software Mini Project — IIIT Bangalore

## Features
- TCP client/server communication
- Concurrent clients using fork()
- Customer balance, deposits, withdrawals and transfers
- fcntl file locking and System V semaphores
- Transaction logging and history
- Loan applications and employee assignment
- Customer, employee, manager and admin roles
- Account activation/deactivation and feedback management

## Build
Run make to build the project. If needed, compile the server with:

gcc -std=c11 -Wall -Wextra -Wpedantic -g session_server.c bank_core.c loan_core.c user_store.c user_registry.c -o bank_session_server

Compile the client with:

gcc -std=c11 -Wall -Wextra -Wpedantic -g session_client_ops.c -o session_client_ops

## Technical limitations
- Concurrency uses fork(), not pthreads.
- Updates spanning multiple files are not fully crash-atomic.
- Customer closure deactivates accounts to preserve transaction history.
