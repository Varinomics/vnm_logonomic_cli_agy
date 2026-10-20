import http.server
import ctypes
import json
import os
from pathlib import Path
import subprocess
import signal
import sys
import tempfile
import threading
import time

bridge = sys.argv[1]
identity = 'b22c8da5-1345-442f-906d-f63a1e1d552a'
other_identity = 'ec33ebf9-0cba-4100-8142-c61503f6c587'

with tempfile.TemporaryDirectory(prefix='agy-resume-bridge-') as temporary:
    root = Path(temporary)
    native = root / 'native child.py'
    native.write_text('''import json, os, pathlib, sys, time
mode, identity, literal = sys.argv[1:4]
assert sys.argv[4] == '--log-file'
log = pathlib.Path(sys.argv[5])
stream = f'I0929 07:51:02.417194     171 conversation_manager.go:887] Streaming conversation {identity}\\n'
redraw = f'I0929 07:51:02.478085     362 manager.go:932] Full redraw completed (rerenderAll) for conversation {identity} (epoch 0, items 4)\\n'
if mode == 'selector':
    with log.open('a') as output: output.write(f'I0929 07:51:02.417194       1 common.go:401] Resuming conversation {identity}\\n')
    sys.exit(0)
if mode == 'missing': sys.exit(0)
output = log.open('a')
if mode == 'overflow': output.write('x' * (1024 * 1024 + 1))
else: output.write(stream + redraw + 'private text must not leave the child log\\n')
output.flush()
print(json.dumps({'child_pid': os.getpid(), 'log_path': str(log)}), flush=True)
print('native stdout', flush=True)
print('native stderr', file=sys.stderr, flush=True)
if mode == 'hold':
    while True:
        output.write('private later log text ' + 'x' * 2048 + '\\n')
        output.flush()
        time.sleep(.001)
message = sys.stdin.readline().rstrip('\\n')
print(json.dumps({'literal': literal, 'message': message}), flush=True)
sys.exit(7 if mode == 'exit7' else 0)
''', encoding='utf-8')
    for mode in ['success', 'exit7', 'selector', 'missing', 'overflow', 'mismatch', 'hold']:
        observations = []
        event = threading.Event()
        authorization = 'Bearer ' + 'a' * 64

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def do_POST(self):
                assert self.path == '/events'
                assert self.headers['Authorization'] == authorization
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                observations.append(body)
                self.send_response(422 if mode == 'mismatch' else 200)
                self.send_header('Content-Length', '0')
                self.end_headers()
                event.set()

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        offer = {'version': 1, 'mechanism': 'http-json-post', 'confirmation': 'observed-identity',
                 'endpoint': f'http://127.0.0.1:{server.server_port}/events', 'authorization': authorization}
        environment = dict(os.environ, AGY_LOGONOMIC_STARTUP_OBSERVER=json.dumps(offer))
        literal = 'space $() ; " literal'
        process = subprocess.Popen([bridge, sys.executable, str(native), mode,
                                    other_identity if mode == 'mismatch' else identity, literal],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=environment, start_new_session=os.name != 'nt')
        if mode in ['success', 'exit7', 'mismatch', 'hold']:
            assert event.wait(5), (mode, 'no native startup observation')
        if mode == 'hold':
            child = json.loads(process.stdout.readline())
            time.sleep(.2) # Native logging continues after identity confirmation.
            if os.name == 'nt':
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
                assert kernel.AssignProcessToJobObject(job, int(process._handle))
                assert kernel.AssignProcessToJobObject(job, native_handle)
                assert kernel.TerminateJobObject(job, 3)
                kernel.CloseHandle(native_handle)
                kernel.CloseHandle(job)
                kernel.WaitNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD]
                stdout, stderr = process.communicate(timeout=5)
                assert not kernel.WaitNamedPipeW(child['log_path'], 0) and ctypes.get_last_error() == 2
            else:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, stderr = process.communicate(timeout=5)
                assert process.returncode == -signal.SIGKILL
        else:
            stdout, stderr = process.communicate(b'own terminal message\n', timeout=15)
        server.shutdown()
        server.server_close()
        worker.join()
        if mode in ['success', 'exit7']:
            assert process.returncode == (7 if mode == 'exit7' else 0), (mode, process.returncode, stderr)
            assert b'native stdout' in stdout and b'native stderr' in stderr
            assert json.loads(stdout.splitlines()[-1]) == {'literal': literal, 'message': 'own terminal message'}
        elif mode != 'hold':
            assert process.returncode == 3, (mode, process.returncode, stderr)
        if mode in ['success', 'exit7', 'mismatch', 'hold']:
            assert len(observations) == 1
            assert list(observations[0]) == ['native_records']
            assert 'Resuming' not in observations[0]['native_records']
            assert 'private text' not in observations[0]['native_records']
        else:
            assert not observations, (mode, 'unproved identity was posted')
        print('PASS:', mode, 'native child/streams/argv/identity/cleanup')
