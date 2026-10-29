"""Native hook output and SDK authenticated-wrapper contracts; no real sessions."""

import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    helper, wrapper = (Path(value).resolve() for value in sys.argv[1:3])
    provider = Path(sys.argv[3]).resolve()
    package = Path(sys.argv[4]).resolve()
    with tempfile.TemporaryDirectory(prefix="agy-helper-") as temporary:
        capture = Path(temporary) / "requests.jsonl"
        environment = {
            **os.environ,
            "LOGONOMIC_VNMCTL_PATH": str(wrapper),
            "LOGONOMIC_SESSION_CAPABILITY": "a" * 64,
            "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.agy",
            "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "b" * 64,
            "LOGONOMIC_PROVIDER_GENERATION_NONCE": "c" * 32,
            "LOGONOMIC_PROVIDER_HELPERS_DIR": str(helper.parent),
            "AGY_LOGONOMIC_HOOKS_ENABLED": "1",
            "AGY_LOGONOMIC_LIFECYCLE_ENABLED": "1",
            "LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED": "c" * 32,
            "AGY_TEST_CAPTURE": str(capture),
        }

        def run(name, payload, arguments=(), input_text=None, **overrides):
            capture.write_text("", encoding="utf-8")
            result = subprocess.run(
                [str(helper), name, *arguments],
                input=json.dumps(payload) if input_text is None else input_text, encoding="utf-8",
                capture_output=True, timeout=70,
                env={key: value for key, value in {**environment, **overrides}.items() if value is not None},
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
            )
            assert result.returncode == 0, (result.returncode, result.stderr)
            records = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
            return result, records

        payload = {"conversationId": "Conversation-Parent", "prompt": "private prompt"}
        result, records = run("PreInvocation", payload)
        assert [record["operation"] for record in records] == [
            "provider-event", "acknowledge", "request-title-instruction"]
        assert records[0]["request"]["event"] == {
            "name": "PreInvocation", "body": {"conversationId": "Conversation-Parent"}}
        context_request = records[2]["request"]
        assert context_request["hook_event_name"] == "PreInvocation"
        assert context_request["delivery"] == {"version": 1, "kind": "model-context"}
        assert json.loads(result.stdout) == {"injectSteps": [{
            "ephemeralMessage": "Set the session title with the supplied host command."}]}
        assert "private prompt" not in capture.read_text(encoding="utf-8")

        _, records = run("PreInvocation", {**payload, "invocationNum": 0, "initialNumSteps": 12,
                                           "transcriptPath": "/private/transcript.jsonl"})
        assert records[0]["request"]["event"]["body"] == {
            "conversationId": "Conversation-Parent", "invocationNum": 0, "initialNumSteps": 12}, \
            "the turn counters reach the host; the transcript location does not"

        _, records = run("PreInvocation", payload, AGY_TEST_RETRY="1")
        assert records[0] == records[1], "retry must reuse key, nonce, and evidence"
        assert len(records[0]["request"]["idempotency_key"]) == 32

        result, records = run("Stop", {**payload, "fullyIdle": True, "executionNum": 11})
        assert result.stdout == "", "Stop must never block or continue the agent"
        assert [record["operation"] for record in records] == ["provider-event", "acknowledge"]

        result, records = run("PostToolUse", {**payload, "toolCall": {"name": "read_file"}})
        assert json.loads(result.stdout) == {}
        assert "prompt" not in json.dumps(records)

        result, records = run("PreInvocation", payload, AGY_LOGONOMIC_LIFECYCLE_ENABLED="0")
        assert [record["operation"] for record in records] == ["request-title-instruction"]
        assert "injectSteps" in json.loads(result.stdout)

        result, records = run("PreInvocation", payload, AGY_TEST_CONTEXT="non-title")
        assert json.loads(result.stdout) == {"injectSteps": [{"ephemeralMessage":
            "Host delegation and notice text.\nPreserve this exact context."}]}
        for mode in ("empty", "refused", "stale_generation", "closed_ledger", "unsupported_capability"):
            result, records = run("PreInvocation", payload, AGY_TEST_CONTEXT=mode)
            assert result.stdout == "", "empty or refused context must not be invented"

        result, records = run("PreInvocation", payload, AGY_LOGONOMIC_HOOKS_ENABLED="0")
        assert records == [] and result.stdout == ""
        result, records = run("PreInvocation", payload, LOGONOMIC_SESSION_CAPABILITY="")
        assert records == [] and result.stdout == ""

        # Oracle: Antigravity's status callback schema names the active
        # conversation and reports true only while a confirmation is shown.
        snapshot = {
            "conversation_id": "Conversation-Parent",
            "agent_state": "tool_use",
            "tool_confirmation_pending": True,
            "email": "private@example.com",
            "transcript_path": "/private/transcript.jsonl",
        }
        for marker in (None, "", "1", "d" * 32, "C" * 32):
            result, records = run("StatusLine", snapshot, LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED=marker)
            assert result.stdout == "" and records == [], \
                "a missing or inherited stale marker cannot enable a new generation's permission observer"
        result, records = run("StatusLine", snapshot)
        assert records, "the marker bound to this provider generation enables its permission observer"
        assert result.stdout == ""
        assert records[0]["request"]["event"] == {
            "name": "StatusLine", "body": {
                "conversationId": "Conversation-Parent",
                "toolConfirmationPending": True,
            }}
        assert [record["operation"] for record in records] == ["provider-event", "acknowledge"]
        assert "private" not in json.dumps(records)
        pending_events = [records[0]["request"]["event"]]

        for pending in (False, True):
            current = {key: value for key, value in snapshot.items() if key != "tool_confirmation_pending"}
            if pending:
                current["tool_confirmation_pending"] = True
            result, records = run("StatusLine", current)
            assert result.stdout == ""
            assert records[0]["request"]["event"]["body"]["toolConfirmationPending"] is pending
            pending_events.append(records[0]["request"]["event"])

        classified = subprocess.run(
            [str(provider), "--permission-snapshot-events", str(package)], input=json.dumps(pending_events),
            encoding="utf-8", capture_output=True, timeout=10)
        assert classified.returncode == 0, classified.stderr
        results = json.loads(classified.stdout)
        assert [result["permission_snapshot"]["pending"] for result in results] == [True, False, True]
        assert all(result["classification"] == "event" and len(result["observations"]) == 1 for result in results)
        assert all(result["observations"][0]["provider_session_id"] == "Conversation-Parent" for result in results)

        _, records = run("StatusLine", {**snapshot, "tool_confirmation_pending": False})
        assert records[0]["request"]["event"]["body"]["toolConfirmationPending"] is False
        for invalid in (
                {**snapshot, "tool_confirmation_pending": "false"},
                {**snapshot, "tool_confirmation_pending": None},
                {**snapshot, "conversation_id": ""},
                {**snapshot, "agent_state": None},
                {"conversation_id": "Conversation-Parent"}):
            result, records = run("StatusLine", invalid)
            assert result.stdout == "" and records == [], "missing observation never resolves pending attention"

        original = '{\n  "conversation_id":"Conversation-Parent", "agent_state":"working",\n  "value":"café"\n}\n\n'
        result, records = run("StatusLine", None, ("--pass-through",), input_text=original)
        assert result.stdout == original, "the user's command receives the exact callback bytes"
        assert records[0]["request"]["event"]["body"]["toolConfirmationPending"] is False
        for overrides in (
                {"AGY_LOGONOMIC_HOOKS_ENABLED": "0"},
                {"AGY_LOGONOMIC_LIFECYCLE_ENABLED": "0"},
                {"LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED": "0"},
                {"LOGONOMIC_SESSION_CAPABILITY": ""}):
            result, records = run("StatusLine", None, ("--pass-through",), input_text=original, **overrides)
            assert result.stdout == original and records == []
        original = json.dumps({**snapshot, "private": "x" * (4 * 1024 * 1024)})
        result, records = run("StatusLine", None, ("--pass-through",), input_text=original)
        assert result.stdout == original and records == [], "observer limits do not truncate the user's stdin"

        # Exercise the installed command through the native shell contract,
        # using the real helper and the SDK wrapper transport above.
        user_script = Path(temporary) / "user's status script.py"
        user_script.write_text(
            "import sys\nsys.stdout.buffer.write(sys.stdin.buffer.read())\n"
            "sys.stdout.buffer.write(b'\\nUSER STATUS\\n')\nsys.exit(37)\n", encoding="utf-8")
        user_argv = [sys.executable, str(user_script)]
        user_command = subprocess.list2cmdline(user_argv) if os.name == "nt" else shlex.join(user_argv)
        generated = subprocess.check_output([
            str(provider), "--installed-status-line-command", str(package), user_command],
            encoding="utf-8")
        callback = ('{\r\n"conversation_id":"Conversation-Parent", '
                    '"agent_state":"working", "text":"café"\r\n}\n').encode()
        for overrides, observed in (
                ({}, True),
                ({"LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED": "d" * 32,
                  "LOGONOMIC_PROVIDER_GENERATION_NONCE": "d" * 32}, True),
                ({"LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED": None}, False),
                ({"LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED": "1"}, False),
                ({"LOGONOMIC_PROVIDER_GENERATION_NONCE": "d" * 32}, False),
                ({"LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED": "C" * 32}, False),
                ({"LOGONOMIC_PROVIDER_GENERATION_NONCE": None}, False),
                ({"LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED": "",
                  "LOGONOMIC_PROVIDER_GENERATION_NONCE": ""}, False),
                ({"LOGONOMIC_PROVIDER_HELPERS_DIR": ""}, False)):
            capture.write_text("", encoding="utf-8")
            result = subprocess.run(generated, shell=True, input=callback, capture_output=True, timeout=10,
                                    env={key: value for key, value in {**environment, **overrides}.items()
                                         if value is not None})
            assert result.returncode == 37 and result.stdout == callback + b"\nUSER STATUS\n", (result, generated)
            records = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
            assert len(records) == (2 if observed else 0), \
                "only a marker matching the nonempty current generation enables the callback"

        if os.name != "nt":
            # The transport cannot answer until the existing status command
            # has read EOF. This causal handshake detects a retained pipe
            # writer without relying on a tight elapsed-time threshold.
            user_script.write_text(
                "import os,sys,time\nfrom pathlib import Path\n"
                "data = sys.stdin.buffer.read()\n"
                "ready = Path(os.environ['AGY_TEST_TRANSPORT_READY'])\n"
                "deadline = time.monotonic() + 5\n"
                "while not ready.exists() and time.monotonic() < deadline: time.sleep(0.01)\n"
                "assert ready.exists(), 'transport did not start'\n"
                "Path(os.environ['AGY_TEST_CALLBACK_EOF']).write_text('eof')\n"
                "sys.stdout.buffer.write(data + b'\\nUSER STATUS\\n')\nsys.exit(37)\n", encoding="utf-8")
            pending_script = Path(temporary) / "pending_transport.py"
            pending_script.write_text(
                "import os,sys,time\nfrom pathlib import Path\n"
                "if sys.argv[2] == 'acknowledge': sys.exit(0)\n"
                "sys.stdin.buffer.read()\n"
                "Path(os.environ['AGY_TEST_TRANSPORT_READY']).write_text('pending')\n"
                "eof = Path(os.environ['AGY_TEST_CALLBACK_EOF'])\n"
                "deadline = time.monotonic() + 5\n"
                "while not eof.exists() and time.monotonic() < deadline: time.sleep(0.01)\n"
                "assert eof.exists(), 'callback did not receive EOF'\n"
                "Path(os.environ['AGY_TEST_EOF_BEFORE_RESPONSE']).write_text('observed')\n"
                "print('{\"accepted\":true,\"epoch\":1}')\n", encoding="utf-8")
            pending_wrapper = Path(temporary) / "pending-vnmctl"
            pending_wrapper.write_text(
                "#!/bin/sh\nexec " + shlex.join([sys.executable, str(pending_script)]) + ' "$@"\n', encoding="utf-8")
            pending_wrapper.chmod(0o700)
            observed = Path(temporary) / "eof-before-response"
            result = subprocess.run(generated, shell=True, input=callback, capture_output=True, timeout=10,
                                    env={**environment,
                                         "LOGONOMIC_VNMCTL_PATH": str(pending_wrapper),
                                         "AGY_TEST_TRANSPORT_READY": str(Path(temporary) / "transport-ready"),
                                         "AGY_TEST_CALLBACK_EOF": str(Path(temporary) / "callback-eof"),
                                         "AGY_TEST_EOF_BEFORE_RESPONSE": str(observed)})
            assert result.returncode == 37 and result.stdout == callback + b"\nUSER STATUS\n", result
            assert observed.exists() and observed.read_text() == "observed", \
                "the status command receives EOF while host transport is still waiting for its response"


if __name__ == "__main__":
    main()
