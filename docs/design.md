# Design notes

This document is the *why*. It's for reviewers, for people deciding whether to
use the app, and for you six months from now when you've forgotten why you
made a particular choice.

## Motivation

I wanted a Windows system monitor that:

- Showed per-core usage **and** per-core clock side by side, because the two
  together tell you whether a busy core is boosting or throttling.
- Showed drive health â€” SMART and NVMe â€” without hiding it behind a wizard.
- Respected the system light/dark theme and DPI, without looking like it was
  built in 2009.
- Was comfortable to extend. Adding a page should be an afternoon's work, not
  a week of fighting the framework.

The existing free tools each do some of that. Task Manager does per-core
clock but hides SMART. Process Explorer does deep inspection but has no
per-core clock view. CrystalDiskInfo does drive health well but nothing else.
HWiNFO does everything but its UI is a wall of numbers. None of them were
comfortable to extend.

Kernova is what I ended up building for my own use. It is not a Task Manager
replacement in the sense of "identical feature set." It has features Task
Manager doesn't (drive health, disk space, flight recorder, benchmarks) and
lacks features Task Manager has (per-process network I/O, the app history
view, UWP process grouping). It is what it is.

## What's original here

Most of the code is standard Win32 and standard Windows APIs, and it would be
dishonest to claim otherwise. But a few things are less common and worth
pointing at:

### The layered widget model

`Widget` splits painting into a static layer (card background, title, big
value, grid lines) and a dynamic layer (the scrolling series, the animating
bars, the live read-out). The static layer is rendered once into an offscreen
surface and cached; the dynamic layer is drawn on top every frame. It's a
pattern more common in games and browser engines than in Win32 apps, and it's
why the graphs can animate at 60 fps without burning CPU. A 16-core CoreGrid
with a 60 fps animation tick costs a small fraction of what re-rendering the
labels every frame would.

### Spring physics instead of exponential easing

`CoreGrid` uses a critically damped spring rather than exponential easing for
its bar and read-out motion. Exponential easing restarts at full speed every
time a new sample lands, which reads as a jerk once a second. The spring
carries its velocity across samples, so motion is continuous and settles
without overshoot. The spring rate scales with the update interval, so it
takes about one interval to arrive regardless of whether the user has chosen
0.5 s, 1 s, or 2 s. This is a small thing; it reads as a large difference in
use.

### The `.mtmtrace` format

Compact, self-describing, CRC-checked, and memory-mappable. Most "record a
trace" features in monitors are either CSV (huge, slow to parse) or a
proprietary blob (opaque, unreadable by anything else). The trace format here
is a few pages of specification (see `formats.md`), and a trivial reader can
be written in any language in an afternoon.

### The four-view Uninstall-registry merge

Windows stores installed-app information under
`SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall`, in four different
places depending on whether the app is 32-bit or 64-bit and whether it was
installed per-machine or per-user. Getting a complete and deduplicated list
means reading all four views, filtering out system components and update
entries, and then reconciling the result against MSI's own product
enumeration. Most tools get this wrong in some way. Doing it correctly is a
few hundred lines of tedious work, and documenting it (here and in
`architecture.md`) means it can be verified and improved.

### Drive health with the correct access mode per drive

ATA SMART requires `IOCTL_ATA_PASS_THROUGH`, which requires a read+write
handle on the physical drive. NVMe health requires only
`IOCTL_STORAGE_QUERY_PROPERTY`, which works on a handle with no access
rights. Many tools just require elevation up front and are done with it.
Kernova opens each drive with the least access it needs, tries the health
query that matches the bus type, and falls back to the older
`IOCTL_STORAGE_PREDICT_FAILURE` only when the modern one isn't available.
The result is that NVMe health works unelevated, which is a small but
noticeable quality-of-life improvement.

## What's borrowed

To be clear about what isn't original:

- Theming the popup menu via `uxtheme` ordinal 135 is a well-known
  undocumented hack that many Windows apps use.
- `SHDefExtractIcon` and `SHGetFileInfo` for icon extraction are the standard
  approaches.
- The speed test uses Cloudflare's public endpoints; the protocol is theirs.
- The PDH counter paths are documented by Microsoft, and the pattern of
  using `% Processor Performance` Ã— nominal clock to get the live clock is
  what Task Manager itself does.
- The `SYSTEM_PROCESS_INFORMATION` struct layout is documented in the
  Windows Internals books and in many open-source tools.
- The squarified treemap algorithm is from Bruls, Huizing & van Wijk (2000).
- The xorshift64 random-number generator is from Marsaglia (2003).

## Known limitations

An honest list is more valuable than a feature list.

- **Per-process disk I/O is not split into read and write.** The
  `SYSTEM_PROCESS_INFORMATION` struct reports `ReadTransferCount` and
  `WriteTransferCount` separately, but in practice the split is unreliable
  for processes that use memory-mapped files or that do I/O through the
  cache. Only the total is shown.
- **Per-core temperature is not shown.** It requires a kernel driver on most
  systems. The app doesn't have one.
- **USB-attached SATA drives only expose SMART when the bridge passes ATA
  passthrough.** Many don't. The health page shows "This drive doesn't
  expose health data through Windows" for those drives.
