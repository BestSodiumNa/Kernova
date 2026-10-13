// Kept in its own file: C++/WinRT headers are large and slow to compile.
#include "apps.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>

namespace mtm {

void CollectStoreApps(std::vector<InstalledApp>& out) {
    using namespace winrt::Windows::ApplicationModel;
    bool initialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        initialized = true;
    } catch (...) {
    }
    try {
        winrt::Windows::Management::Deployment::PackageManager manager;
        for (const Package& pkg : manager.FindPackagesForUser(L"")) {
            try {
                if (pkg.IsFramework() || pkg.IsResourcePackage()) continue;
                if (pkg.SignatureKind() == PackageSignatureKind::System) continue;
                InstalledApp app;
                app.isStoreApp = true;
                app.source = L"Microsoft Store / MSIX";
                app.name = pkg.DisplayName().c_str();
                if (app.name.empty() || app.name.rfind(L"ms-resource:", 0) == 0) app.name = pkg.Id().Name().c_str();
                app.publisher = pkg.PublisherDisplayName().c_str();
                const auto v = pkg.Id().Version();
                app.version = Format(L"%u.%u.%u.%u", v.Major, v.Minor, v.Build, v.Revision);
                const auto ft = winrt::clock::to_file_time(pkg.InstalledDate());
                FILETIME f{static_cast<DWORD>(ft.value), static_cast<DWORD>(ft.value >> 32)};
                app.installDate = FormatFileTime(f, false);
                try {
                    app.location = pkg.InstalledLocation().Path().c_str();
                    app.iconLocation = pkg.Logo().AbsoluteUri().c_str();
                } catch (...) {
                }
                out.push_back(std::move(app));
            } catch (...) {
                // A single broken package registration shouldn't hide the rest.
            }
        }
    } catch (...) {
    }
    if (initialized) winrt::uninit_apartment();
}

}  // namespace mtm
