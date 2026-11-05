import ctypes
import http.server
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time

bridge = sys.argv[1]
console_driver = len(sys.argv) > 2 and sys.argv[2] == '--console-driver'
wait_fixture = sys.argv[2] if len(sys.argv) > 2 and not console_driver else None
identity = 'b22c8da5-1345-442f-906d-f63a1e1d552a'
other_identity = 'ec33ebf9-0cba-4100-8142-c61503f6c587'
literal = 'space $() ; " literal'


def readline(process):
    # A broken helper must fail this test instead of leaving it blocked in a
    # fixture handshake. The native helper itself has no additional thread.
    ready = threading.Event()
    result = []
    def read():
        result.append(process.stdout.readline())
        ready.set()
    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    assert ready.wait(5), 'native fixture did not reach its handshake'
    reader.join()
    assert result[0], 'native fixture exited before its handshake'
    return result[0]


def send(process, command):
    process.stdin.write(command.encode() + b'\n')
    process.stdin.flush()


def trace_calls(path):
    if not path.exists():
        return []
    # A live writer may be between the final write and its newline.
    return [line.split() for line in path.read_text().splitlines() if len(line.split()) == 6]


def await_trace(path, predicate):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        calls = trace_calls(path)
        if predicate(calls):
            return calls
        assert not any(call[0] == 'nanosleep' and int(call[2]) == 40000000 for call in calls), \
            'the bridge still uses its lifetime 40 ms sleep instead of a native wait'
        time.sleep(.005)
    raise AssertionError(('native wait handshake missing', trace_calls(path)))


def terminate_custody(process, child):
    if os.name != 'nt':
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        return
    if child is None:
        process.kill()
        return
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [w.LPVOID, w.LPCWSTR]
    kernel.CreateJobObjectW.restype = w.HANDLE
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
    kernel.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    job = kernel.CreateJobObjectW(None, None)
    native_handle = kernel.OpenProcess(0x0101, False, child['child_pid'])
    assert job and native_handle
    try:
        assert kernel.AssignProcessToJobObject(job, int(process._handle))
        assert kernel.AssignProcessToJobObject(job, native_handle)
        assert kernel.TerminateJobObject(job, 3)
    finally:
        kernel.CloseHandle(native_handle)
        kernel.CloseHandle(job)


