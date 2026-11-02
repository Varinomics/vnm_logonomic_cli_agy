# Agy status callback cost

## Managed callbacks (finding 63)

On provider `d84d8e5`, a complete StatusLine observation in a prepared generation with lifecycle delivery enabled gets a fresh delivery key, even when the pending level is unchanged. The helper retries a transport failure or pending result with the same key and acknowledges a committed outcome. On Logonomic `b9e44f6`, a matching snapshot can be accepted without changing the attention revision. The new-admission path in the shipping wrapper waits up to the 100 ms poll interval before its first result query. A replay that already has a committed outcome can take a shorter path.

The host retains an acknowledged journal entry until expiry. Source inspection alone does not establish journal exhaustion, a missed permission episode, or the time paid by a callback. The existing provider fake returns a committed acceptance immediately. The host journal test submits directly to the manager. Each omits work done by the other side of the installed callback.

For the managed measurement, run repeated false snapshots followed by true/false episodes on one admitted conversation. Record callback wall time through shell exit, wrapper and core calls, committed attention changes, and retained entries and bytes before and after. Keep the same conversation, generation and session throughout a series. The current helper assigns a distinct key to a new observation with the same level. A retry of a lost response reuses the original key. Skipping callbacks would require a design for failed delivery, rebinding and generation changes.

## Unmanaged Windows callbacks (finding 64)

The installed Windows command starts PowerShell under Antigravity's command shell. PowerShell opens the retained settings file before it can determine that an unmanaged session has no original command to run. With the native status line disabled Antigravity does not invoke the command. The documented 300 ms minimum interval between invocations cannot establish how often an actual session changes state. [Antigravity status line](https://antigravity.google/docs/cli/statusline/)

Run the following with a native Windows package and the provider test executable from the same build.

```powershell
py tests/measure_windows_unmanaged_status.py <path-to-agy_provider_tests.exe> <path-to-package-directory>
```

The probe uses the packaged settings transform in a disposable profile. It compares complete installed callbacks to the unwrapped case in which the CLI has no external command to dispatch. Each sample reports elapsed time, descendant process starts and job CPU including the Python driver. The unwrapped elapsed value measures only the driver's empty branch. The probe also adopts a later command with a quoted path and compares stdin, stdout and exit status with direct invocation. Run the existing native `agy_helper_tests` alongside it for managed forwarding and command-length coverage.

At this checkpoint the Windows probe has passed Python syntax and argument/guard checks on Linux. Native Windows timing, real CLI cadence, and the authenticated Linux host measurements are pending. There is no production change.
