# Antigravity add-on for Logonomic

[![Provider contracts](https://github.com/Varinomics/vnm_logonomic_cli_agy/actions/workflows/ci.yml/badge.svg)](https://github.com/Varinomics/vnm_logonomic_cli_agy/actions/workflows/ci.yml)

This package supplies Antigravity launcher discovery, automatic hook integration,
live conversation changes, quota observations, exact conversation resume, and host delegated implementation
tasks. The adapter and helper use
the public Logonomic CLI add-on SDK and C++20. They have no Qt or private
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
records. Every native usage event becomes one delta record keyed by its cascade
ID, step index, and response ID; reported input, output, and cache-read counters
are retained as observed. Missing counters remain unknown; the scanner does not
extract, persist, or transmit conversation text or estimate usage.
Pages come from one sealed host-selected snapshot cache, so continuation never
rescans a changing history.

## Live conversation changes and resume

For a sole literal local interactive launch with lifecycle integration, the
package observes the native root conversation selected by Antigravity. This
includes an idle `/clear` and selection through `/resume`, `/switch`, or
`/conversation`; the user need not send another prompt. Antigravity's
`SessionStart` hook runs before the first invocation of a new conversation and
does not report these idle changes. The package therefore keeps its owned native
log pipe open for the lifetime of the CLI and accepts only the exact root
`conversation_manager.go` streaming record. Opening or cancelling the picker,
viewing a subagent, and selecting the current ID leave the association unchanged.
Returning to an earlier ID produces an explicit resume transition, leaving epoch
allocation and conversation continuity to Logonomic.

This evidence establishes the selected native ID, not successful history
loading or a working/idle phase. Native Antigravity can select an ID and then
fail to read that conversation; the association follows that actual selection,
while inspection reports unavailable or unknown content honestly. Status-line
snapshots and directory contents never create an epoch. Unrecognized or
conflicting native records are not used as selection evidence.

Every selection captures a fresh activation nonce before helper delivery.
Subsequent hooks capture that activation before reading their callback payload
or doing asynchronous work, and the adapter checks it against the host's
current transition. A delayed hook from the first visit to A cannot affect A
after a later A→B→A sequence. Retries retain their original evidence and
idempotency key. Model-context requests carry the same originally captured
native ID and transition nonce through `model-context-delivery@2`. The host
checks that activation before consuming one-shot instructions or notices and
at its response claim boundary. A queued request that survives its helper
cannot consume a later activation's context. The helper also suppresses a
response after its source activation changes. Native selection capture and
publication continue independently of context requests.

An activated hook requires a host that negotiates `model-context-delivery@2`
for title, delegation, and notice injection. An older host refuses this
context request without consumption; the helper never retries it through the
unfenced version-one route. Native association observation remains available.
Existing launch shapes without the local selection observer retain their
version-one model-context behavior and its previous qualification boundary.

The private activation file contains only session/generation
binding metadata and selected identity; it contains no session capability,
prompt, transcript, or native log. Normal settlement removes it.

Live selection observation preserves native terminal streams, arguments,
permission posture, and exit status. It is enabled for recognized local launch
shapes whose literal environment and arguments can be preserved. A launch with
its own `--log-file`, a remote backend, or an unsupported shell expression does
not receive this observer. Existing remote hooks and exact remote resume remain
available; remote idle selection has not been qualified.

Resume relaunches a recorded conversation as `agy --conversation <ID>` in the
session workspace, through the package's bridge. The ID is the resume
record's `provider_session_id`, which a lifecycle observation supplies. Local
selection records also retain the resolved native home, so a saved association
cannot silently select the same ID in a different profile. Legacy ID-only
records remain accepted. The package does not select or isolate an account. The
launcher's literal assignments and options carry over. Any other command in the
launcher, an option that selects another conversation, runs a single prompt or
starts no session, a positional word, or a word or assignment a shell would
expand makes the launcher unresumable instead of approximated. Installed hooks report into a resumed conversation as into a
launch.

Current hosts also offer an authenticated `interactive-resume@2` startup
observer. The package's resume bridge starts the native CLI with one fresh
kernel-backed `--log-file` pipe with no persistent log artifact. A native conversation
stream followed by its completed redraw supplies observed loaded identity;
the requested selector or a saved database timestamp does not. Only those two
native records are sent to the observer. All observed loaded identities are
returned so the host can reject a conflicting conversation. No model request
is made to obtain this startup evidence. A launcher with its own `--log-file`
is refused rather than replacing the user's log destination. The bridge keeps
native stdin, stdout, stderr, options and exit status. After confirmation it
continues the bounded local selection observer when enabled and discards all
other native records. Startup verification remains stricter than an in-process
selection: it requires the exact requested conversation to complete its native
redraw before a resumed launch is accepted. Both buffers and parsing are bounded;
normal settlement or forceful custody termination releases the OS pipes without
leaving a transcript. A missing, over-bound or rejected observation
fails the resumed launch. Global hook ownership and all permissions are unchanged.

`interactive-resume@3` and `@4` provide exact, read-only inspection of a local
conversation in the frozen native scope. Version 3 reports availability;
version 4 separately reports content as empty, nonempty, or unknown. The reader
validates native root identity and the supported SQLite/protobuf structure.
Only a complete known empty root proves emptiness, and only valid native user
text proves nonempty content. Unknown schemas, unsupported message items,
malformed payloads, inaccessible history, and remote backends remain unknown.
Inspection never scans for another conversation or deletes history.

Inspection and resume preparation share the same exact identity and scope
check. A proven absent local conversation is unavailable. Unknown optional
inspection does not prevent an otherwise supported exact native resume; the
native startup observer still verifies the launched conversation. The private
SQLite VFS refuses native writes and creation of WAL/SHM files, while retaining
live-WAL visibility when native read-only sidecars permit it. A closed WAL
without usable sidecars remains unknown. On POSIX, inspection under effective
UID 0 returns unknown before opening SQLite because the native SQLite VFS may
otherwise change sidecar ownership. These limitations do not disable live
selection or exact native resume.

The package advertises no read-only structured review and no account
authentication observation.

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

Configure an out-of-source CMake build with the required platform toolchain.
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` accepts an explicit local
`sdk/logonomic_cli_addon_sdk` directory; without it, CMake obtains that public SDK
from the Logonomic repository's `master` branch. Windows compilation uses the
user-wide `queued-build` command and the applicable VS 2026/FASTBuild policy.

The default build produces `agy_adapter`, `agy_helper`, and the indexed package
under `package/logonomic.cli.agy` for a single-configuration generator, or
`package/<config>/logonomic.cli.agy` for a multi-configuration generator.
The `agy_provider_tests` target exercises native
recognition, quota, lifecycle, transcript privacy, read-only hook ownership, and
delegated preparation and inspection.
CTest also registers SDK package validation and conformance. Host integration
qualification must exercise the real authenticated helper and launcher service
alongside the package tests.

Native selection evidence was established with the official Antigravity 1.5.3
Linux executable, including idle A→B→A, same-ID reselection, cancelled and
empty pickers, subagent execution/view, and selection followed by a native
history-load failure. The maintained tests cover source-captured activation
fences, delivery retries, declared permission-snapshot routing, native startup
readiness, and read-only inspection. Native macOS, Windows, remote backends,
and full desktop Notes continuity require their corresponding integration
gates; package-level fixtures do not substitute for those gates.

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
