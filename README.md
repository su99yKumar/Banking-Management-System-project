# Banking Management System

CS-513 System Software Mini Project — IIIT Bangalore

A C-based banking management system demonstrating TCP sockets, fork-based concurrency, file locking, System V semaphores, transaction logging, role-based access and loan management.

## Project Structure
- `src/` — server, client and core modules
- `include/` — header files
- `demos/` — System Software concept demonstrations
- `tests/` — test programs
- `tools/` — standalone utilities
- `data/examples/` — sample account data

## Build
Run these commands from the repository root:

```bash
make
make demos
make tools
make test-bin
```

## Run the server
Create the local runtime account file, then start the server:

```bash
cp data/examples/users_network_test.example.txt users_network_test.txt
./bank_session_server
```

Open another terminal in the same repository directory and run:

```bash
./session_client_ops
```

## System Software Concepts
TCP sockets, fork-based concurrency, fcntl record locks, System V semaphores, race-condition prevention, deadlock handling and fsync.

## Known Limitations
- Concurrency uses processes via fork(), not pthreads.
- Operations spanning multiple files are not fully crash-atomic.
- Customer closure deactivates accounts to preserve transaction history.
- Runtime credentials, registries, session locks and transaction logs should not be committed.
