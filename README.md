# Antigravity add-on for Logonomic

This package supplies Antigravity launcher discovery, optional hook integration,
quota observations, and exact import of existing Antigravity conversation
identities. The adapter and helper use the public Logonomic CLI add-on SDK and
C++20. They have no Qt or private desktop-header dependency.

The package recognizes native `agy` executable invocations, including supported
shell wrappers and literal environment assignments. `npx agy` and `bunx agy`
name unrelated package-manager inputs and are not Antigravity launchers.
Lifecycle evidence requires a verified native executable reporting at least
Antigravity 1.1.11. Newer versions are admitted without an upper allowlist;
individual hook records must satisfy their observed contract.

## User-owned hook configuration

Antigravity reads `~/.gemini/config/hooks.json` globally. Launch preparation
never creates, modifies, repairs, or deletes that file. To opt in, merge the
`logonomic-hook-relay` member from the matching package asset into that file:

- Windows: `assets/hooks-windows.json`
- Linux: `assets/hooks-linux.json`

Preserve every other member. When replacing a previous Logonomic subscription,
replace that member as one unit with the new packaged-helper stanza. The package
does not dispatch to the desktop's native relay.

Preparation enables the integration only when the exact supported stanza is
already present at an owned regular path, the launcher's home is statically
resolvable, and the selected launcher is recognized. Otherwise the original
launcher remains an ordinary unmanaged launch. The subscription is inert outside
a bound Logonomic session. A failed or older version probe withdraws lifecycle
observations while retaining opted-in title delivery.

`PreInvocation` records inferred working state and then requests the host's
current title/delegation instruction. Antigravity names no turn and restarts
`invocationNum` at zero for every execution of a conversation, so the first
invocation of an execution names the root turn from `conversationId` and
`initialNumSteps`, the conversation's step count when that execution began;
later invocations of the execution name no turn and cannot advance it again.
An execution that another hook forces to continue after `Stop` therefore counts
as a new turn, and a conversation rewound to an earlier step count repeats a
retired turn identity, whose start the host refuses as stale. An accepted instruction is emitted as an
ephemeral `injectSteps` message. No stale instruction is cached in the package.
`PostToolUse` observes only direct `invoke_subagent` children proven by the
bounded adjacent transcript records. Transcript contents, prompts, and log paths
are not forwarded. A fully idle `Stop` returns control to the user and reconciles
active children; it does not end the continuing conversation or claim a turn
outcome. Stop emits no decision that could block or continue the agent.

Quota runs `agy -p /usage --output-format json` with stdin closed, under host
process custody. It requires exactly two named model-family allowance groups
with one numeric bucket each. Weekly durations and valid reset dates are
normalized; unrecognized durations are omitted. Timeouts and malformed responses
cannot publish ready quota.

Usage scanning reads the user's per-conversation SQLite databases in
`.gemini/antigravity-cli/conversations` through read-only connections and
SQLite's online backup into a bounded in-memory snapshot, one database at a
time. Raw database and transcript content is never written to disk, returned,
or retained in the usage cache; only native usage metadata is extracted into
records. Every native usage event becomes one delta record keyed by its cascade
ID, step index, and response ID; reported input, output, and cache-read counters
are retained as observed. Missing counters remain unknown; the scanner does not
extract, persist, or transmit conversation text, estimate usage, or attach cost.
Pages come from one sealed host-selected snapshot cache, so continuation never
rescans a changing history.

Resume relaunches a recorded conversation as `agy --conversation <ID>` in the
session workspace, through the package's launch bridge. The ID is the resume
record's `provider_session_id`, which a lifecycle observation or an imported
conversation identity supplies. Antigravity keeps a single profile per user and
ignores `GEMINI_CLI_HOME`, so the package neither isolates nor selects an
account: resume reaches the conversation through whichever account that profile
holds, and Antigravity itself reports a conversation it cannot find there. The
launcher's literal assignments and options carry over. Any other command in the
launcher, an option that selects another conversation, runs a single prompt or
starts no session, a positional word, or a word or assignment a shell would
expand makes the launcher unresumable instead of approximated. Installed hooks report into a resumed conversation as into a
launch.

Antigravity exposes no delegated task interface, read-only structured review,
or account authentication observation in the adopted native integration. The
package does not advertise those capabilities.

Delegated-task support is currently unimplemented and unverified in the adopted
integration; this is not proof that the Antigravity CLI is incapable of it.
Proper host delegated-task support is ABSOLUTELY MANDATORY, not optional. The
package MUST implement and validate end-to-end host delegated tasks, including
required control tools and action admission, actual CLI integration,
authentication, host process custody, and cleanup. Capability declarations, mode
acknowledgments, and blanket refusal do not constitute completion. Preserve safe
refusal until its replacement passes end-to-end validation. Any established
upstream limitation MUST be reported with concrete evidence; it does not silently
waive the product requirement. This mandatory implementation remains incomplete.

## Build and qualification

Configure an out-of-source CMake build with the required platform toolchain.
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` accepts an explicit local
`sdk/logonomic_cli_addon_sdk` directory; without it, CMake obtains that public SDK
from the Logonomic repository's `master` branch. Windows compilation uses the
user-wide `queued-build` command and the applicable VS 2026/FASTBuild policy.

The default build produces `agy_adapter`, `agy_helper`, and the indexed package
under `package/logonomic.cli.agy` for a single-configuration generator, or
`package/<config>/logonomic.cli.agy` for a multi-configuration generator.
The `agy_provider_tests` target exercises native
recognition, quota, lifecycle, transcript privacy, and read-only hook ownership.
CTest also registers SDK package validation and conformance. Host integration
qualification must exercise the real authenticated helper and launcher service
alongside the package tests.

The package recipe proves the Windows/Linux x86-64 compiler target during
configuration. `cmake --install` exports a fresh artifact beneath its install-time
prefix and preserves executable permissions. An existing per-package destination,
including a dangling symlink, is refused. POSIX `DESTDIR` staging is supported;
nonempty `DESTDIR` with a drive-letter prefix is refused. Export is not atomic:
a failure may leave a partial artifact directory. Choose a fresh destination or
manually remove only that identified disposable export before retrying; never
remove an installed generation this way. The application's package ingestion
owns atomic installation and updates.

The icon's MIT notice is in `package/notices/lobe_icons_mit.txt`; source and
conversion details are in `package/provenance/lobe_icons.json`. The package uses
a transparent 512-pixel PNG rendered from the original colored SVG, preserving
its mask and blur appearance within the package's raster-image contract. The
unchanged source SVG is retained in `source-assets/antigravity-color.svg` outside
the assembled package.
Antigravity names and logos identify independently provided software and do not
imply endorsement or affiliation.
