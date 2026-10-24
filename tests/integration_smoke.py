#!/usr/bin/env python3
"""Isolated end-to-end smoke tests for the threaded Banking Management System.
Uses a temporary runtime directory; does not touch the user's working account files.
"""
from __future__ import annotations
import concurrent.futures
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "bank_session_server"
CLIENT = ROOT / "session_client_ops"
EXAMPLE = ROOT / "data" / "examples" / "users_network_test.example.txt"
HOST, PORT = "127.0.0.1", 5053
failures: list[str] = []
passes = 0

def mark(label: str, ok: bool, detail: str = "") -> None:
    global passes
    print(f"{'PASS' if ok else 'FAIL'}  {label}" + (f" — {detail}" if detail else ""), flush=True)
    if ok:
        passes += 1
    else:
        failures.append(label)

def need(label: str, output: str, token: str) -> None:
    mark(label, token in output, f"expected {token!r}" if token not in output else "")

def can_bind_port() -> bool:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((HOST, PORT))
        return True
    except OSError:
        return False

def main() -> int:
    if not SERVER.is_file() or not CLIENT.is_file() or not EXAMPLE.is_file():
        print("Build first with `make`; required binaries or example account fixture are missing.", file=sys.stderr)
        return 2
    if not can_bind_port():
        print("Port 5053 is already in use. Stop any running bank_session_server with Ctrl+C, then rerun.", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="bank-feature-smoke-") as td:
        runtime = Path(td)
        shutil.copy2(EXAMPLE, runtime / "users_network_test.txt")
        server_log_path = runtime / "server.log"
        server_log = server_log_path.open("w+")
        server = subprocess.Popen([str(SERVER)], cwd=runtime, stdout=server_log,
                                  stderr=subprocess.STDOUT, text=True)

        def stop_server() -> None:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=3)
            server_log.flush()

        try:
            ready = False
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    break
                try:
                    with socket.create_connection((HOST, PORT), timeout=0.25):
                        ready = True
                        break
                except OSError:
                    time.sleep(0.1)
            if not ready:
                server_log.flush()
                server_log.seek(0)
                print("Server did not start. Startup log:\n" + server_log.read(), file=sys.stderr)
                return 2

            def client(username: str, password: str, role: str, commands: list[str], label: str) -> str:
                payload = "\n".join([username, password, *commands, "LOGOUT", ""])
                try:
                    p = subprocess.run([str(CLIENT)], input=payload, cwd=runtime,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, timeout=12)
                except subprocess.TimeoutExpired:
                    mark(label + " client completion", False, "client timed out")
                    return ""
                out = p.stdout or ""
                mark(label + " login", f"LOGIN_OK ROLE={role}" in out,
                     (f"expected successful role login; output={out[:300]!r}"
                      if f"LOGIN_OK ROLE={role}" not in out else ""))
                mark(label + " logout", "Server response: BYE" in out,
                     "BYE response missing" if "Server response: BYE" not in out else "")
                return out

            # Multiple client requests in parallel, using isolated disposable data.
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                fa = pool.submit(client, "alice", "Alice123!", "CUSTOMER",
                                 ["DEPOSIT 0.01", "BALANCE"], "Concurrent Alice")
                fb = pool.submit(client, "bob", "Bob123!", "CUSTOMER",
                                 ["DEPOSIT 0.01", "BALANCE"], "Concurrent Bob")
                concurrent_alice, concurrent_bob = fa.result(), fb.result()
            need("Concurrent Alice deposit", concurrent_alice, "DEPOSIT_OK")
            need("Concurrent Bob deposit", concurrent_bob, "DEPOSIT_OK")

            alice = client("alice", "Alice123!", "CUSTOMER", [
                "BALANCE", "DEPOSIT 1.00", "WITHDRAW 1.00",
                "TRANSFER 000002 1.00", "TRANSFER 000001 1.00",
                "WITHDRAW 999999.00", "TRANSACTION_HISTORY",
                "APPLY_LOAN 100.00 12", "LOAN_STATUS",
                "FEEDBACK smoke_test_feedback",
                "CHANGE_PASSWORD Alice123! TestAlice123!",
                "CHANGE_PASSWORD TestAlice123! Alice123!",
                "ASSIGN_LOAN 000001 employee",
            ], "Alice customer feature tests")
            for token, label in [
                ("BALANCE ", "customer balance"), ("DEPOSIT_OK", "customer deposit"),
                ("WITHDRAW_OK", "customer withdrawal"), ("TRANSFER_OK", "customer transfer"),
                ("ERROR CANNOT_TRANSFER_TO_SELF", "self-transfer rejection"),
                ("ERROR INSUFFICIENT_FUNDS", "insufficient-funds rejection"),
                ("TRANSACTION_HISTORY ACCOUNT=000001", "customer transaction history"),
                ("LOAN_APPLIED ID=", "customer loan application"), ("STATUS=PENDING", "pending loan status"),
                ("FEEDBACK_SUBMITTED", "customer feedback"), ("PASSWORD_CHANGED", "customer change password"),
                ("ERROR PERMISSION_DENIED", "customer denied manager command"),
            ]:
                need(label, alice, token)
            m1 = re.search(r"LOAN_APPLIED ID=(\d{6})", alice)
            if not m1:
                raise RuntimeError("Could not parse Alice's new loan ID; aborting isolated tests.")
            loan_approve = m1.group(1)

            bob = client("bob", "Bob123!", "CUSTOMER", [
                "BALANCE", "TRANSFER 000001 1.00", "WITHDRAW 999999.00",
                "APPLY_LOAN 200.00 12", "LOAN_STATUS",
            ], "Bob customer feature tests")
            for token, label in [("BALANCE ", "Bob balance"), ("TRANSFER_OK", "reverse transfer"),
                                 ("ERROR INSUFFICIENT_FUNDS", "Bob insufficient funds"),
                                 ("LOAN_APPLIED ID=", "Bob loan application")]:
                need(label, bob, token)
            m2 = re.search(r"LOAN_APPLIED ID=(\d{6})", bob)
            if not m2:
                raise RuntimeError("Could not parse Bob's new loan ID; aborting isolated tests.")
            loan_reject = m2.group(1)

            manager = client("manager", "Manager123!", "MANAGER", [
                f"ASSIGN_LOAN {loan_approve} employee", f"ASSIGN_LOAN {loan_reject} employee",
                "REVIEW_FEEDBACK", "LIST_FEEDBACK 1", "ADD_EMPLOYEE denied_user DeniedPass123!",
                "CHANGE_PASSWORD Manager123! TestManager123!",
                "CHANGE_PASSWORD TestManager123! Manager123!",
            ], "Manager feature tests")
            for token, label in [("LOAN_ASSIGNED", "manager loan assignment"),
                                 ("smoke_test_feedback", "manager review feedback"),
                                 ("ERROR PERMISSION_DENIED", "manager denied admin command"),
                                 ("PASSWORD_CHANGED", "manager change password")]:
                need(label, manager, token)

            employee = client("employee", "Employee123!", "EMPLOYEE", [
                "ASSIGNED_LOANS", f"APPROVE_LOAN {loan_approve} 100.00",
                f"REJECT_LOAN {loan_reject}", "PASSBOOK 000001", "LOOKUP_ACCOUNT 000001",
                "BALANCE", "ADD_CUSTOMER smoke_customer SmokeCust123! Smoke Customer",
                "LIST_USERS", "CHANGE_PASSWORD Employee123! TestEmployee123!",
                "CHANGE_PASSWORD TestEmployee123! Employee123!",
            ], "Employee feature tests")
            for token, label in [(f"LOAN={loan_approve}", "employee sees assigned loan"),
                                 ("LOAN_APPROVED", "employee approval"), ("LOAN_REJECTED", "employee rejection"),
                                 ("TRANSACTION_HISTORY ACCOUNT=000001", "employee passbook"),
                                 ("ACCOUNT 000001", "employee account lookup"),
                                 ("ERROR CUSTOMER_ONLY_COMMAND", "employee denied customer balance"),
                                 ("CUSTOMER_CREATED USER=smoke_customer ACCOUNT=", "employee adds customer"),
                                 ("ERROR PERMISSION_DENIED", "employee denied admin command"),
                                 ("PASSWORD_CHANGED", "employee change password")]:
                need(label, employee, token)
            mc = re.search(r"CUSTOMER_CREATED USER=smoke_customer ACCOUNT=(\d{6})", employee)
            if not mc:
                raise RuntimeError("Could not parse new customer account ID; aborting isolated tests.")
            customer_id = mc.group(1)

            # Keep status testing separate from Alice/Bob loan-status checks.
            manager_status = client("manager", "Manager123!", "MANAGER", [
                f"SET_ACCOUNT_STATUS {customer_id} D",
                f"SET_ACCOUNT_STATUS {customer_id} A",
            ], "Manager account status tests")
            need("manager deactivate test customer", manager_status,
                 f"ACCOUNT_STATUS_UPDATED {customer_id} D")
            need("manager reactivate test customer", manager_status,
                 f"ACCOUNT_STATUS_UPDATED {customer_id} A")
            status_customer = client("smoke_customer", "SmokeCust123!", "CUSTOMER",
                                     ["BALANCE"], "Customer active after reactivation")
            need("reactivated customer can access balance", status_customer, "BALANCE ")

            alice_status = client("alice", "Alice123!", "CUSTOMER", ["LOAN_STATUS"], "Alice loan status")
            bob_status = client("bob", "Bob123!", "CUSTOMER", ["LOAN_STATUS"], "Bob loan status")
            need("Customer sees approved loan ID", alice_status, f"ID={loan_approve}")
            need("Customer sees approved loan status", alice_status, "STATUS=APPROVED")
            need("Customer sees rejected loan ID", bob_status, f"ID={loan_reject}")
            need("Customer sees rejected loan status", bob_status, "STATUS=REJECTED")

            admin = client("admin", "Admin123!", "ADMIN", [
                "ADD_EMPLOYEE smoke_employee SmokeEmp123!",
                "ADD_EMPLOYEE smoke_role SmokeRole123!",
                "MODIFY_EMPLOYEE_DETAILS smoke_employee Smoke Test Employee",
                "VIEW_EMPLOYEE_DETAILS smoke_employee",
                "RESET_PASSWORD smoke_employee NewSmokeEmp123!",
                "SET_USER_ROLE smoke_role MANAGER", "LIST_USERS",
                f"MODIFY_CUSTOMER_NAME {customer_id} Admin Updated Customer",
                "CHANGE_PASSWORD Admin123! TestAdmin123!",
                "CHANGE_PASSWORD TestAdmin123! Admin123!",
            ], "Admin feature tests")
            for token, label in [("EMPLOYEE_CREATED USER=smoke_employee", "admin adds employee"),
                                 ("EMPLOYEE_DETAILS_UPDATED smoke_employee", "admin modifies employee details"),
                                 ("EMPLOYEE_DETAILS USER=smoke_employee NAME=Smoke Test Employee", "admin views employee details"),
                                 ("EMPLOYEE_PASSWORD_RESET smoke_employee", "admin resets password"),
                                 ("USER_ROLE_UPDATED smoke_role MANAGER", "admin changes user role"),
                                 ("USERS ", "admin lists users"), ("CUSTOMER_NAME_UPDATED", "admin modifies customer details"),
                                 ("PASSWORD_CHANGED", "admin changes own password")]:
                need(label, admin, token)

            role_check = client("smoke_role", "SmokeRole123!", "MANAGER", [
                "SET_ACCOUNT_STATUS 000002 D", "SET_ACCOUNT_STATUS 000002 A",
            ], "Role change takes effect")
            need("changed role permits manager operations", role_check,
                 "ACCOUNT_STATUS_UPDATED 000002 A")

            reset_user = client("smoke_employee", "NewSmokeEmp123!", "EMPLOYEE",
                                ["ASSIGNED_LOANS"], "Reset-password login test")
            need("reset password accepts new credential", reset_user, "ASSIGNED_LOANS")

            modified_customer = client("employee", "Employee123!", "EMPLOYEE", [
                f"MODIFY_CUSTOMER_NAME {customer_id} Employee Updated Customer",
                f"DELETE_CUSTOMER {customer_id}",
            ], "Employee modify/delete customer tests")
            need("employee modifies customer details", modified_customer, f"CUSTOMER_NAME_UPDATED {customer_id}")
            need("employee deactivates customer", modified_customer, f"CUSTOMER_DEACTIVATED {customer_id}")

            cleanup_admin = client("admin", "Admin123!", "ADMIN", [
                "DELETE_EMPLOYEE smoke_employee",
            ], "Admin delete employee test")
            need("admin deletes employee", cleanup_admin, "EMPLOYEE_DELETED smoke_employee")

            # Make sure the log contains parseable, unique transaction IDs.
            log_path = runtime / "transactions.log"
            ids: list[int] = []
            if log_path.exists():
                for line in log_path.read_text(errors="replace").splitlines():
                    parts = line.split()
                    if parts and parts[0].isdigit():
                        ids.append(int(parts[0]))
            mark("transaction log IDs are unique", bool(ids) and len(ids) == len(set(ids)),
                 "" if ids and len(ids) == len(set(ids)) else "missing or duplicate transaction IDs")

            stop_server()
        finally:
            stop_server()
            server_log.close()

    print(f"\nRESULT: {passes} checks passed; {len(failures)} failed.")
    if failures:
        print("Failed checks: " + ", ".join(failures))
        return 1
    print("All feature smoke checks passed (ACID crash recovery intentionally excluded).")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FATAL: {exc}", file=sys.stderr)
        raise SystemExit(2)
        raise
