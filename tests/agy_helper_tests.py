"""Native hook output and SDK authenticated-wrapper contracts; no real sessions."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    helper, wrapper = (Path(value).resolve() for value in sys.argv[1:3])
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
            "AGY_TEST_CAPTURE": str(capture),
        }

        def run(name, payload, **overrides):
            capture.write_text("", encoding="utf-8")
            result = subprocess.run(
                [str(helper), name], input=json.dumps(payload), encoding="utf-8",
                capture_output=True, timeout=70, env={**environment, **overrides},
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
            )
            assert result.returncode == 0, result.stderr
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


if __name__ == "__main__":
    main()