- **The Drive Health page can take tens of seconds on SATA drives.** Each
  SMART command has a five-second timeout, and a drive that doesn't respond
  will use all of it. The query runs on a worker thread and the page is empty
  until it finishes, but the user sees a spinner for a while.
- **The trace format is versioned but not forward-compatible.** A v2 file
  won't load in a v1 build. This is deliberate: strict equality on the
  version word is safer than trying to skip unknown record types.
- **The Connections page rebuilds its row set every tick.** On a machine
  with thousands of connections (a busy server, a machine running a torrent
  client), this can be slow. A diffing implementation would be faster but
  more complex; the current one is fine for typical desktop use.
- **Installed Apps lists MSIX packages for the current user only.**
  `PackageManager.FindPackagesForUser(L"")` is current-user only. Listing
  every user's packages requires elevation and a different API call.
- **The benchmark history is limited to 100 entries.** Older results are
  discarded.
- **The Flight Recorder buffer holds about an hour at 1 Hz.** At faster
  update intervals, it holds proportionally less.

## Design decisions worth explaining

A few choices a reader might question, with the reasoning.

### Why a custom widget class instead of using the standard controls?

The app needs layered repainting (for smooth animation) and consistent
theming (for the dark mode and the accent colour). The standard controls
support neither without constant fights â€” every themed control needs a
subclass to handle painting, and every animated control needs manual
double-buffering. Building a small widget framework was less code than
fighting the standard controls on every page.

The tradeoff is that every widget must handle its own DPI scaling and theme
invalidation. This is documented in `architecture.md`, and it's the reason
`VisualGeneration()` exists.

### Why GDI+ and not Direct2D?

GDI+ is part of the base OS. Direct2D is faster, especially for gradients,
but it would add a dependency and a larger code surface for no visible
benefit at this scale. The gradients in the app are simple linear gradients
on rounded rectangles; GDI+ handles them at frame rate on any machine from
the last decade.

The mixed GDI/GDI+ usage is because GDI+ text rendering doesn't use
ClearType. For small text at typical UI sizes, ClearType is visibly better.
The pattern is: GDI+ for shapes, GDI for text, with a `Graphics` object
scoped so it's destroyed before the text is drawn.

### Why PDH instead of ETW?

PDH is synchronous, easy to use, and available on every Windows version the
app supports. ETW is more accurate and lower-overhead but requires a session,
a consumer thread, and a much larger code surface. For the metrics the app
needs (per-core clocks, disk activity, GPU usage), PDH is sufficient.

The counter paths are in English, which is a minor limitation: the app calls
`PdhAddEnglishCounterW` rather than `PdhAddCounterW`, so it works on
non-English Windows installations. `PdhAddEnglishCounterW` requires Vista or
later.

### Why `NtQuerySystemInformation` instead of the documented APIs?

`NtQuerySystemInformation(SystemProcessInformation)` returns, in a single
call, everything the Processes page needs: PID, parent PID, name, CPU time,
private working set, working set, commit, thread count, handle count,
session ID, base priority, and I/O counters. Getting the same data from
documented APIs would take several calls per process, would race with process
exit, and would fail for protected processes.

The cost is that the struct layout is not in the public SDK and is
x64-specific. This is documented in `architecture.md`, and the current layout
matches Windows 10 1809 through Windows 11 24H2.

### Why `std::jthread` instead of `std::thread`?

`std::jthread` supports cooperative cancellation via `stop_token`, and it
joins automatically on destruction. Both are useful: the benchmarks page
needs to cancel a running test, and the flight recorder needs a clean
shutdown.

The catch is the declaration-order rule: a `std::jthread` member must be
declared last so it's destroyed before the members it might touch. This is
documented, and it's checked by inspection rather than by the compiler.

### Why the elevated helper pattern instead of running the whole app as admin?

Running the whole UI elevated means every part of the app runs with
administrator rights, including the parts that read user input, parse files,
and talk to the network. The attack surface is much larger than it needs to
be.

The helper pattern runs the small, well-defined privileged operations in a
separate process that accepts a strict command line, does one thing, and
exits. The UI itself runs unelevated. The user sees one UAC prompt per
privileged action.

The command-line parsers are the security boundary. Each one accepts only
the specific shapes it needs, and rejects everything else. This is documented
in `architecture.md` and in `security.md`.

## What the app is not

- **Not a Task Manager replacement.** It has features Task Manager doesn't,
  and it lacks features Task Manager has. It's a different tool.
- **Not a system cleaner, optimizer, or "tune-up" utility.** The Optimize
  drives panel runs `defrag.exe` with the same flags Windows' own tool uses;
  it doesn't invent its own optimisation heuristics.
- **Not a security tool.** It shows you what's running and what's connected;
  it doesn't judge whether that's suspicious. Use Process Explorer, Autoruns,
  and your own judgement for that.
- **Not a benchmark for comparing machines.** The benchmark numbers are
  meaningful within a single machine over time (did my SSD get slower, is my
  CPU throttling more than usual), not between machines. The CPU benchmark
  measures FP64 throughput, which is a specific workload that most consumer
  CPUs don't fully expose.
- **Not finished.** Every tool like this is a work in progress. See the
  "Known limitations" section above.