with tempfile.TemporaryDirectory(prefix='agy-resume-bridge-') as temporary:
    root = Path(temporary)
    native = root / 'native child.py'
    native.write_text(r'''import json, os, pathlib, signal, subprocess, sys, time
sys.stdout.reconfigure(newline='\n')
sys.stderr.reconfigure(newline='\n')
mode, identity, literal = sys.argv[1:4]
assert sys.argv[4] == '--log-file'
log = pathlib.Path(sys.argv[5])
if mode == 'missing': sys.exit(0)
if mode == 'no-logger': time.sleep(60)
if mode == 'console-break':
    def on_break(signum, frame):
        print('native-ctrl-break', flush=True)
        sys.exit(7)
    signal.signal(signal.SIGBREAK, on_break)
output = log.open('wb', buffering=0)
if os.name != 'nt':
    # Opening /proc/self/fd/N or /dev/fd/N duplicates the inherited writer. Only
    # the opened file remains, so closing it really produces logger EOF.
    os.close(int(log.name))
    gate = os.environ.get('AGY_TEST_WAIT_GATE_FD')
    if gate: os.close(int(gate))
print(json.dumps({'child_pid': os.getpid(), 'log_path': str(log)}), flush=True)
print('native stdout', flush=True)
print('native stderr', file=sys.stderr, flush=True)
stream = f'I0929 07:51:02.417194     171 conversation_manager.go:887] Streaming conversation {identity}\n'.encode()
redraw = f'I0929 07:51:02.478085     362 manager.go:932] Full redraw completed (rerenderAll) for conversation {identity} (epoch 0, items 4)\n'.encode()
remaining = stream
if mode == 'selector':
    output.write(f'I0929 07:51:02.417194       1 common.go:401] Resuming conversation {identity}\n'.encode())
    sys.exit(0)
if mode == 'early-eof':
    output.close()
    time.sleep(60)
if mode == 'timeout': time.sleep(60)
if mode == 'malformed': output.write(b'broken\0native record\n')
elif mode == 'oversized-record': output.write(b'x' * (16 * 1024 + 1) + b'\n')
elif mode == 'overflow': output.write(b'x' * (1024 * 1024 + 1))
elif mode != 'bursts': output.write(stream + redraw + b'private text must not leave the child log\n')
if mode in ['malformed', 'oversized-record', 'overflow']: time.sleep(60)
if mode == 'hold':
    while True:
        output.write(b'private later log text ' + b'x' * 2048 + b'\n')
        time.sleep(.001)
if mode == 'console-break':
    while True: time.sleep(.01)
for message in sys.stdin:
    command = message.rstrip('\n')
    if command == 'partial':
        output.write(stream[:len(stream)//2])
        remaining = stream[len(stream)//2:]
        print('partial-written', flush=True)
    elif command == 'proof':
        output.write(remaining + b'private startup text\n' + redraw)
        print('proof-written', flush=True)
    elif command == 'burst':
        for _ in range(32): output.write(b'private later text ' + b'x' * 65536)
        print('burst-written', flush=True)
    elif command == 'close':
        output.close()
        print('closed', flush=True)
    elif command == 'descendant':
        os.set_inheritable(output.fileno(), True)
        arguments = [sys.executable, '-c', 'import time; time.sleep(60)']
        options = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if os.name == 'nt':
            # The descendant must retain only the logger, never the bridge's
            # captured terminal handles, or communicate() would test the fixture.
            for descriptor in [0, 1, 2]: os.set_inheritable(descriptor, False)
            options['close_fds'] = False
        else: options['pass_fds'] = (output.fileno(),)
        descendant = subprocess.Popen(arguments, **options)
        print(json.dumps({'descendant_pid': descendant.pid}), flush=True)
        sys.exit(7)
    elif command == 'signal':
        signal.signal(signal.SIGTERM, signal.SIG_DFL)
        os.kill(os.getpid(), signal.SIGTERM)
    else:
        print(json.dumps({'literal': literal, 'message': command}), flush=True)
        sys.exit(7 if mode == 'exit7' or command == 'exit7' else 0)
''', encoding='utf-8')

    def run_case(executable, mode, instrumented=False):
        observations, errors = [], []
        event = threading.Event()
        authorization = 'Bearer ' + 'a' * 64

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def do_POST(self):
                try:
                    assert self.path == '/events'
                    assert self.headers['Authorization'] == authorization
                    body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    observations.append(body)
                    self.send_response(422 if mode == 'mismatch' else 200)
                    self.send_header('Content-Length', '0')
                    self.send_header('Connection', 'close')
                    self.end_headers()
                except Exception as error:
                    errors.append(error)
                finally:
                    event.set()

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        offer = {'version': 1, 'mechanism': 'http-json-post', 'confirmation': 'observed-identity',
                 'endpoint': f'http://127.0.0.1:{server.server_port}/events', 'authorization': authorization}
        environment = dict(os.environ, AGY_LOGONOMIC_STARTUP_OBSERVER=json.dumps(offer))
        trace = root / (mode + '.trace')
        extra = {}
        gate_reader = gate_writer = None
        if instrumented:
            environment['AGY_TEST_NATIVE_TRACE'] = str(trace)
            if mode == 'wait-exit-race':
                gate_reader, gate_writer = os.pipe()
                environment['AGY_TEST_WAIT_GATE_FD'] = str(gate_reader)
                extra['pass_fds'] = (gate_reader,)
        started = time.monotonic()
        process = subprocess.Popen([executable, sys.executable, str(native), 'bursts' if instrumented else mode,
                                    other_identity if mode == 'mismatch' else identity, literal],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   bufsize=0, env=environment, start_new_session=os.name != 'nt',
                                   creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if mode == 'console-break' else 0,
                                   **extra)
        if gate_reader is not None:
            os.close(gate_reader)
        child = descendant = None
        captured = b''
        expected = 3
        confirmed = mode in ['success', 'exit7', 'mismatch', 'hold', 'bursts', 'eof', 'descendant',
                             'signaled', 'console-break'] or instrumented
        try:
            if confirmed and mode != 'mismatch':
                line = readline(process)
                child = json.loads(line)
                captured += line + readline(process)
                if os.name == 'nt':
                    assert child['log_path'].startswith('\\\\.\\pipe\\')
                else:
                    assert child['log_path'].startswith(('/proc/self/fd/', '/dev/fd/'))
                if instrumented:
                    await_trace(trace, lambda calls: any(call[0] == 'poll' and 0 < int(call[2]) <= 10000 for call in calls))
                if mode == 'bursts':
                    send(process, 'partial')
                    assert readline(process) == b'partial-written\n'
                    assert not event.is_set(), 'partial native record was treated as identity proof'
                if mode == 'bursts' or instrumented:
                    send(process, 'proof')
                    assert readline(process) == b'proof-written\n'
            if confirmed:
                assert event.wait(5), (mode, 'no native startup observation')
            if instrumented:
                await_trace(trace, lambda calls: any(call[0] == 'poll' and int(call[2]) == -1 for call in calls))
                if mode in ['wait-eof', 'wait-exit-race']:
                    send(process, 'close')
                    assert readline(process) == b'closed\n'
                    calls = await_trace(trace, lambda calls: any(call[0] == 'read' and int(call[2]) == 65536 and int(call[3]) == 0 for call in calls))
                    eof = next(call for call in calls if call[0] == 'read' and int(call[2]) == 65536 and int(call[3]) == 0)
                    await_trace(trace, lambda calls: any(call[0] == 'poll' and int(call[1]) > int(eof[1]) and
                        int(call[2]) == -1 and eof[4] not in call[4:6] for call in calls))
                    assert process.poll() is None, 'logger EOF ended the original child wait'
                if mode == 'wait-exit-race':
                    await_trace(trace, lambda calls: any(call[0] == 'gate' for call in calls))
                    send(process, 'exit7')
                    # Logger EOF is already retired, so only the retained
                    # child notification can release the real poll. WNOWAIT
                    # leaves the helper to reap the original exit status.
                    os.write(gate_writer, b'x')
                    await_trace(trace, lambda calls: any(call[0] == 'exit_before_poll' and
                        int(call[2]) == child['child_pid'] and int(call[3]) == 0 and
                        int(call[4]) == child['child_pid'] and int(call[5]) == 7 for call in calls))
                elif mode == 'wait-signal':
                    os.kill(process.pid, signal.SIGCHLD)
                    await_trace(trace, lambda calls: sum(call[0] == 'poll' and int(call[2]) == -1 for call in calls) >= 2)
                    assert process.poll() is None, 'an unrelated child notification ended the original child'
                if mode != 'wait-exit-race':
                    send(process, 'exit7')
                expected = 7
                stdout, stderr = process.communicate(timeout=5)
                calls = trace_calls(trace)
                first_wait = next(index for index, call in enumerate(calls) if call[0] == 'poll' and int(call[2]) == -1)
                assert all(int(call[2]) == -1 for call in calls[first_wait:] if call[0] == 'poll'), calls
                assert not any(call[0] == 'nanosleep' for call in calls), calls
                if mode in ['wait-eof', 'wait-exit-race']:
                    assert sum(call[0] == 'read' and call[4] == eof[4] and int(call[3]) == 0 for call in calls) == 1, calls
            elif mode == 'hold':
                # Keep the native writer active after accepted identity, then
                # exercise external custody termination and kernel pipe cleanup.
                time.sleep(.2)
                terminate_custody(process, child)
                stdout, stderr = process.communicate(timeout=5)
                expected = 3 if os.name == 'nt' else -signal.SIGKILL
                if os.name == 'nt':
                    from ctypes import wintypes as w
                    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                    kernel.WaitNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD]
                    assert not kernel.WaitNamedPipeW(child['log_path'], 0) and ctypes.get_last_error() == 2
            elif mode == 'console-break':
                from ctypes import wintypes as w
                kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel.GenerateConsoleCtrlEvent.argtypes = [w.DWORD, w.DWORD]
                assert kernel.GenerateConsoleCtrlEvent(1, process.pid), ctypes.get_last_error()
                stdout, stderr = process.communicate(timeout=5)
                expected = 7
                assert b'native-ctrl-break\n' in stdout
            elif mode in ['bursts', 'eof', 'descendant', 'signaled']:
                if mode == 'bursts':
                    for _ in range(2):
                        send(process, 'burst')
                        assert readline(process) == b'burst-written\n'
                if mode == 'eof':
                    send(process, 'close')
                    assert readline(process) == b'closed\n'
                    assert process.poll() is None
                if mode == 'descendant':
                    send(process, 'descendant')
                    descendant = json.loads(readline(process))['descendant_pid']
                elif mode == 'signaled':
                    send(process, 'signal')
                else:
                    send(process, 'exit7')
                expected = 128 + signal.SIGTERM if mode == 'signaled' else 7
                stdout, stderr = process.communicate(timeout=5)
            else:
                stdout, stderr = process.communicate(b'own terminal message\n', timeout=15)
                if mode in ['success', 'exit7']:
                    expected = 7 if mode == 'exit7' else 0
                    assert json.loads(stdout.splitlines()[-1]) == {'literal': literal, 'message': 'own terminal message'}
                if mode in ['timeout', 'no-logger']:
                    assert time.monotonic() - started >= 9.5, 'startup failed before its ten-second deadline'
            stdout = captured + stdout
            assert process.returncode == expected, (mode, process.returncode, expected, stderr)
            if confirmed and mode != 'mismatch':
                assert b'native stdout' in stdout and b'native stderr' in stderr
            assert b'private' not in stdout and b'private' not in stderr
            assert not errors, errors
            if confirmed:
                assert len(observations) == 1
                assert list(observations[0]) == ['native_records']
                assert 'Resuming' not in observations[0]['native_records']
                assert 'private' not in observations[0]['native_records']
            else:
                assert not observations, (mode, 'unproved identity was posted')
            print('PASS:', mode, 'native child/streams/argv/identity/waits/cleanup')
        finally:
            if gate_writer is not None:
                os.close(gate_writer)
            if process.poll() is None:
                terminate_custody(process, child)
                process.communicate(timeout=5)
            if descendant is not None:
                try:
                    os.kill(descendant, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            server.shutdown()
            server.server_close()
            worker.join()

    if console_driver:
        run_case(bridge, 'console-break')
    else:
        for mode in ['success', 'exit7', 'selector', 'missing', 'no-logger', 'overflow', 'malformed',
                     'oversized-record', 'early-eof', 'timeout', 'mismatch', 'bursts', 'eof', 'descendant', 'hold']:
            run_case(bridge, mode)
        if os.name != 'nt':
            run_case(bridge, 'signaled')
        # Each exit occurs at a different natural scheduling point after the POST.
        # The instrumented case below also forces the exact signal-before-poll gap.
        for _ in range(8):
            run_case(bridge, 'exit7')
        if wait_fixture:
            for mode in ['wait-silent', 'wait-eof', 'wait-exit-race', 'wait-signal']:
                run_case(wait_fixture, mode, instrumented=True)
        if os.name == 'nt':
            # A separate hidden console keeps the signal within this fixture's
            # process group and leaves the invoking terminal untouched.
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
            subprocess.run([sys.executable, __file__, bridge, '--console-driver'], check=True, timeout=15,
                           stdin=subprocess.DEVNULL, stdout=sys.stdout, stderr=sys.stderr,
                           startupinfo=startup, creationflags=subprocess.CREATE_NEW_CONSOLE)
