CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
CPPFLAGS = -Iinclude

SERVER_SOURCES = src/session_server.c src/bank_core.c src/loan_core.c \
                 src/user_store.c src/user_registry.c

DEMO_PROGRAMS = hello record_demo race_demo lock_demo lock_modes \
                deadlock_demo tcp_server tcp_client tcp_fork_server \
                tcp_fork_client tcp_frame_server tcp_frame_client \
                sem_demo txn_log_demo

TOOL_PROGRAMS = account_ops loan_ops passbook

.PHONY: all clean demos tools test-bin

all: bank_session_server session_client_ops

bank_session_server: $(SERVER_SOURCES) include/bank_core.h \
                     include/loan_core.h include/user_store.h \
                     include/user_registry.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SERVER_SOURCES) -o $@

session_client_ops: src/session_client_ops.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

session_client: src/session_client.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

account_ops: tools/account_ops.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

loan_ops: tools/loan_ops.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

passbook: tools/passbook.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

bank_core_test: tests/bank_core_test.c src/bank_core.c include/bank_core.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/bank_core_test.c src/bank_core.c -o $@

$(DEMO_PROGRAMS): %: demos/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

demos: $(DEMO_PROGRAMS)

tools: $(TOOL_PROGRAMS)

test-bin: bank_core_test

clean:
	rm -f bank_session_server session_client_ops session_client
	rm -f $(DEMO_PROGRAMS) $(TOOL_PROGRAMS) bank_core_test
	rm -f *.o
