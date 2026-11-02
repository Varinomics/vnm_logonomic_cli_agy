"""Measure the installed Linux callback and shipping vnmctl against scripted local-control replies."""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time


def read_exact(connection, count):
    data = bytearray()
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        if not chunk:
            raise RuntimeError("local-control peer closed an incomplete frame")
        data.extend(chunk)
    return data


def client_processes(connection, core):
    # The core is waiting for our reply, so it and its calling processes are
    # still alive. Do not time subprocesses under a trace harness.
    credentials = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
    pid, uid, _ = struct.unpack("3i", credentials)
    if uid != os.getuid() or not (Path(f"/proc/{pid}/exe")).samefile(core):
        raise RuntimeError("connection did not come from the specified wrapper's private core")
    processes = {}
    while pid != os.getpid():
        if pid <= 1 or pid in processes:
            raise RuntimeError("core is not a descendant of the benchmark")
        directory = Path("/proc") / str(pid)
        processes[pid] = (directory / "exe").resolve(strict=True).name
        status = (directory / "status").read_text().splitlines()
        pid = int(next(line for line in status if line.startswith("PPid:")).split()[1])
    return processes


def run_callback(command, environment, level):
    payload = json.dumps({"conversation_id": "p16", "agent_state": "working",
                          "tool_confirmation_pending": level}).encode()
    start = time.perf_counter_ns()
    result = subprocess.run(["/bin/sh", "-c", command], input=payload, env=environment,
                            cwd=environment["HOME"], capture_output=True, timeout=8)
    elapsed_ms = (time.perf_counter_ns() - start) / 1e6
    if result.returncode or result.stdout or result.stderr:
        raise RuntimeError(f"empty status command failed (exit {result.returncode}, "
                           f"{len(result.stdout)} stdout bytes, {len(result.stderr)} stderr bytes)")
    return elapsed_ms


def serve_callback(server, core, env, seen_keys, level, admitted):
    actions = ["deliver", "result", "acknowledge"] if admitted else ["deliver", "acknowledge"]
    processes = {}
    received = []
    key = None
    committed = {"accepted": True, "epoch": 1}
    for action in actions:
        connection, _ = server.accept()
        with connection:
            connection.settimeout(2)
            processes.update(client_processes(connection, core))
            size, = struct.unpack("!I", read_exact(connection, 4))
            if not 0 < size <= 1024 * 1024:
                raise RuntimeError("invalid control frame size")
            request = json.loads(read_exact(connection, size))
            received.append(time.perf_counter_ns())
            args = request["arguments"]
            if (request["command"] != "call" or request["name"] != action or
                    request["target"]["provider_class"] != "logonomic.provider_event" or
                    request["token"] != env["VNM_CONTROL_TOKEN"] or
                    args["session_capability"] != env["LOGONOMIC_SESSION_CAPABILITY"]):
                raise RuntimeError(f"unexpected or unauthenticated {action} request")
            if action == "deliver":
                key = args["idempotency_key"]
                expected_event = {"name": "StatusLine", "body": {
                    "conversationId": "p16", "toolConfirmationPending": level}}
                if (not key or key in seen_keys or args["event"] != expected_event or
                        args["generation_nonce"] != env["LOGONOMIC_PROVIDER_GENERATION_NONCE"] or
                        args["generation_digest"] != env["LOGONOMIC_PROVIDER_GENERATION_DIGEST"] or
                        args["package_id"] != env["LOGONOMIC_PROVIDER_PACKAGE_ID"]):
                    raise RuntimeError("unexpected callback identity, binding or sanitized event")
                seen_keys.add(key)
                payload = {"accepted": True, "kind": "provider_event_admitted"} if admitted else {
                    "accepted": True, "kind": "provider_event_committed", "outcome": committed}
            else:
                if args["idempotency_key"] != key:
                    raise RuntimeError(f"{action} did not use this delivery's key")
                payload = {"status": "committed", "outcome": committed} if action == "result" else {
                    "status": "acknowledged"}
            response = json.dumps({"ok": True, "payload": payload}).encode()
            connection.sendall(struct.pack("!I", len(response)) + response)
    return {"ipc_actions": actions, "observed_processes": dict(Counter(processes.values())),
            "deliver_to_result_ms": (received[1] - received[0]) / 1e6 if admitted else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("provider_tests", type=Path)
    parser.add_argument("package", type=Path)
    parser.add_argument("vnmctl", type=Path, help="production wrapper beside its logonomic_vnmctl_core")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("run on Linux with /proc and Unix-domain sockets available")
    provider, package, wrapper = [p.resolve(strict=True) for p in (args.provider_tests, args.package, args.vnmctl)]
    core = wrapper.with_name("logonomic_vnmctl_core").resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="agy-transport-") as temp, socket.socket(socket.AF_UNIX) as server:
        root = Path(temp)
        endpoint = str(root / "control.sock")
        server.bind(endpoint)
        server.listen(1)
        server.settimeout(5)
        nonce = secrets.token_hex(16)
        env = {"PATH": os.defpath, "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH", ""), "HOME": temp,
               "TMPDIR": temp, "VNM_CONTROL_ENDPOINT": endpoint, "VNM_CONTROL_TOKEN": secrets.token_hex(32),
               "LOGONOMIC_SESSION_CAPABILITY": secrets.token_hex(32), "LOGONOMIC_VNMCTL_PATH": str(wrapper),
               "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.agy", "LOGONOMIC_PROVIDER_GENERATION_NONCE": nonce,
               "LOGONOMIC_PROVIDER_GENERATION_DIGEST": secrets.token_hex(32), "LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED": nonce,
               "LOGONOMIC_PROVIDER_HELPERS_DIR": str(package / "helpers"), "AGY_LOGONOMIC_HOOKS_ENABLED": "1",
               "AGY_LOGONOMIC_LIFECYCLE_ENABLED": "1"}
        transformed = subprocess.check_output([provider, "--installed-status-line-settings", package],
                                              input=b"{}", cwd=root, env=env, timeout=30)
        settings = root / ".gemini" / "antigravity-cli" / "settings.json"
        settings.parent.mkdir(parents=True)
        settings.write_bytes(transformed)
        command = json.loads(transformed)["statusLine"]["command"]
        rows = []
        seen_keys = set()
        with ThreadPoolExecutor(1) as executor:
            for admitted in (True, False):
                # Both series use fresh keys. The second forces the response a
                # cached replay could see; it does not reproduce a real replay.
                for level in (False, False, False, False, True, False, True, False):
                    start = time.monotonic()
                    future = executor.submit(run_callback, command, env, level)
                    sample = serve_callback(server, core, env, seen_keys, level, admitted)
                    rows.append({"path": "admitted" if admitted else "immediately-committed-control",
                                 "pending": level, "callback_ms": future.result(timeout=9), **sample})
                    time.sleep(max(0, 0.300 - (time.monotonic() - start)))
        print(json.dumps({"owner": "scripted Linux socket, no host journal or permission state",
                          "requested_interval_ms": 300, "unique_keys": len(seen_keys), "samples": rows}, indent=2))


if __name__ == "__main__":
    main()
