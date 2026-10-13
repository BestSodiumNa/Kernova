#pragma once

#include "common.h"

namespace mtm {

// Small icon for a process's executable. Returns the generic application icon until
// the real one has been looked up; lookups happen in ResolveProcessIcons().
HICON ProcessIcon(DWORD pid);

// Called once per tick (after the process sampler): drops entries for exited processes
// and resolves a bounded number of pending icons so the UI never stalls on a big list.
void ResolveProcessIcons();

}  // namespace mtm

namespace mtm {

// Small icon described by a registry-style location: "C:\path\app.exe,3", "app.ico",
// "%SystemRoot%\x.dll,-101" (resource id), a .png/.lnk/any file, or a file:/// URI.
// Thread-safe and cached; the icons live for the rest of the app's lifetime.
// Returns the generic application icon when nothing usable is found.
HICON IconFromLocation(const std::wstring& location);

// Shell icon for a file (exe, shortcut, document), as Explorer shows it. Thread-safe, cached.
HICON FileIcon(const std::wstring& path);

// The generic application icon (not owned).
HICON GenericAppIcon();

}  // namespace mtm
