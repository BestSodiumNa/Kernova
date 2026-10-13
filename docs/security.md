# Security

This document describes the security-relevant aspects of Kernova: how it
handles privileged operations, what it does on the network, and how to report
a vulnerability.

## Reporting

Open an issue on the GitHub repository. Please describe the steps to
reproduce and the version (the app doesn't display a version number yet; the
build date in the file properties is sufficient).

## Privileged operations

Kernova runs unelevated by default. A few operations require administrator
rights, and those are performed by re-launching the app elevated with a
specific command-line flag, doing one thing, and exiting. The UI itself never
runs elevated unless the user explicitly restarts it that way.

The three elevated helpers are:

- `--set-dns <guid> <v4-list> <v6-list>` — changes the DNS servers on the
  network adapter with the given GUID. `<guid>` must parse as a GUID via
  `IIDFromString`, and each entry in the two lists must parse as an IPv4 or
  IPv6 address via `InetPton`. An empty list is `-`.
- `--optimize <letter> <analyze|optimize> <path>` — runs `defrag.exe` on the
  given drive. `<letter>` must be a single alphabetic character, the second
  argument must be exactly `analyze` or `optimize`, and `<path>` must end in
  `.mtmopt`. The path is used as the name of a temp file that the elevated
  process appends its output to; the UI tail-reads that file for progress.
- `--power <subcommand>` — three fixed shapes: `parking on|off`,
  `preset <digit>`, or `delete <guid>`. The GUID must parse via
  `IIDFromString`; the digit must be a single character in `0`–`9`.

If you find a way to make any of these do something outside its documented
shape — especially if it lets you pass a path, a shell command, or an
arbitrary registry key — please report it. The parsers are intended to be
strict; anything they accept beyond what's listed above is a bug.

## Shell execution

The Installed Apps page has a "Uninstall" action that runs the uninstaller
registered in the Windows Uninstall registry. This is exactly as dangerous
as running the same uninstaller from Settings → Apps, no more and no less:
the app doesn't construct the command line, it reads it from the registry.
The user is asked to confirm before it runs.

If the uninstaller requires elevation, the app launches it via
`ShellExecuteEx` with the `runas` verb, which shows the standard UAC prompt.
The elevated uninstaller runs in its own process; Kernova does not run
elevated as a result.

## Network activity

Kernova never opens a listening socket. It makes outbound network requests
only when the user clicks a button that requires them:

- **Speed test** (Connections → Speed test) contacts `speed.cloudflare.com`
  using Cloudflare's public test endpoints. This transfers up to a few
  hundred megabytes per run.
- **DNS latency test** (Connections → DNS → Test latency) sends one DNS
  query per listed provider to that provider. The query is for
  `www.google.com` and does not reveal anything about the user beyond what
  any DNS query would.
- **Search online** (process context menu) opens Bing in the user's default
  browser.
- **ipinfo.io lookup** (Connections context menu) opens a page in the user's
  default browser.

Nothing else. No telemetry, no analytics, no crash reporting, no
auto-update check, no background polling of any endpoint.

## Local data

The app writes to two places:

- `HKCU\Software\Kernova` — settings. The schema is documented in
  `docs/formats.md`.
- `%LOCALAPPDATA%\Kernova\benchmarks.dat` — benchmark history.

Trace files are written only when the user presses "Start recording" or
"Save buffer," and only to a path the user chooses.

On first run, if `HKCU\Software\Kernova` doesn't exist and either
`HKCU\Software\BetterTaskManager` or `HKCU\Software\MyTaskManager` does, the
newer of the two is copied to the new key. This is a one-time migration from
earlier builds of the same app.

## File inputs

The trace loader (`.mtmtrace`) and the benchmark history loader
(`benchmarks.dat`) both parse user-supplied files. Both use bounds-checked
readers, and both reject malformed input with a specific error rather than
crashing or reading past the end of the buffer. The trace loader additionally
CRCs each record and refuses to continue past a corrupt one. If you find a
file that causes either loader to misbehave, please report it — those are
the two attack surfaces most worth fuzzing.

## Known non-issues

Things that look like vulnerabilities but aren't, so you can save yourself
the report:

- **The app can terminate processes.** That's the point of a task manager.
  It requires `PROCESS_TERMINATE` on the target, which the user already has
  for their own processes.
- **The app can read drive health.** This requires opening
  `\\.\PhysicalDriveN`. It does not require or grant access to the data on
  the drive, only to the drive's own status.
- **The app can change DNS.** This requires elevation, is a documented
  feature, and is subject to the same strict argument validation as the
  other helpers.
- **The app reads the Uninstall registry key.** That key is readable by any
  user and doesn't contain secrets.
- **The app uses undocumented APIs.** `uxtheme` ordinal 135,
  `NtQuerySystemInformation`, and `DnsFlushResolverCache` are all widely
  used, stable for many Windows versions, and don't grant elevated access.