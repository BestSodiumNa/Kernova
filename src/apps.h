#pragma once

#include "common.h"

#include <set>

namespace mtm {

struct InstalledApp {
    std::wstring name, publisher, version, installDate, location, source, uninstall;
    std::wstring iconLocation;  // DisplayIcon-style "path,index", or a logo image path
    HICON icon = nullptr;       // filled in by the page (shared cache, not owned)
    uint64_t sizeBytes = 0;
    bool isStoreApp = false;
};

// Uninstall registry keys in all four views (machine/user x 64/32-bit).
// keyNames receives every subkey name seen (lower-case), including hidden ones,
// so MSI products already represented there are not listed twice.
void CollectRegistryApps(std::vector<InstalledApp>& out, std::set<std::wstring>& keyNames);

// Windows Installer products not already listed in the registry.
void CollectMsiApps(std::vector<InstalledApp>& out, const std::set<std::wstring>& skipProductCodes);

// MSIX / Microsoft Store packages for the current user (excludes frameworks and system packages).
void CollectStoreApps(std::vector<InstalledApp>& out);

}  // namespace mtm
