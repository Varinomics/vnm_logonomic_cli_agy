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
current title/delegation instruction. An accepted instruction is emitted as an
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

Antigravity exposes no exact same-account conversation selector, delegated task
interface, read-only structured review, account authentication observation, or
usage-history scanner in the adopted native integration. The package does not
advertise those capabilities. Imported conversation identities remain history
records, not authority to resume a conversation.

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
