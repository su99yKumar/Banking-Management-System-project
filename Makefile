CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -pthread
CPPFLAGS = -Iinclude

SERVER_SOURCES = src/session_server.c src/bank_core.c src/loan_core.c \
                 src/user_store.c src/user_registry.c

.PHONY: all clean test test-integration

all: bank_session_server session_client_ops

bank_session_server: $(SERVER_SOURCES) include/bank_core.h \
                     include/loan_core.h include/user_store.h \
                     include/user_registry.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SERVER_SOURCES) -o $@

session_client_ops: src/session_client_ops.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

bank_core_test: tests/bank_core_test.c src/bank_core.c include/bank_core.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/bank_core_test.c src/bank_core.c -o $@

test: bank_core_test tests/fixtures/users_core_test.txt
	@tmpdir=$$(mktemp -d); \
	trap 'rm -rf "$$tmpdir"' EXIT; \
	cp tests/fixtures/users_core_test.txt "$$tmpdir/users_core_test.txt"; \
	(cd "$$tmpdir" && "$(CURDIR)/bank_core_test")

test-integration: all
	python3 tests/integration_smoke.py

clean:
	rm -f bank_session_server session_client_ops bank_core_test *.o
