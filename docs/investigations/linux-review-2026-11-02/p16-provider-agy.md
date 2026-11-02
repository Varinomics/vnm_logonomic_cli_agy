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

One additional existing host test, `permission_snapshot_routes_authenticated_ordered_journal_events`, failed in its common setup before exercising the permission episodes. It waits for a debug message containing `Logonomic helper ingress: admitted` and the delivery key. The current host's successful admission path inserts the entry and returns the admitted response without emitting that message. The explicit host counters and state assertions in the separate P16 replay observed all nine admissions. The shared host test should use the admission response or journal state as its oracle.

## Unmanaged Windows callbacks (finding 64)

The installed Windows command starts PowerShell under Antigravity's command shell. PowerShell opens the retained settings file before it can determine that an unmanaged session has no original command to run. With the native status line disabled Antigravity does not invoke the command. The documented 300 ms minimum interval between invocations cannot establish how often an actual session changes state. [Antigravity status line](https://antigravity.google/docs/cli/statusline/)

Run the following with a native Windows package and the provider test executable from the same build.

```powershell
py tests/measure_windows_unmanaged_status.py "$providerBuild/agy_provider_tests.exe" "$providerBuild/package/logonomic.cli.agy"
```

The probe uses the packaged settings transform in a disposable profile. It compares complete installed callbacks to the unwrapped case in which the CLI has no external command to dispatch. Each sample reports elapsed time, descendant process starts and job CPU including the Python driver. The unwrapped elapsed value measures only the driver's empty branch. The probe also adopts a later command with a quoted path and compares stdin, stdout and exit status with direct invocation. Run the existing native `agy_helper_tests` alongside it for managed forwarding and command-length coverage.

The Windows probe passed Python syntax, argument and platform-guard checks on Linux. An isolated driver check also rejected a direct-command bypass and stale retention. The remaining native gate is to run this probe and the helper contract tests on Windows, then evaluate startup cost with an observed CLI callback cadence. There is no production behavior change in this PR.
