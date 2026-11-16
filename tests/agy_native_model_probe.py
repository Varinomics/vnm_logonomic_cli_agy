"""Native source/helper gate; root activation and host transport are fixtures."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

parser = argparse.ArgumentParser(description='Probe native Antigravity models using an isolated loopback Gemini endpoint.')
parser.add_argument('--cli', required=True, help='Official agy binary')
parser.add_argument('--helper', required=True, help='Built agy_helper binary')
parser.add_argument('--output-directory', required=True, help='New directory for isolated fixture data and logs')
args = parser.parse_args()
cli = Path(args.cli).resolve(strict=True)
helper = Path(args.helper).resolve(strict=True)
RUN = Path(args.output_directory).resolve()
RUN.mkdir(parents=True)
WORK = RUN / 'work'
for directory in [WORK / '.agents', RUN / 'home', RUN / 'tmp']:
    directory.mkdir(parents=True, exist_ok=True)
requests = []

class Endpoint(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        requests.append({'method': 'GET', 'path': self.path})
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps({'models': [{'name': 'models/native-root-model',
            'displayName': 'native-root-model', 'supportedGenerationMethods': ['generateContent', 'countTokens']}]}).encode())

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['content-length'])))
        requests.append({'method': 'POST', 'path': self.path, 'keys': sorted(body)})
        assert self.path.startswith('/v1beta/models/generativelanguage.googleapis.com/models/native-root-model:')
        self.send_response(200)
        if ':countTokens' in self.path:
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(b'{"totalTokens":10}')
            return
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        response = {'candidates': [{'content': {'role': 'model', 'parts': [{'text': 'AGY_NATIVE_MODEL_OK'}]},
            'finishReason': 'STOP', 'index': 0}],
            'usageMetadata': {'promptTokenCount': 10, 'candidatesTokenCount': 4, 'totalTokenCount': 14},
            'modelVersion': 'native-root-model'}
        self.wfile.write(('data: ' + json.dumps(response) + '\n\n').encode())
        self.wfile.flush()

server = ThreadingHTTPServer(('127.0.0.1', 0), Endpoint)
threading.Thread(target=server.serve_forever, daemon=True).start()
hook = RUN / 'capture.py'
hook.write_text('''import hashlib,json,os,pathlib,subprocess,sys,uuid
body = json.load(sys.stdin)
with open(os.environ['AGY_TEST_CAPTURE'], 'a') as output:
    output.write(json.dumps({'event': sys.argv[1], 'body': body}) + '\\n')
if sys.argv[1] == 'SessionStart':
    pathlib.Path(os.environ['AGY_LOGONOMIC_ACTIVATION_FILE']).write_text(json.dumps({
        'version': 1, 'generation_nonce': os.environ['LOGONOMIC_PROVIDER_GENERATION_NONCE'],
        'session_digest': hashlib.sha256(os.environ['LOGONOMIC_SESSION_CAPABILITY'].encode()).hexdigest(),
        'provider_session_id': body['conversationId'], 'transition_nonce': uuid.uuid4().hex,
        'committed': True}))
if sys.argv[1] == 'PreInvocation':
    subprocess.run([os.environ['AGY_TEST_HELPER'], 'PreInvocation'], input=json.dumps(body),
        text=True, stdout=subprocess.DEVNULL, check=True)
print('{}')
''')
capture = RUN / 'hooks.jsonl'
if capture.exists():
    capture.unlink()
(WORK / '.agents/hooks.json').write_text(json.dumps({'native-model-fixture': {
    name: [{'type': 'command', 'command': f'python3 {hook} {name}', 'timeout': 5}]
    for name in ['SessionStart', 'PreInvocation', 'PostInvocation', 'Stop']}}))
base = f'http://127.0.0.1:{server.server_port}'
model = 'gemini-api://generativelanguage.googleapis.com/models/native-root-model'
settings = {'modelProvider': 'gemini',
    'customModelsConfig': {'customModels': {'native-root-model': {'modelName': model, 'label': 'selected-alias'}}}}
settings_path = RUN / 'home/.gemini/antigravity-cli/settings.json'
settings_path.parent.mkdir(parents=True, exist_ok=True)
settings_path.write_text(json.dumps(settings))
wrapper = RUN / 'vnmctl'
wrapper.write_text('''#!/usr/bin/env python3
import json,os,sys
if sys.argv[1:3] == ['provider-event','deliver']:
    with open(os.environ['AGY_TEST_HELPER_CAPTURE'], 'a') as output:
        output.write(json.dumps(json.load(sys.stdin)) + '\\n')
    print('{"accepted":true,"epoch":1}')
elif sys.argv[1:3] == ['provider-event','acknowledge']:
    pass
elif sys.argv[1] == 'request-title-instruction':
    print('{"accepted":true,"instruction_required":false,"instruction":""}')
else:
    sys.exit(2)
''')
wrapper.chmod(0o755)
helper_capture = RUN / 'helper_events.jsonl'
if helper_capture.exists():
    helper_capture.unlink()
environment = {key: os.environ[key] for key in ['PATH', 'LANG', 'LD_LIBRARY_PATH', 'HOME'] if key in os.environ}
environment.update({'GEMINI_API_KEY': 'fixture-only', 'GOOGLE_GEMINI_BASE_URL': base,
    'AGY_CLI_DISABLE_AUTO_UPDATE': '1', 'AGY_TEST_CAPTURE': str(capture),
    'HTTP_PROXY': 'http://127.0.0.1:9', 'HTTPS_PROXY': 'http://127.0.0.1:9',
    'ALL_PROXY': 'http://127.0.0.1:9', 'NO_PROXY': '127.0.0.1,localhost',
    'AGY_TEST_HELPER': str(helper),
    'AGY_TEST_HELPER_CAPTURE': str(helper_capture),
    'AGY_LOGONOMIC_HOOKS_ENABLED': '1', 'AGY_LOGONOMIC_LIFECYCLE_ENABLED': '1',
    'AGY_LOGONOMIC_ACTIVATION_FILE': str(RUN / 'activation.json'),
    'LOGONOMIC_VNMCTL_PATH': str(wrapper), 'LOGONOMIC_SESSION_CAPABILITY': 'a' * 64,
    'LOGONOMIC_PROVIDER_PACKAGE_ID': 'logonomic.cli.agy',
    'LOGONOMIC_PROVIDER_GENERATION_DIGEST': 'b' * 64,
    'LOGONOMIC_PROVIDER_GENERATION_NONCE': 'c' * 32,
    'LOGONOMIC_PROVIDER_HELPERS_DIR': str(RUN / 'helpers')})
command = ['bwrap', '--die-with-parent', '--ro-bind', '/', '/', '--dev-bind', '/dev', '/dev', '--proc', '/proc',
    '--bind', str(RUN / 'home'), str(Path.home()), '--bind', str(RUN), str(RUN),
    '--bind', str(RUN / 'tmp'), '/tmp', '--chdir', str(WORK),
    str(cli), '--settings', json.dumps(settings), '--model', 'selected-alias',
    '--print', 'Reply AGY_NATIVE_MODEL_OK. Do not use tools.', '--output-format', 'stream-json', '--print-timeout', '30s']
try:
    result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=45)
    (RUN / 'stdout.jsonl').write_text(result.stdout)
    (RUN / 'stderr.txt').write_text(result.stderr)
    print(json.dumps({'exit': result.returncode, 'requests': requests,
        'response_seen': 'AGY_NATIVE_MODEL_OK' in result.stdout, 'stderr_tail': result.stderr[-1000:]}))
    hooks = [json.loads(line) for line in capture.read_text().splitlines()] if capture.exists() else []
    print(json.dumps({'hooks': [{'event': item['event'], **{key: item['body'].get(key)
        for key in ['conversationId', 'modelName', 'invocationNum', 'initialNumSteps']}}
        for item in hooks]}))
    helper_events = [json.loads(line) for line in helper_capture.read_text().splitlines()]
    updates = [update for event in helper_events
        for update in event['event']['body']['event']['body'].get('model_state', {}).get('updates', [])]
    print(json.dumps({'helper_model_updates': updates}))
    assert result.returncode == 0 and requests
    assert any(update['model'] == model for update in updates)
    activation = json.loads((RUN / 'activation.json').read_text())
    assert all(update['transition_nonce'] == activation['transition_nonce'] and
        update['provider_session_id'] == activation['provider_session_id'] and
        update['agent_id'] == '' for update in updates)
    native_init = next(json.loads(line)['init'] for line in result.stdout.splitlines()
        if json.loads(line).get('event') == 'init')
    assert native_init['model'] == 'selected-alias'
finally:
    server.shutdown()
