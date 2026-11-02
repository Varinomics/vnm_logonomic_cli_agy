# Agy status callback cost

## Managed callbacks (finding 63)

In an isolated Logonomic session the installed Agy status command took a median **140.65 ms** across nine callbacks. Four consecutive false snapshots all went through delivery, classification and acknowledgement. They added four journal entries and left the permission revision unchanged.

### Authenticated Linux session

The real session manager installed the packaged Agy adapter into a disposable profile. A version probe stood in for the CLI and a synthetic worker supplied callback JSON. A real PreInvocation hook established the conversation. Each subsequent sample ran the installed shell command to completion using the worker's session capability and negotiated provider generation. The host serviced IPC throughout. The callback had no user status command.

| Input, in order | Callbacks | Median wall time | Min–max | Accepted / acknowledged | Attention revision after group | Retained entries / accounted bytes after group |
|---|---:|---:|---:|---:|---:|---:|
| PreInvocation setup | — | — | — | — | 0 | 1 / 192 |
| False, false, false, false | 4 | 142.93 ms | 139.03–145.93 ms | 4 / 4 | 0 | 5 / 960 |
| True, false, true, true, false | 5 | 138.93 ms | 136.02–142.81 ms | 5 / 5 | 4 | 10 / 1,920 |
| All StatusLine callbacks | 9 | 140.65 ms | 136.02–145.93 ms | 9 / 9 | 4 | 10 / 1,920 |

Actual start-to-start spacing was 300.14–300.34 ms. The nine callbacks added 1,728 journal-accounted bytes (192 each). The byte counter is the journal's own accounting measure. The manager received exactly nine deliveries, nine result queries and nine acknowledgements. A separate traced replay recorded two shells, one observer, two `vnmctl` wrappers and three `logonomic_vnmctl_core` processes per callback, with three successful Unix-socket connections. The timing table uses the untraced replay.

The headless software scene graph could not initialize the terminal renderer. The successful run used the SDK's Xvfb/xcb/Mesa OpenGL backend with a private runtime directory and a common Qt socket directory for the host and child. This establishes callback cost for a synthetic state-change sequence on Linux. Actual Antigravity update frequency, steady-state CPU use and journal exhaustion remain unmeasured.

### Where the time goes

On provider `d84d8e5`, a complete StatusLine observation in a prepared generation with lifecycle delivery enabled gets a fresh key. The helper retries a transport failure or pending result with the same key and acknowledges a committed outcome. On Logonomic `b9e44f6`, a newly admitted delivery waits up to the 100 ms poll interval before its first result query. An already-committed response can take a shorter path. The host compacts acknowledged entries and retains them until expiry.

The first poll is the measured delay to investigate at the host boundary. A provider-side cache that drops repeated levels would need to preserve retries, distinguish a new binding or conversation from the preceding one, and deliver each genuine permission episode. These measurements establish the cost of the current path. They do not validate a suppression scheme.

### Linux provider measurement

The packaged adapter transformed empty settings in a disposable home. Each sample starts immediately before executing the installed command through `/bin/sh -c` and ends after it exits and its pipes close. Eight callbacks per row were scheduled 300 ms apart. The managed rows use the shipping observer and SDK client with the provider's immediate-acceptance `agy_fake_vnmctl` in place of the host wrapper. Captured events were then classified by the packaged adapter.

| Callback input | Median wall time | Min–max | Deliveries / acknowledgements | Classified levels |
|---|---:|---:|---:|---|
| Unmanaged Linux, no user command | 1.91 ms | 1.63–2.04 ms | 0 / 0 | — |
| Managed, eight consecutive false | 7.09 ms | 6.74–8.73 ms | 8 / 8 | Eight false |
| Managed, four true/false pairs | 7.31 ms | 6.48–11.58 ms | 8 / 8 | Four true/false pairs in order |

All 16 managed observations had distinct keys. A separate process trace recorded five processes in each managed sample (two shells, the helper and two fake-wrapper invocations) and two shells in the unmanaged sample. These times exclude the shipping host poll. Classification replay does not measure attention transitions or journal occupancy.

### Shipping wrapper and private core

The Linux transport probe runs the installed callback, packaged helper, production `vnmctl` and its private core. A disposable Unix socket checks the token, session capability, provider binding, delivery key and sanitized event. It replies immediately to each request. Both series use the same sequence of four false levels followed by two true/false pairs. Each callback has a fresh key. Wall time includes collection of peer process ancestry while handling the requests.

