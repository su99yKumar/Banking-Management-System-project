.DEFAULT_GOAL := all
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g

.PHONY: all clean

# Build the complete banking system and standalone demonstrations.
all: bank_session_server session_client_ops account_ops loan_ops passbook \
     bank_core_test hello record_demo race_demo lock_demo lock_modes \
     deadlock_demo tcp_server tcp_client tcp_fork_server tcp_fork_client \
     tcp_frame_server tcp_frame_client sem_demo txn_log_demo

# Main server: customer banking, loans, employee/manager/admin roles.
bank_session_server: session_server.c bank_core.c loan_core.c user_store.c user_registry.c bank_core.h loan_core.h user_store.h user_registry.h
	$(CC) $(CFLAGS) session_server.c bank_core.c loan_core.c user_store.c user_registry.c -o $@
