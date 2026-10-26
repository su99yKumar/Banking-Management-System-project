# Banking Management System

CS-513 System Software Mini Project — IIIT Bangalore

A C/POSIX file-backed banking simulation with a TCP server, pthread client-session workers, role-based commands, account transactions, loans, locking and persistent transaction logging.

## Essential project files

- `src/` — server, full command client, banking/loan/user modules
- `include/` — interfaces for core modules
- `tests/` — unit test, fixture and isolated integration smoke suite
- `data/examples/` — safe sample account records for a local run
- `docs/architecture.md` — architecture/component blueprint and conceptual domain model
- `docs/test-report.md` — test scope, evidence and known limitations

Standalone demos and utility executables are not required by the banking application and are excluded from the final project commit.

## Build and tests

```bash
make clean
make
make test
make test-integration
```

`make test` runs the core test against a temporary copy of its fixture. `make test-integration` launches an isolated server using temporary runtime files and checks the main customer, employee, manager and administrator workflows. The integration suite uses `127.0.0.1:5053`; stop any other running server before starting it. The test data is discarded at the end.

## Run locally

From the repository root, create sample runtime data only for a new local demo:

```bash
cp data/examples/users_network_test.example.txt users_network_test.txt
./bank_session_server
```

In another terminal, from the same directory:

```bash
./session_client_ops
```

The server uses relative paths. Keep runtime credentials, user registry, account data, loan records, feedback and transaction logs local; do not commit those files. The sample account records are not live user data.

## Synchronization design

TCP connections are handled by pthread workers. The shared-operation pthread mutex serializes shared-state command processing. Record locks protect file records, while a System V semaphore coordinates shared transaction-log operations. File operations use POSIX/Linux system calls including `open`, `pread`, `pwrite`, `fcntl`, `fsync` and `close`.


