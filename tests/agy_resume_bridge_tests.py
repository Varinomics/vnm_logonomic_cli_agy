import http.server
import json
import os
from pathlib import Path
import subprocess
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
    native.write_text('''import json, pathlib, sys
mode, identity, literal = sys.argv[1:4]
assert sys.argv[4] == '--log-file'
log = pathlib.Path(sys.argv[5])
stream = f'I0929 07:51:02.417194     171 conversation_manager.go:887] Streaming conversation {identity}\\n'
redraw = f'I0929 07:51:02.478085     362 manager.go:932] Full redraw completed (rerenderAll) for conversation {identity} (epoch 0, items 4)\\n'
if mode == 'selector':
    log.write_text(f'I0929 07:51:02.417194       1 common.go:401] Resuming conversation {identity}\\n')
    sys.exit(0)
if mode == 'missing': sys.exit(0)
if mode == 'overflow': log.write_text('x' * (1024 * 1024 + 1))
else: log.write_text(stream + redraw + 'private text must not leave the child log\\n')
print('native stdout', flush=True)
print('native stderr', file=sys.stderr, flush=True)
message = sys.stdin.readline().rstrip('\\n')
print(json.dumps({'literal': literal, 'message': message}), flush=True)
sys.exit(7 if mode == 'exit7' else 0)
''', encoding='utf-8')
    for mode in ['success', 'exit7', 'selector', 'missing', 'overflow', 'mismatch']:
        observations = []
        event = threading.Event()
        authorization = 'Bearer ' + 'a' * 64

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                assert self.path == '/events'
                assert self.headers['Authorization'] == authorization
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                observations.append(body)
                self.send_response(422 if mode == 'mismatch' else 200)
                self.end_headers()
                event.set()

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        owned = root / mode
        owned.mkdir()
        offer = {'version': 1, 'mechanism': 'http-json-post', 'confirmation': 'observed-identity',
                 'endpoint': f'http://127.0.0.1:{server.server_port}/events', 'authorization': authorization}
        environment = dict(os.environ, AGY_LOGONOMIC_STARTUP_OBSERVER=json.dumps(offer),
                           AGY_LOGONOMIC_STARTUP_ROOT=str(owned))
        literal = 'space $() ; " literal'
        process = subprocess.Popen([bridge, sys.executable, str(native), mode,
                                    other_identity if mode == 'mismatch' else identity, literal],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=environment)
        if mode in ['success', 'exit7', 'mismatch']:
            assert event.wait(5), (mode, 'no native startup observation')
        stdout, stderr = process.communicate(b'own terminal message\n', timeout=15)
        server.shutdown()
        server.server_close()
        worker.join()
        assert not list(owned.iterdir()), (mode, 'owned startup log not retired')
        if mode in ['success', 'exit7']:
            assert process.returncode == (7 if mode == 'exit7' else 0), (mode, process.returncode, stderr)
            assert b'native stdout' in stdout and b'native stderr' in stderr
            assert json.loads(stdout.splitlines()[-1]) == {'literal': literal, 'message': 'own terminal message'}
        else:
            assert process.returncode == 3, (mode, process.returncode, stderr)
        if mode in ['success', 'exit7', 'mismatch']:
            assert len(observations) == 1
            assert list(observations[0]) == ['native_records']
            assert 'Resuming' not in observations[0]['native_records']
            assert 'private text' not in observations[0]['native_records']
        else:
            assert not observations, (mode, 'unproved identity was posted')
        print('PASS:', mode, 'native child/streams/argv/identity/cleanup')
