# File and registry formats

This document describes every persistent thing the app writes: the trace
file, the benchmark history, and the registry schema.

## `.mtmtrace` — Flight Recorder trace

Little-endian. Strings are UTF-16LE with a `uint16_t` length prefix (the
length is in characters, not bytes).

### File header (16 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 8 | Magic | ASCII `MTMTRACE` |
| 8 | 4 | Version | `1` |
| 12 | 4 | Reserved | `0` |

The reserved word is there so a future version can add flags without bumping
the version number.

### Records

Immediately after the header, the file is a sequence of records:

```text
[u32 length][u32 crc32c][payload of length bytes]
```

- `length` is the size of the payload in bytes, not including the length and
  CRC fields.
- `crc32c` is the CRC32C (Castagnoli, polynomial `0x82F63B78`, reflected) of
  the payload, computed with an initial value of 0 and a final XOR of
  `0xFFFFFFFF`. The table used is the standard one; it is generated at
  compile time in `recorder.cpp`.

A record with `length == 0` or with `pos + length > fileSize` is treated as a
truncated final record: the reader stops and returns what it has. A record
whose CRC doesn't match, or whose payload fails to decode, is a corruption:
the reader fails with an error that includes the byte offset.

### Payload layout

Fields are written in the order below, all little-endian and tightly packed
(no alignment padding). All `float` values are IEEE 754 binary32.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `time` — FILETIME (100 ns ticks since 1601-01-01 UTC) |
| 8 | 8 | `monotonic` — `GetTickCount64()` at capture |
| 16 | 4 | `cpu` — total CPU percentage, `float` |
| 20 | 4 | `gpu` — total GPU percentage, `float` |
| 24 | 4 | `diskActive` — busiest disk active percentage, `float` |
| 28 | 8 | `memUsed` — bytes |
| 36 | 8 | `memTotal` — bytes |
| 44 | 8 | `diskRead` — bytes/s |
| 52 | 8 | `diskWrite` — bytes/s |
| 60 | 8 | `netIn` — bytes/s |
| 68 | 8 | `netOut` — bytes/s |
| 76 | 8 | `gpuMemory` — dedicated GPU memory in use, bytes |

Then the per-core arrays:

| Offset | Size | Field |
|---|---|---|
| 84 | 2 | `cores` — count, `uint16` |
| 86 | 4 × N | `coreUsage[i]` — per-core percentage, `float`, N = `cores` |
| 86 + 4N | 2 × N | `coreMhz[i]` — per-core clock, `uint16`, N = `cores` |

Then the top-process array:

| Offset | Size | Field |
|---|---|---|
| 86 + 6N | 1 | `procs` — count, `uint8` (max 32) |
| 87 + 6N | 4 | `top[i].pid` — `uint32` |
| ... | 4 | `top[i].cpu` — percentage, `float` |
| ... | 8 | `top[i].memory` — private working set, bytes |
| ... | 2 | `top[i].nameLen` — `uint16`, characters |
| ... | 2 × L | `top[i].name` — UTF-16LE |

The reader enforces the caps `cores <= 0xFFFF` and `procs <= 32`; a record
that exceeds them is treated as corrupt.

### Tolerances

- A truncated final record is accepted: the reader stops at the last
  complete record.
- A corrupt middle record aborts the load. This is deliberate: continuing
  past a corrupt record would read the next record's length from the wrong
  offset and produce a cascade of garbage.
- An empty file, a file with the wrong magic, or a file with an unsupported
  version is rejected with a specific error message.

### Forward compatibility

Version 2 will not be readable by a version 1 build. The version check is
strict equality, not "greater than or equal to," because a v2 file may
contain record types or fields a v1 reader wouldn't understand.

## `benchmarks.dat` — benchmark history

Best-effort. The file is rewritten in full on every change; a crash during
the write can leave it truncated, in which case the loader returns an empty
history rather than crashing. This is intentional — losing benchmark history
is not worth crashing over.

Little-endian. Strings are UTF-16LE with a `uint32_t` length prefix (the
length is in characters).

### Header

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | Magic | `0x4842544D` (`MTBH` in ASCII, little-endian) |
| 4 | 4 | Version | `1` |
| 8 | 4 | Count | number of records, capped at 100 |

### Per-record layout

Each record is a sequence of fields in the order below. Strings are written
with `uint32_t` length + `length * sizeof(wchar_t)` bytes. PODs are written
with their raw in-memory representation.

| Order | Type | Field |
|---|---|---|
| 1 | string | `test` — "CPU", "GPU", or "Disk C:\" |
| 2 | string | `device` |
| 3 | string | `score` — long human-readable form |
| 4 | string | `details` |
| 5 | string | `headline` — short form for the card |
| 6 | `double` | `value` — the headline number, comparable across runs |
| 7 | `uint64` | `when` — FILETIME, UTC |
| 8 | `uint8` | `ok` — 1 if the run completed, 0 otherwise |
| 9 | `uint32` | `metrics` count (max 256) |
| 10 | 2 × count strings | metric key and value, in order |
| 11 | `uint32` | `timeline` count (max 100000) |
| 12 | count × `double` | throughput samples |
| 13 | string | `timelineUnit` |
| 14 | string | `timelineNote` |

The reader caps record counts at 100, metric counts at 256, and timeline
counts at 100000 to bound memory on a corrupted file.

## `HKCU\Software\Kernova` — settings

All values are under this key. Values are `REG_DWORD` unless noted.

| Value | Type | Meaning |
|---|---|---|
| `Theme` | DWORD | `0` = match Windows, `1` = dark, `2` = light |
| `Accent` | DWORD | Index into the accent list (`0` = cyan, `1` = violet, `2` = lime, `3` = amber, `4` = rose) |
| `IntervalMs` | DWORD | Sampling interval in milliseconds (`500`, `1000`, or `2000`) |
| `AnimFps` | DWORD | Graph animation framerate (`0` = off, `30`, or `60`) |
| `TopMost` | DWORD | `1` if the window is always on top |
| `Placement` | BINARY | A `WINDOWPLACEMENT` struct, written on exit and read on startup |
| `LastTabId` | DWORD | FNV-1a hash of the last open page's title |
| `BenchGpuLuidLow` | DWORD | Low part of the LUID of the GPU selected on the Benchmarks page |
| `OptimizeReminderDays` | DWORD | Reminder interval for drive optimization (`0` = off, `14`, `30`, or `90`) |
| `OptimizeSnoozeUntil` | BINARY | A FILETIME; the reminder is suppressed until this time |

### Migration from earlier names

If `HKCU\Software\Kernova` does not exist and either
`HKCU\Software\BetterTaskManager` or `HKCU\Software\MyTaskManager` does, the
newer of the two is copied to the new key on first settings access. This runs
once per process and can be removed if those names are never used again.

## `%LOCALAPPDATA%\Kernova\benchmarks.dat`

See the section above. The directory is created on first write.