| Scripted first response | Callbacks | Median wall time (min–max) | IPC actions per callback | Observed client processes |
|---|---:|---:|---|---:|
| Admitted, then committed at the first query | 8 | 136.90 ms (128.07–148.29) | deliver, result, acknowledge | 8 |
| Already committed (experimental control) | 8 | 23.46 ms (19.88–30.89) | deliver, acknowledge | 7 |

In the admitted case the endpoint observed two shells, one helper, two wrappers and three private cores in each callback's process ancestry. It samples this ancestry once per request, so shorter-lived processes outside those samples would be missed. The median interval between the endpoint reading delivery and reading the result query was 107.85 ms. The synthetic committed answer lets the same wrapper skip that query. A fresh event on the real host still requires classification. The difference between these rows does not establish achievable savings. The socket fixture has no attention state or journal.

```sh
python3 tests/measure_linux_status_transport.py "$provider_build/agy_provider_tests" \
    "$provider_build/package/logonomic.cli.agy" "$host_bin/vnmctl"
```

Use the production `vnmctl` next to its `logonomic_vnmctl_core`. Both probes print per-sample measurements and impose no performance threshold.

### Linux verification

Provider `d84d8e5` was built in standalone Release mode with GCC 13.3, Logonomic SDK source from `b9e44f6` and the external Linux kit `2026.10.23.1`. The kit archive matched the pinned SHA-256 and SDK preflight passed. CMake used the C and C++ ccache launchers, and a repeated object compilation hit the persistent cache. Configure used one queued slot and the provider build used four. All 29 indexed package files matched their staged sizes and hashes.

The registered `agy_provider_tests`, `agy_helper_tests`, `agy_package_validation` and `agy_package_conformance` passed under the headless runner. The packaged `--permission-snapshot` and `--status-line` selectors also passed individually. The instrumented host's source inventory verified all 31 entries. Its authenticated replay passed in both timing and process-trace runs, with no skips.

### Recorded host-test failure

The earlier Linux run recorded one failing host test, `permission_snapshot_routes_authenticated_ordered_journal_events`. It failed in common setup before exercising its permission episodes. This failure remains an open finding owned by Logonomic; it is not a passing permission gate. The Windows work below did not rerun that host test, and its original raw Linux failure log was not available for independent inspection here.

Source inspection at the recorded Logonomic `b9e44f6` and the Windows SDK source revision `5e1aed0c` agrees with the recorded cause. In `tests/logonomic_session_service_journal_tests.cpp`, common setup already checks admission, committed result, projection and lifecycle state before line 845 waits for `Logonomic helper ingress: admitted` plus the delivery key. Line 854 also waits for a successful-commit debug message. In `src/logonomic_session_service_provider_journal.cpp`, successful insertion, dispatch and admission at lines 257–273 emit no admission message, and the commit path at lines 749–765 logs refusal rather than successful commitment. The permission-specific test starts at line 873, after those setup waits.

The concrete host-owned follow-up is to replace both stale success-log waits with the actual admission response, result or journal state. Keep the authentication, ordering, epoch, replay and permission assertions intact. The separate P16 replay's recorded counters observed all nine admissions; that is scoped replay evidence, not a substitute for fixing and rerunning the failing host test. This checkpoint changes no Logonomic-owned source or permission checks.

## Unmanaged Windows callbacks (finding 64)

The installed Windows command starts PowerShell under Antigravity's command shell. PowerShell opens the retained settings file before it can determine that an unmanaged session has no original command to run. With the native status line disabled Antigravity does not invoke the command. The documented 300 ms minimum interval between invocations cannot establish how often an actual session changes state. [Antigravity status line](https://antigravity.google/docs/cli/statusline/)

Run the following with a native Windows package and the provider test executable from the same build.

```powershell
py tests/measure_windows_unmanaged_status.py "$providerBuild/agy_provider_tests.exe" "$providerBuild/package/logonomic.cli.agy"
```

The probe uses the packaged settings transform in a disposable profile. It compares complete installed callbacks to the unwrapped case in which the CLI has no external command to dispatch. Each sample reports elapsed time, descendant process starts and job CPU including the Python driver. The unwrapped elapsed value measures only the driver's empty branch. The probe also adopts a later command with a quoted path and compares stdin, stdout and exit status with direct invocation. Run the existing native `agy_helper_tests` alongside it for managed forwarding and command-length coverage.

### Native Windows result, 2026-11-02

The supplied probe completed with its default 12 paired samples on native Windows build 22631, using Python 3.13.7 and a package and provider executable from the same fresh standalone Release build.

| Path | Samples | Median wall time | Min–max | Process starts per sample | Median job CPU, including driver (min–max) |
|---|---:|---:|---:|---:|---:|
| Unwrapped, no external command | 12 | 0.0022 ms | 0.0005–0.0047 ms | 0 | 0 ms (0–0) |
| Installed unmanaged callback | 12 | 663.09 ms | 477.37–738.90 ms | 2 | 31.25 ms (0–140.625) |

