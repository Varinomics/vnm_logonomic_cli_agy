"""Measure the installed, unmanaged Agy status callback on a disposable Windows profile."""

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


class JobAccounting(ctypes.Structure):
    _fields_ = [
        ("user", ctypes.c_int64), ("kernel", ctypes.c_int64),
        ("period_user", ctypes.c_int64), ("period_kernel", ctypes.c_int64),
        ("page_faults", wintypes.DWORD), ("total_processes", wintypes.DWORD),
        ("active_processes", wintypes.DWORD), ("terminated_by_limit", wintypes.DWORD),
    ]


def job_counter():
    # Put only this benchmark process and its future children in a fresh job.
    # Counts include short-lived descendants that process-list polling can miss.
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    api.CreateJobObjectW.restype = wintypes.HANDLE
    api.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    api.QueryInformationJobObject.argtypes = [
        wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]
    job = api.CreateJobObjectW(None, None)
    if not job or not api.AssignProcessToJobObject(job, wintypes.HANDLE(-1)):
        raise ctypes.WinError(ctypes.get_last_error())

    def read():
        values = JobAccounting()
        # JobObjectBasicAccountingInformation is class 1. Times are 100 ns ticks.
        if not api.QueryInformationJobObject(job, 1, ctypes.byref(values), ctypes.sizeof(values), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return values.total_processes, (values.user + values.kernel) / 10000

    return read  # The OS releases the handle when this standalone benchmark ends.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("provider_tests", type=Path)
    parser.add_argument("package", type=Path)
    parser.add_argument("--samples", type=int, default=12)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("run this fixture on native Windows")
    if not 1 <= args.samples <= 32:
        parser.error("sample count must be between 1 and 32")

    provider, package = args.provider_tests.resolve(), args.package.resolve()
    payload = '{"conversation_id":"p16", "agent_state":"working", "text":"café"}\r\n'.encode()
    with tempfile.TemporaryDirectory(prefix="agy unmanaged ") as temporary:
        root = Path(temporary)
        env = {key: value for key, value in os.environ.items()
               if not key.upper().startswith(("LOGONOMIC_", "AGY_LOGONOMIC_"))}
        env.update(USERPROFILE=temporary, HOME=temporary)
        settings = root / ".gemini" / "antigravity-cli" / "settings.json"
        settings.parent.mkdir(parents=True)

        def install(source):
            result = subprocess.run([str(provider), "--installed-status-line-settings", str(package)],
                                    input=json.dumps(source).encode(), capture_output=True,
                                    env=env, cwd=root, check=True, timeout=30)
            settings.write_bytes(result.stdout)
            return json.loads(result.stdout)

        def callback(command):
            return subprocess.run(command, shell=True, input=payload, capture_output=True,
                                  cwd=root, env=env, timeout=10)

        installed = install({})
        wrapped = installed["statusLine"]["command"]
        if not wrapped:
            raise RuntimeError("packaged transform did not install a callback")
        if installed["logonomic-statusline-relay"]["original"] is not None:
            raise RuntimeError("unmanaged empty-command baseline has a retained original")
        count = job_counter()
        rows = []
        for sample in range(args.samples):
            # The built-in display has no external command to execute. An empty
            # cmd /c invocation would add a shell that the baseline never starts.
            for name, command in (("unwrapped-no-command", None), ("installed", wrapped)):
                starts, cpu = count()
                start = time.perf_counter_ns()
                result = callback(command) if command else None
                elapsed = time.perf_counter_ns() - start
                end_starts, end_cpu = count()
                if result is not None and (result.returncode or result.stdout):
                    raise RuntimeError(f"unexpected empty status result: {result!r}")
                rows.append({"sample": sample + 1, "path": name, "elapsed_ms": elapsed / 1e6,
                             "process_starts": end_starts - starts, "job_cpu_ms_including_driver": end_cpu - cpu,
                             "stderr_bytes": len(result.stderr) if result is not None else 0})

        # Replace the installed command as a user would. Exercise the owning
        # transform's adoption path before executing the resulting callback.
        probe = root / "user's status & relay.py"
        probe.write_text("import sys\nsys.stdout.buffer.write(sys.stdin.buffer.read())\nsys.exit(37)\n",
                         encoding="utf-8")
        command = subprocess.list2cmdline([sys.executable, str(probe)])
        installed["statusLine"]["command"] = command
        repaired = install(installed)
        repaired_command = repaired["statusLine"]["command"]
        relay = repaired["logonomic-statusline-relay"]
        if not repaired_command or repaired_command == command or repaired_command != relay["command"]:
            raise RuntimeError("replacement bypassed the installed Windows wrapper or disagrees with its relay")
        original = json.loads(relay["original"])
        if original["command"] != command:
            raise RuntimeError("packaged transform did not retain the replacement command")
        for candidate in (command, repaired_command):
            result = callback(candidate)
            if result.returncode != 37 or result.stdout != payload:
                raise RuntimeError(f"unexpected replacement status result: {result!r}")
        print(json.dumps({"windows_version": str(sys.getwindowsversion()), "samples": rows,
                          "replacement_stdio_exit": "passed"}, indent=2))


if __name__ == "__main__":
    main()
