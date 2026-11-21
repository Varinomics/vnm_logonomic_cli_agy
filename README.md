# Antigravity add-on for Logonomic

[![Provider contracts](https://github.com/Varinomics/vnm_logonomic_cli_agy/actions/workflows/ci.yml/badge.svg)](https://github.com/Varinomics/vnm_logonomic_cli_agy/actions/workflows/ci.yml)

This package supplies Antigravity launcher discovery, automatic hook integration,
live conversation changes, quota observations, exact conversation resume, and host delegated implementation
tasks. The adapter and helper use
the Logonomic CLI add-on SDK and C++20. They have no Qt or private
desktop-header dependency.

The package recognizes native `agy` executable invocations, including supported
shell wrappers and literal environment assignments. `npx agy` and `bunx agy`
name unrelated package-manager inputs and are not Antigravity launchers.
Lifecycle evidence requires a verified native executable reporting at least
Antigravity 1.1.11. Newer versions are admitted without an upper allowlist;
individual hook records must satisfy their observed contract.

## Hook configuration

Antigravity reads `~/.gemini/config/hooks.json` globally. For recognized launches
with a statically resolvable home, Logonomic prepares the file automatically
before the terminal starts, including before an exact conversation resume. An
absent file becomes a JSON object containing the package's `logonomic-hook-relay`
member. An absent or stale member in an existing object is installed or replaced
as one unit with the matching packaged-helper stanza. Every unrelated byte is
preserved, including numeric spellings, formatting, and escaped member names.
An exact supported member leaves the entire file unchanged.

Recognized literal environment assignments and shell wrappers retain hook
integration. Launchers with additional commands, including sourced environment
scripts, receive no managed hook or delivery authority because those commands
can alter the home outside the recognized assignments.

The adapter transforms a bounded snapshot without receiving its native path.
The host owns the replacement and refuses linked/reparse paths and unresolved
homes. Before any change, it retains and verifies the exact previous bytes and
whether the file existed, then rechecks the target and snapshot and commits the
replacement atomically. Recovery copies are adjacent directories named
`hooks.json.logonomic-recovery-<nonce>` containing `content.bin` and
`metadata.json`. Metadata identifies the requested/resolved path, prior presence,
and content digest. A verified record with `present: true` supplies the exact
bytes for restoring the previous file; `present: false` records that the previous
file was absent. Successful records are retained without automatic expiry and
are never overwritten. An incomplete directory from a failed attempt is not a
verified recovery record.

Malformed, duplicate-name, nonobject, or over-bound JSON and a failed recovery
copy prevent replacement and fail preparation. Hook documents and replacements
are limited to 256 KiB, with the SDK's nesting and object-member limits. The
Windows updater setting described below applies independently of hook
configuration. The subscription invokes the packaged helper and stays inert
outside a bound Logonomic session. A failed or older native version probe
withdraws lifecycle observations while retaining title delivery.

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

Native permission dialogs are observed through Antigravity's
[`statusLine` callback](https://antigravity.google/docs/cli/statusline/), whose
`tool_confirmation_pending` flag is true while a confirmation is shown and
omitted after it closes. Hosts supporting `permission-snapshot@1` also prepare
`~/.gemini/antigravity-cli/settings.json` through the same retained-preimage
and atomic-replacement contract. The host enables observation only after both
configuration operations succeed. Older hosts leave this observer inert.
The enable marker must match the current provider generation nonce exactly;
an inherited marker cannot enable observation in a different generation.

The settings transform composes an existing status command with the packaged
observer, preserving its callback input, output, exit status and display
options. With no custom command, the built-in status line remains visible.
An explicit `statusLine.enabled: false` stays disabled, so native permission
observation is unavailable until the user enables it. Original settings are
retained in `logonomic-statusline-relay` to prevent nested wrappers during
repair; replacing the custom command adopts the user's replacement. A command
whose wrapper would exceed the platform command-length limit is refused. On
Windows, the wrapper reads the retained command from that settings file so its
length does not grow with the user's command. The retained settings also keep
the callback callable outside a Logonomic session.

The helper forwards only the active conversation ID and the pending boolean.
Missing or malformed snapshots cannot clear an outstanding permission, and a
callback cannot establish a session epoch or claim a turn. The host pairs
accepted true/false levels within the current conversation, allowing later
permission dialogs to request attention again. A valid callback with the flag
omitted or false resolves that permission episode; session or epoch closure
also ends it. A `Stop` hook alone supplies no permission decision. This signal
covers native tool confirmations. Ordinary model questions and `ask_question`
tool proposals do not establish that a user is waiting to answer and are not
reported as pending permission.

Quota runs `agy -p /usage --output-format json` with stdin closed, under host
process custody. It requires exactly two named model-family allowance groups
with one numeric bucket each. Weekly durations and valid reset dates are
normalized; unrecognized durations are omitted. Timeouts and malformed responses
cannot publish ready quota.

On Windows, provider-managed launches and version probes set Antigravity's documented
[`AGY_CLI_DISABLE_AUTO_UPDATE=true`](https://www.agy.dev/docs/cli/troubleshooting/#resolve-self-updater-locks-and-failures)
to prevent the native background updater from opening terminal windows outside
Logonomic. This sets the initial process environment for interactive and resumed
sessions, delegated tasks, and quota refreshes. An explicit assignment inside a
launcher shell script can override it. The setting belongs to each launched
process; the user's profile and standalone Antigravity launches keep their
updater policy.

Usage scanning reads the user's per-conversation SQLite databases in
`.gemini/antigravity-cli/conversations` through read-only connections and
SQLite's online backup into a bounded in-memory snapshot, one database at a
time. Raw database and transcript content is never written to disk, returned,
or retained in the usage cache; only native usage metadata is extracted into
records. The scanner identifies each delta by its native cascade and step,
including the response ID when present. If a later observation supplies that ID,
the accepted record keeps its original key. Compatible copies count once.
Reported input, output, and cache-read counters are retained as observed.
Missing counters remain unknown; the scanner does not
extract, persist, or transmit conversation text or estimate usage.
Pages come from one sealed host-selected snapshot cache, so continuation never
rescans a changing history.

## Live conversation changes and resume

For supported literal local launches, the package observes native root selection
without requiring another prompt, including idle clear/new and manual resume.
Status-line snapshots do not create an epoch. Captured activation identities
fence delayed hooks and model-context delivery; activated context requests
require a host that negotiates `model-context-delivery@2`.

Exact resume uses `agy --conversation <ID>` and preserves the verified native
home. Optional `interactive-resume@3/@4` inspection distinguishes availability
from content and keeps uncertain history recoverable. The provider confirms
identity separately from effective working directory and does not advertise
`execution-context@1` or claim native exclusive-selection enforcement.

See [Native conversation integration](docs/native_conversation_contract.md) for
the supported launch shapes, native evidence, compatibility behavior, and
directory, project, ownership, and inspection boundaries. The package advertises
no read-only structured review or account authentication observation.

## Delegated tasks

Proper host delegated-task support is ABSOLUTELY MANDATORY, not optional. The
package MUST implement and validate end-to-end host delegated tasks, including
required control tools and action admission, actual CLI integration,
authentication, host process custody, and cleanup. Capability declarations, mode
acknowledgments, and blanket refusal do not constitute completion. Any
established upstream limitation MUST be reported with concrete evidence; it does
not silently waive the product requirement.

The package declares `delegated-session@3` and its `structured-session@3`
dependency for the `implementation` purpose. A bare structured implementation
run, without Task servers, is refused.

Preparation requires the host's relay connection. Each Task server becomes an
MCP server named `logonomic_<server_id>` whose command is the host's relay with
`--descriptor`, `--server`, and a catalog receipt path and nonce, and which
carries no environment. These definitions form one private artifact,
`agy/.agents/mcp_config.json` below the Task's private root, and that folder is
added to the agy workspace with `--add-dir`. agy loads a workspace folder's
`.agents/mcp_config.json` in addition to the user's own servers, so the package
never writes the user's Antigravity configuration. The acknowledgment names the
Task servers and connection version 1. A server identity that is not a plain
lowercase name is refused, because agy names a directory after each server.

The launcher must be a sole, literal native command, as for resume, and must not
select a conversation, a prompt mode, an input or output format, a deadline, an
agent, or an execution mode; the program must report Antigravity 1.2.11 or
newer. The process runs through the launch bridge in the session workspace,
with the frozen launcher environment and the launcher's literal assignments, as
`agy <launcher options> --input-format stream-json --output-format stream-json
--disable-slash-commands --add-dir <folder> --print=`; slash expansion would
rewrite the raw prompt.

`delegated-model-selection@1` accepts per-task model and reasoning-effort
selection through agy's native `--model` and `--effort` options. Each requested
setting replaces that option in this invocation; omitted settings retain the
launcher's defaults. The saved launcher and Antigravity settings are unchanged.

A delegated run keeps the launcher's permission posture, and the package adds
no permission. Headless agy denies every tool it cannot ask about unless the
launcher passes `--dangerously-skip-permissions`, which approves every tool, or
the user's own `permissions.allow` in `~/.gemini/antigravity-cli/settings.json`
grants it; agy offers no narrower grant a package could add. For the Task tools
that means entries such as `mcp(logonomic_coordination/*)` and
`mcp(logonomic_delegation/*)` (`mcp(<server>/<tool>)` for single tools). When
neither the launcher nor those entries grant every Task tool, preparation
refuses with `task_tools_not_permitted` and a detail naming the entries to add.
Other tools follow the same posture: agy ends a turn at a tool it denies.

Inspection follows agy's headless stream-json records. The single `init` record
must report the session workspace; the raw prompt is then written as one `user`
message and stdin is closed, which ends the process after that turn. agy starts the added folder's servers when it
processes the message and reports the root conversation's `user_input` step
once they are loaded. At that step each Task server's receipt must hold its
nonce, which the host writes only after the server's own catalog succeeded. A
missing receipt fails the task and the host terminates agy, which may have
progressed until then. Progress records are presentation only. The task result
is the root conversation's single successful, non-empty `result` response; an
error or empty result, a record after the result, a nonzero exit, or a timeout
fails the task. A result whose denied actions include the root conversation's
last MCP call, made to a Task server and left unanswered, fails with
`task_tools_not_permitted`; a turn agy ended at another denied tool without a
response fails with `tool_permission_denied`.

agy has no ephemeral mode: a delegated task is recorded in the user's
Antigravity conversation history, and agy caches the Task servers' tool schemas
below `~/.gemini/antigravity-cli/mcp/logonomic_<server_id>/`, which the stable
server names keep bounded.

## Build and qualification

The [contributor guide](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/CONTRIBUTING.md) covers local package checks.
Send provider pull requests to [the agy repository](https://github.com/Varinomics/vnm_logonomic_cli_agy/pulls).

Configure an out-of-source CMake build with the required platform toolchain.
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` accepts an explicit local
`sdk/logonomic_cli_addon_sdk` directory; without it, CMake obtains that SDK
from `vnm_logonomic_cli_addon` on `master`. Windows compilation uses the
user-wide `queued-build` command and the applicable VS 2026/FASTBuild policy.

The provider links the SQLite amalgamation, which configuration downloads
from www.sqlite.org and verifies by its SHA3-256. A release names the exact
archive with `AGY_SQLITE_AMALGAMATION_ARCHIVE_URL` and
`AGY_SQLITE_AMALGAMATION_ARCHIVE_SHA3_256`; without them configuration takes the
current archive that SQLite's download page names.

The default build produces `agy_adapter`, `agy_helper`, and the indexed package
under `package/logonomic.cli.agy` for a single-configuration generator, or
`package/<config>/logonomic.cli.agy` for a multi-configuration generator.
The `agy_provider_tests` target exercises native
recognition, quota, lifecycle, transcript privacy, read-only hook ownership, and
delegated preparation and inspection.
CTest also registers SDK package validation and conformance. Host integration
qualification must exercise the real authenticated helper and launcher service
alongside the package tests.

The package recipe proves the Windows/Linux x86-64 or macOS ARM64 compiler target
during configuration. `cmake --install` exports a fresh artifact beneath its install-time
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
