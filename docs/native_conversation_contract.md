# Native conversation integration

This reference describes the Antigravity adapter's identity, history, and
execution-directory boundaries. Capability negotiation selects the supported
protocol; a native version used as evidence is not a compatibility ceiling.

## Selection and activation

For a sole literal local interactive launch with lifecycle integration, the
package keeps its owned `--log-file` pipe open and recognizes the native root
`conversation_manager.go` streaming record. The accepted record names the
selected conversation. Text typed into the terminal, directory timestamps,
child streams, and status-line snapshots do not create a conversation epoch.

Native `/clear` and selection through `/resume`, `/switch`, or `/conversation`
can change that root without another model invocation. The activity hooks
alone cannot detect such an idle change. The documented `SessionStart` hook
runs before a new conversation's first model invocation, rather than serving
as an idle selection callback. See the native
[hook contract](https://antigravity.google/docs/hooks#sessionstart).

A new selected identity produces a transition with its exact native resume
record. Returning to an earlier identity produces `RESUME`; selecting the
current identity is idempotent. Opening or cancelling a picker has no effect
unless the native root-selection record actually changes. The record proves
selection, not successful loading of the conversation's stored history or a
working/idle phase. A native history-load error therefore does not justify
silently preserving the previous identity.

The bridge captures a new activation nonce before delivering a selection.
Subsequent hooks capture that activation before reading their callback input.
The adapter checks both native identity and nonce against the host's current
transition. This distinguishes delayed events from the first visit to A in an
A → B → A sequence. Retries keep their original evidence and idempotency key.
Status-line permission observations remain restricted to the current activation.

Activated model-context requests require `model-context-delivery@2`. The host
checks the captured identity and nonce before consuming one-shot instructions
or notices and when claiming a response. The helper also suppresses a response
after its source activation changes. An older host refuses this context request;
the helper never retries it through an unfenced version-one request. Launch
shapes without the local selection observer retain their version-one behavior.

The bridge preserves the native terminal streams, arguments, permission posture,
and exit status. It does not replace a launcher's own `--log-file`, wrap a remote
backend, or approximate an unsupported shell expression. Such launches retain
their supported activity-hook path; they have no idle-selection guarantee.
The private activation file contains generation/session binding metadata and the
selected identity, without the session capability or conversation text, and is
removed at normal settlement.

When the original native process exits, the bridge makes one bounded,
nonblocking drain of buffered log records and gives captured selections one
shared five-second settlement window. In-flight and queued selections retain
their original order, keys, and activation nonces. A stalled host is cancelled
through the existing helper custody path when that window expires. The bridge
preserves the native exit status and does not wait for a descendant to close the
logger. Abnormal startup/transport exits retain immediate cancellation.

## Native model observation

The optional `model-state@1` capability samples the root's native
`PreInvocation.modelName`. The [native hook contract](https://antigravity.google/docs/hooks#preinvocation)
identifies that value as the model handling the invocation. The helper captures
the existing private activation before stdin and retains the current root
identity guard. It emits no model observation for an unfenced launch or another
conversation's callback. Backend identity remains absent when the native hook
does not report it.

The native `(initialNumSteps, invocationNum)` tuple supplies source order,
encoded into uint64 using two uint32 components. Missing, malformed or larger
counters omit the optional fact. A trajectory rewind therefore cannot replay an
older model with newer authority. Sampling occurs at invocation boundaries;
idle selection/configuration changes are not current execution-model evidence.

This route does not infer a child's model from root hooks or subagent type.
Child and structured execution models remain unconfirmed without their own
native serving-model observations. Model reporting adds no lifecycle events or
Session Log rows. `agy_model_tests` checks the source contract and optional
envelope isolation; it does not substitute for native CLI or owner visual gates.

### Executed native model check (Linux 1.5.9, 2026-11-16)

The official Linux CLI archive was downloaded from the URL named by Google's
installer manifest and verified against that manifest's SHA-512. Its actual
headless process completed a request to a loopback Gemini-compatible endpoint
with a fixture API key, a fresh isolated home mount, and the updater disabled.
No account session or remote model service was used.

The configured display label was deliberately different from the native model
identifier. The resulting evidence distinguishes the two paths:

| Native output | Observed value |
|---|---|
| `PreInvocation.modelName` | `gemini-api://generativelanguage.googleapis.com/models/native-root-model` |
| `PreInvocation.invocationNum` | `0` |
| `PreInvocation.initialNumSteps` | `1` |
| Structured `init.model` | `selected-alias` |

The actual production `agy_helper` consumed that native invocation callback and
delivered the same literal model with the root's captured activation nonce,
empty child identity, and native order `4294967296`. The endpoint's reply was
observed in the CLI's successful final result. The helper's capture transport
and initial committed root activation were fixtures; this check does not cover
the production selection bridge, host admission, desktop badges, or native
child execution.

The structured init field is demonstrably a selected display label. It remains
excluded from structured serving-model observations. Child and structured cells
therefore remain unconfirmed, while the native root hook/helper path has been
exercised.

[`agy_native_model_probe.py`](../tests/agy_native_model_probe.py) preserves this
Linux gate. It requires `bwrap`, an official native CLI, the built production
helper, and a new output directory. It keeps the home selector unchanged while
mounting an empty private home and writing only fixture data. The native process
stays under the probe's process custody.

```sh
queued-build --slots 1 -- python3 tests/agy_native_model_probe.py \
  --cli /path/to/agy \
  --helper /path/to/build/agy_helper \
  --output-directory /path/to/new/native-model-probe
```

The production adapter/helper build and registered `agy_model_tests` also pass
with the official Linux development SDK and the corresponding Logonomic SDK
extension. Full package conformance and owner visual acceptance remain separate
gates.

## Exact resume and read-only inspection

`interactive-resume@2` prepares `agy --conversation <ID>` through the packaged
bridge and acknowledges the exact opaque resume record. Local selection records
also retain the resolved native home. Inspection and preparation share that
identity and scope check, so an enriched record cannot silently select the same
raw ID under another home. Existing ID-only records remain accepted under their
frozen host context. The package does not select or isolate an account.

The launcher's literal assignments and compatible options carry over. Additional
shell commands, expansion-dependent words, prompt-only options, or options that
select another conversation make the exact recipe unavailable. The provider
does not substitute a nearby launcher or conversation.

When the host offers authenticated startup observation, a matching native stream
record followed by completed redraw establishes loaded identity. The requested
selector and database timestamps do not. This cold-resume check is stronger
than the in-process selection observation. It makes no model request, retains
no native log artifact, and refuses missing, conflicting, or over-bound evidence.

`interactive-resume@3` reports availability, and `@4` independently reports
content. Both inspect only the exact local conversation database in the resolved
native scope and acknowledge the complete inspection request. Version 3 responses
omit the content field. Native availability does not imply nonempty content.

Inspection validates the launcher's literal option shape before interpreting
the local/remote switch. Empty or expansion-dependent arguments leave
availability and content unknown. Ordinary launch preparation keeps an
unsupported invocation intact without installing the local selection observer;
exact resume preparation continues to refuse an unsupported recipe.

The supported SQLite/protobuf structure must establish the exact root identity.
A complete recognized root with no steps or parent references proves `EMPTY`;
valid retained native user text proves `NONEMPTY`. Unsupported schemas or message
items, malformed/partial records, inaccessible history, uncertain replay, and
remote backends remain `UNKNOWN`. Titles, usage totals, byte counts, and missing
hook observations never prove emptiness. Inspection never deletes native history
or the saved resume record, so a later completed native write can change its
answer.

The private SQLite VFS refuses native writes and creation of WAL/SHM files. It
retains live-WAL visibility when the existing read-only sidecars support it.
A closed WAL without usable sidecars remains unknown. POSIX inspection under
effective UID 0 remains unknown before SQLite is opened because the native VFS
may otherwise change sidecar ownership. Proven local absence refuses preparation;
an unknown optional inspection does not veto an otherwise supported exact resume.

## Execution directory and project scope

The package does not advertise `execution-context@1`. Its native selection
evidence confirms an identity without confirming the CLI's effective default
execution directory. Current cwd therefore stays unknown rather than being
filled from launch input, origin provenance, native storage paths, or a command's
directory override.

The documented status-line `cwd` describes the startup directory. Its
`workspace.current_dir` and `workspace.project_dir` fields have no documented
accepted-default-directory or activation-ordering guarantee. They are display
metadata, not sufficient evidence for this capability. See the native
[status-line fields](https://antigravity.google/docs/cli/statusline#available-json-fields).
Likewise, hook `workspacePaths` identifies mounted workspaces, not a single
default execution directory. See the native
[common hook fields](https://antigravity.google/docs/hooks#common-input-fields).

Native resume selects the conversation's associated project, both through
startup `--conversation` and through `/resume`. A project can span multiple
folders. Consequently, the process spawn directory alone does not prove the
directory accepted by a resumed conversation. The adapter preserves the exact
identity recipe but does not promise keep-current, restore-origin,
last-confirmed, or alternate-directory behavior. See the native
[project resume behavior](https://antigravity.google/docs/projects?tab=cli#resuming-an-existing-conversation).

A directory change alone cannot invent a conversation, change Notes identity,
or overwrite immutable origin. Cross-directory identity equivalence requires
separate verification of provider, account, profile, native store/backend, and
environment scope. Matching a raw native ID or a path is insufficient.

## Exclusive ownership boundary

The root-selection record arrives after the native CLI has selected a
conversation. Rejecting its reporting hook cannot prevent that native selection.
The adapter has no qualified native preselection veto or ownership reservation
that safely resolves a selection racing another live owner or a host resume.

Read-only inspection of the Windows x86_64 Antigravity 1.4.11 executable's embedded
diagnostics and its 1.1.10 changelog identifies duplicate-open detection as a
non-blocking advisory banner. Its warning describes possible conflicts when
both sessions send messages; it does not establish exclusive admission. This is
binary inspection, not an exercised duplicate-open interaction. Native warning
presence is not evidence of ownership enforcement.

The public [CLI v1.1.10 changelog](https://antigravity.google/docs/changelog?tab=cli)
also describes duplicate-open handling as a non-blocking advisory. This
corroborates the limitation without relying solely on executable strings; it
does not qualify a live ownership conflict or provide a preselection veto.

One live owner remains the required host contract. Defensive owner bookkeeping
and truthful selection reporting do not by themselves satisfy it. The adapter
does not claim complete manual-selection ownership support until an effective
native prevention or safe resolution control is established.

## Native evidence boundary

The primary documentation above specifies the public hook, status-line, and
project semantics. Windows x86_64 Antigravity 1.4.11 `--version` and `--help`
confirm the installed native entry point and its `--conversation` and `--log-file`
options. Neither help output nor embedded binary strings validate a real picker
transition, effective directory change, ownership conflict, or desktop Notes
association.

A fresh isolated Linux x86_64 probe on 2026-11-08 verified the official
Antigravity 1.5.3 binary, its help/version output, API-key provider startup, and
disabled updater. It reached the theme chooser and then Terms of Service &
Data Use. Consent was neither accepted nor toggled, and the fixture process
was settled. No prompt, slash command, root selection, model-endpoint request,
or host association was exercised. This remains an unavailable onboarding gate,
not evidence that native selection is unsupported.

The pending native matrix includes idle `/clear`/`/new`, successful `/resume`
selection and cancellation, A → B → A, startup `--conversation`, and project
selection. The official [1.5.0 CLI changelog](https://github.com/google-antigravity/antigravity-cli/blob/main/CHANGELOG.md)
states that switching through `/projects` starts a new conversation while
editing a project's folders is a separate action. The project contract also
defines `/fork <project_id>` as creating a new conversation. These documented
paths need real native evidence; they do not establish a mutable default cwd.

Provider/SDK fixtures establish parser, negotiation, acknowledgment, and
activation-fence behavior. Actual CLI interactions and host/runtime behavior
require their own platform-specific evidence. Local selection support must not
be presented as remote idle-selection support, and identity confirmation must
not be presented as directory confirmation.
