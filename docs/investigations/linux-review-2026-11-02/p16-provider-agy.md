# Agy status callback cost

## Managed callbacks (finding 63)

On provider `d84d8e5`, a complete StatusLine observation in a prepared generation with lifecycle delivery enabled gets a fresh delivery key, even when the pending level is unchanged. The helper retries a transport failure or pending result with the same key and acknowledges a committed outcome. On Logonomic `b9e44f6`, a matching snapshot can be accepted without changing the attention revision. The new-admission path in the shipping wrapper waits up to the 100 ms poll interval before its first result query. A replay that already has a committed outcome can take a shorter path.

The host retains an acknowledged journal entry until expiry. Source inspection alone does not establish journal exhaustion, a missed permission episode, or the time paid by a callback. The existing provider fake returns a committed acceptance immediately. The host journal test submits directly to the manager. Each omits work done by the other side of the installed callback.

### Linux provider measurement

The packaged adapter transformed empty settings in a disposable home. Each sample starts immediately before executing the installed command through `/bin/sh -c` and ends after it exits and its pipes close. Eight callbacks per row were scheduled 300 ms apart. The managed rows use the shipping observer and SDK client with the provider's immediate-acceptance `agy_fake_vnmctl` in place of the host wrapper. Captured events were then classified by the packaged adapter.

| Callback input | Median wall time | Min–max | Deliveries / acknowledgements | Classified levels |
|---|---:|---:|---:|---|
| Unmanaged Linux, no user command | 1.91 ms | 1.63–2.04 ms | 0 / 0 | — |
| Managed, eight consecutive false | 7.09 ms | 6.74–8.73 ms | 8 / 8 | Eight false |
| Managed, four true/false pairs | 7.31 ms | 6.48–11.58 ms | 8 / 8 | Four true/false pairs in order |

All 16 managed observations had distinct keys. A separate process trace recorded five processes in each managed sample (two shells, the helper and two fake-wrapper invocations) and two shells in the unmanaged sample. These times exclude the shipping host poll. Classification replay does not measure attention transitions or journal occupancy.

### Linux verification

Provider `d84d8e5` was built in standalone Release mode with GCC 13.3, Logonomic SDK source from `b9e44f6` and the external Linux kit `2026.10.23.1`. The kit archive matched the pinned SHA-256 and SDK preflight passed. CMake used the C and C++ ccache launchers, and a repeated object compilation hit the persistent cache. Configure used one queued slot and the provider build used four. All 29 indexed package files matched their staged sizes and hashes.

The registered `agy_provider_tests`, `agy_helper_tests`, `agy_package_validation` and `agy_package_conformance` passed under the headless runner. The packaged `--permission-snapshot` and `--status-line` selectors also passed individually.

## Unmanaged Windows callbacks (finding 64)

The installed Windows command starts PowerShell under Antigravity's command shell. PowerShell opens the retained settings file before it can determine that an unmanaged session has no original command to run. With the native status line disabled Antigravity does not invoke the command. The documented 300 ms minimum interval between invocations cannot establish how often an actual session changes state. [Antigravity status line](https://antigravity.google/docs/cli/statusline/)

Run the following with a native Windows package and the provider test executable from the same build.

```powershell
py tests/measure_windows_unmanaged_status.py <path-to-agy_provider_tests.exe> <path-to-package-directory>
```

The probe uses the packaged settings transform in a disposable profile. It compares complete installed callbacks to the unwrapped case in which the CLI has no external command to dispatch. Each sample reports elapsed time, descendant process starts and job CPU including the Python driver. The unwrapped elapsed value measures only the driver's empty branch. The probe also adopts a later command with a quoted path and compares stdin, stdout and exit status with direct invocation. Run the existing native `agy_helper_tests` alongside it for managed forwarding and command-length coverage.

The Windows probe passed Python syntax, argument and platform-guard checks on Linux. An isolated driver check also rejected a direct-command bypass and stale retention. Native Windows timing and actual CLI cadence remain open. The authenticated Linux host measurement is in progress. There is no production change.