Every installed sample exited zero with empty stdout. Each also emitted 616 stderr bytes; the supplied probe records their count rather than their content. A separate isolated reproduction of the same installed command also produced 616 bytes, consisting of two Windows PowerShell CLIXML progress records saying `Preparing modules for first use.`, with no error record. This supports module preparation as part of the disposable-profile measurement without proving the content of each original sample's stderr. Job CPU was reported in 15.625 ms increments in this run; a zero delta does not establish zero CPU cost. The no-command row measures the Python driver's empty branch, not a CLI invocation.

The replacement-command oracle passed: the packaged transform adopted a later command whose quoted path contained a space, apostrophe and ampersand; the resulting command still traversed the installed wrapper, retained the new original, and matched direct invocation's exact UTF-8/CRLF stdin bytes, stdout and exit status 37.

Other queued builds were active during the native run. These observations establish the cost of this disposable-profile callback path under that workload. They do not establish warmed-session latency, actual Antigravity callback cadence, steady-state CPU use, or a production optimization's savings. The native probe imposes no performance threshold.

### Native build and focused contracts

The provider checkout was PR 7's `d5d3ec9`, whose production sources match `d84d8e5`. Its SDK came from the unchanged `sdk/logonomic_cli_addon_sdk` subtree of Logonomic `5e1aed0c`. This was a standalone provider build, not a full Logonomic build. It has its own `CMakeCache.txt`; it does not claim Logonomic entrypoint build-origin or source-inventory records.

Visual Studio 2026 Community 18.11.12328.437 reported a complete, launchable installation. CBS and Windows Update reboot indicators were clear; `PendingFileRenameOperations` was present and did not prevent the build. Configure and build both ran after `vcvarsall.bat x64 -vcvars_ver=14.51`. CMake 4.4.2 selected MSVC 19.51.36260.0, with both compiler paths under `C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231`. No Qt build was needed.

Configure used one queued slot and the FASTBuild generator with `CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV=OFF`. FASTBuild 1.20 used the persistent object cache and the required two-slot queued `-dist -nolocalrace` build; the queue selected XENIA alone with `-forceremote -j2`. XENIA's preflight connection succeeded. The build reported literal `FBuild: OK:` lines for `agy_provider_tests`, `agy_fake_vnmctl`, `logonomic_cli_addon_validate` and `logonomic_cli_addon_conformance`, with 75 cache misses and 75 stores, 2 min 9.65 s wall time and 8 min 13.57 s remote CPU time.

The first configure attempt failed because combined, unquoted `-DNAME=C:/...` arguments were split by the queue's PowerShell invocation. Separate `-D "NAME=C:/..."` arguments configured a new tree successfully; no package code changed. FASTBuild also reported that its shared monitor log could not be opened, but completed the distributed build. Existing compiler warnings were retained in the build log; none prevented compilation.

| Native gate | Result |
|---|---|
| `agy_provider_tests` | Passed, 4.11 s |
| `agy_helper_tests` | Passed, 29.17 s |
| `agy_package_validation` | Passed, 0.05 s |
| `agy_package_conformance` | Passed, 0.58 s |
| Packaged `--permission-snapshot` selector | Passed |
| Packaged `--status-line` selector | Passed |

All four selected CTests passed, with no skips. The helper contracts exercised authenticated managed forwarding and acknowledgement, retry identity, current-generation marker matching, stale/missing marker rejection, sanitized true/false permission snapshots, invalid-observation rejection, byte-preserving pass-through, retained user-command output and exit status, and the Windows 1,500/7,000-character command cases in managed and unmanaged environments. The Windows arbitrary-JSON-member cases also passed. These are provider/helper contracts using the fake wrapper; they do not claim that the recorded host permission test passed.

Rerun the native contracts and selectors after building the same provider/package pair:

```powershell
ctest --test-dir $providerBuild -R '^(agy_provider_tests|agy_helper_tests|agy_package_validation|agy_package_conformance)$' --output-on-failure
& "$providerBuild/agy_provider_tests.exe" "$providerBuild/package/logonomic.cli.agy" --permission-snapshot
& "$providerBuild/agy_provider_tests.exe" "$providerBuild/package/logonomic.cli.agy" --status-line
```

Raw native configure, build, CTest, selector and probe output is retained outside the repository at `C:/plms/varinomics/_agent_reports/agy_pr7_20261102_native`. The table and findings above preserve the results without requiring those workstation-local logs. Production callback optimization is outside this checkpoint; no production behavior changed.
