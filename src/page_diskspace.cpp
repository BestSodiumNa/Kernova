#include "graph.h"
#include "page.h"
#include "table.h"
#include "theme.h"

#include <shobjidl.h>

#include <algorithm>
#include <thread>
#include <unordered_map>

namespace mtm {
namespace {

constexpr size_t kLargestFiles = 1000;

struct FolderAgg {
    std::wstring name, path;
    uint64_t bytes = 0, files = 0;
    bool looseFiles = false;  // the "(files directly in this folder)" bucket
};
struct FileEntry {
    std::wstring path;
    uint64_t bytes = 0;
    FILETIME modified{};
};
struct ExtAgg {
    uint64_t bytes = 0, files = 0;
};
struct ScanResult {
    std::wstring root;
    std::vector<FolderAgg> folders;
    std::vector<FileEntry> files;  // largest first
    std::vector<std::pair<std::wstring, ExtAgg>> exts;
    uint64_t totalBytes = 0, totalFiles = 0, totalDirs = 0, unreadable = 0;
    double seconds = 0;
    bool cancelled = false;
};

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
    return !dir.empty() && dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

// Long-path form so folders deeper than MAX_PATH can still be read.
std::wstring ToApiPath(std::wstring p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}
std::wstring ToDisplayPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
    if (p.rfind(L"\\\\?\\", 0) == 0) return p.substr(4);
    return p;
}

std::wstring ExtensionOf(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot || dot == name || !dot[1]) return L"(no extension)";
    return ToLower(dot);
}

ScanResult Scan(std::wstring root, std::stop_token st, const std::function<void(uint64_t, uint64_t, const std::wstring&)>& progress) {
    ScanResult res;
    if (root.size() == 2 && root[1] == L':') root += L'\\';
    while (root.size() > 3 && root.back() == L'\\') root.pop_back();
    res.root = root;
    Stopwatch sw, report;

    std::unordered_map<std::wstring, ExtAgg> exts;
    std::vector<FileEntry> heap;  // min-heap on size, keeps the N largest
    auto bySizeDesc = [](const FileEntry& a, const FileEntry& b) { return a.bytes > b.bytes; };
    int looseIdx = -1;

    struct Item {
        std::wstring dir;
        int top;  // index into res.folders, or -1 for the root itself
    };
    std::vector<Item> stack{{ToApiPath(root), -1}};
    while (!stack.empty()) {
        if (st.stop_requested()) {
            res.cancelled = true;
            break;
        }
        Item item = std::move(stack.back());
        stack.pop_back();
        ++res.totalDirs;

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(Join(item.dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            ++res.unreadable;
            continue;
        }
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Junctions and symlinks would double-count or loop forever.
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                int top = item.top;
                const std::wstring child = Join(item.dir, n);
                if (top < 0) {
                    res.folders.push_back({n, ToDisplayPath(child)});
                    top = static_cast<int>(res.folders.size()) - 1;
                }
                stack.push_back({child, top});
                continue;
            }
            const uint64_t size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            res.totalBytes += size;
            ++res.totalFiles;
            int top = item.top;
            if (top < 0) {
                if (looseIdx < 0) {
                    res.folders.push_back({L"(files directly in this folder)", root, 0, 0, true});
                    looseIdx = static_cast<int>(res.folders.size()) - 1;
                }
                top = looseIdx;
            }
            res.folders[static_cast<size_t>(top)].bytes += size;
            ++res.folders[static_cast<size_t>(top)].files;
            ExtAgg& e = exts[ExtensionOf(n)];
            e.bytes += size;
            ++e.files;
            if (heap.size() < kLargestFiles || size > heap.front().bytes) {
                heap.push_back({ToDisplayPath(Join(item.dir, n)), size, fd.ftLastWriteTime});
                std::push_heap(heap.begin(), heap.end(), bySizeDesc);
                if (heap.size() > kLargestFiles) {
                    std::pop_heap(heap.begin(), heap.end(), bySizeDesc);
                    heap.pop_back();
                }
            }
        } while (FindNextFileW(h, &fd) && !st.stop_requested());
        FindClose(h);

        if (report.Seconds() > 0.25) {
            report.Reset();
            progress(res.totalFiles, res.totalBytes, ToDisplayPath(item.dir));
        }
    }
    std::sort(heap.begin(), heap.end(), bySizeDesc);
    res.files = std::move(heap);
    res.exts.assign(exts.begin(), exts.end());
    res.seconds = sw.Seconds();
    return res;
}

double Pct(uint64_t part, uint64_t total) { return total ? 100.0 * double(part) / double(total) : 0; }

class DiskSpacePage final : public Page {
public:
    const wchar_t* Title() const override { return L"Disk space"; }

    void Create() override {
        wchar_t sysDrive[MAX_PATH] = L"C:";
        GetEnvironmentVariableW(L"SystemDrive", sysDrive, MAX_PATH);
        path_ = CreateEdit((std::wstring(sysDrive) + L"\\").c_str(), L"Folder or drive to scan", 0xE8B7 /* folder */);
        browse_ = CreateButton(L"  Browse…");
        up_ = CreateButton(L"  Up");
        scan_ = CreateButton(L"  Scan", true);
        view_ = CreateCombo({L"Folders", L"Largest files", L"File types"});
        status_ = CreateLabel(L"Pick a folder or drive and press Scan. Double-click a folder to drill in.",
                              SS_CENTERIMAGE | SS_PATHELLIPSIS);
        folders_.Create({{L"Folder", 340}, {L"Size", 110, true}, {L"% of total", 90, true, 100}, {L"Files", 100, true}}, 1, true);
        files_.Create({{L"File", 560}, {L"Size", 110, true}, {L"Modified", 140}}, 1, true);
        types_.Create({{L"Extension", 200}, {L"Size", 110, true}, {L"% of total", 90, true, 100}, {L"Files", 100, true}}, 1, true);
        treemap_.Create([this](uint64_t tag, bool open) { OnPick(tag, open); });
    }

    void Layout(RECT a) override {
        const int h = Scale(34), gap = Scale(8);
        const int btn = Scale(110);
        const int right = a.right;
        Place(view_, right - Scale(150), a.top, Scale(150), Scale(200));
        Place(scan_, right - Scale(150) - gap - btn, a.top, btn, h);
        Place(up_, right - Scale(150) - 2 * gap - 2 * btn, a.top, btn, h);
        Place(browse_, right - Scale(150) - 3 * gap - 3 * btn, a.top, btn, h);
        Place(path_, a.left, a.top, right - Scale(150) - 4 * gap - 3 * btn - a.left, h);
        Place(status_, a.left, a.top + h + Scale(8), a.right - a.left, Scale(24));
        // Visual first: the treemap takes the top half, the detailed list sits below it.
        const int mapTop = a.top + h + Scale(44);
        const int mapH = std::max<int>(Scale(200), static_cast<int>(a.bottom - mapTop) / 2);
        Place(treemap_.Handle(), a.left, mapTop, a.right - a.left, mapH);
        const int tableTop = mapTop + mapH + Scale(12);
        for (Table* t : {&folders_, &files_, &types_})
            Place(t->Handle(), a.left, tableTop, a.right - a.left, a.bottom - tableTop);
    }

    void Show(bool v) override {
        visible_ = v;
        ShowAll({path_, browse_, scan_, up_, view_, status_, treemap_.Handle()}, v);
        UpdateViewVisibility();
    }

    bool OnCommand(HWND control, WORD code) override {
        if (code != BN_CLICKED && code != CBN_SELCHANGE) return false;
        if (control == browse_) {
            const std::wstring p = PickFolder();
            if (!p.empty()) {
                SetWindowTextW(path_, p.c_str());
                StartScan();
            }
            return true;
        }
        if (control == scan_) {
            if (scanning_) worker_.request_stop();
            else StartScan();
            return true;
        }
        if (control == up_) {
            std::wstring p = GetText(path_);
            while (p.size() > 3 && p.back() == L'\\') p.pop_back();
            const size_t slash = p.find_last_of(L'\\');
            if (slash != std::wstring::npos && p.size() > 3) {
                p = p.substr(0, slash);
                if (p.size() == 2) p += L'\\';
                SetWindowTextW(path_, p.c_str());
                StartScan();
            }
            return true;
        }
        if (control == view_) {
            UpdateViewVisibility();
            UpdateTreemap();
            return true;
        }
        return false;
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override {
        if (folders_.HandleNotify(nm, result) || files_.HandleNotify(nm, result) || types_.HandleNotify(nm, result)) return true;
        if (nm->code == NM_DBLCLK && nm->hwndFrom == folders_.Handle()) {
            if (const Row* r = folders_.SelectedRow()) {
                const FolderAgg& f = result_.folders[static_cast<size_t>(r->tag)];
                if (!f.looseFiles) {
                    SetWindowTextW(path_, f.path.c_str());
                    StartScan();
                }
            }
            return true;
        }
        // Keep the treemap highlight in step with the list selection.
        if (nm->code == LVN_ITEMCHANGED && nm->hwndFrom == CurrentTable().Handle()) {
            if (const Row* r = CurrentTable().SelectedRow()) treemap_.SetSelected(r->tag);
            return true;
        }
        return false;
    }

    bool OnContextMenu(HWND control, POINT pt) override {
        std::wstring path;
        bool isFile = false;
        if (control == folders_.Handle()) {
            if (const Row* r = folders_.SelectedRow()) path = result_.folders[static_cast<size_t>(r->tag)].path;
        } else if (control == files_.Handle()) {
            if (const Row* r = files_.SelectedRow()) {
                path = result_.files[static_cast<size_t>(r->tag)].path;
                isFile = true;
            }
        } else {
            return false;
        }
        if (path.empty()) return true;
        if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, isFile ? L"Show in Explorer" : L"Open in Explorer");
        if (!isFile) AppendMenuW(menu, MF_STRING, 2, L"Scan this folder");
        AppendMenuW(menu, MF_STRING, 3, L"Copy path");
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) ShowInExplorer(path, isFile);
        else if (cmd == 2) {
            SetWindowTextW(path_, path.c_str());
            StartScan();
        } else if (cmd == 3) CopyToClipboard(path);
        return true;
    }

private:
    int View() const { return static_cast<int>(SendMessageW(view_, CB_GETCURSEL, 0, 0)); }
    Table& CurrentTable() { return View() == 1 ? files_ : (View() == 2 ? types_ : folders_); }

    // Block clicked: select its row. Double-click: open the folder (or show the file).
    void OnPick(uint64_t tag, bool open) {
        CurrentTable().SelectTag(tag);
        if (!open || scanning_) return;
        if (View() == 0 && tag < result_.folders.size() && !result_.folders[tag].looseFiles) {
            SetWindowTextW(path_, result_.folders[tag].path.c_str());
            StartScan();
        } else if (View() == 1 && tag < result_.files.size()) {
            ShowInExplorer(result_.files[tag].path, true);
        }
    }

    void UpdateTreemap() {
        std::vector<Treemap::Item> items;
        std::wstring title;
        const std::wstring total = FormatBytes(result_.totalBytes);
        if (View() == 0) {
            for (size_t i = 0; i < result_.folders.size(); ++i)
                items.push_back({result_.folders[i].name, result_.folders[i].bytes, i});
            title = L"Folders in " + result_.root + L"  ·  " + total;
        } else if (View() == 1) {
            const size_t n = std::min<size_t>(result_.files.size(), 120);
            for (size_t i = 0; i < n; ++i) {
                const std::wstring& p = result_.files[i].path;
                const size_t slash = p.find_last_of(L"\\/");
                items.push_back({slash == std::wstring::npos ? p : p.substr(slash + 1), result_.files[i].bytes, i});
            }
            title = Format(L"Largest %zu files", n);
        } else {
            for (size_t i = 0; i < result_.exts.size(); ++i)
                items.push_back({result_.exts[i].first, result_.exts[i].second.bytes, i});
            title = L"Space by file type  ·  " + total;
        }
        if (result_.root.empty()) title = L"Disk usage";
        treemap_.SetItems(std::move(items), std::move(title));
        if (const Row* r = CurrentTable().SelectedRow()) treemap_.SetSelected(r->tag);
        else treemap_.SetSelected(Treemap::kOtherTag);
    }

    void UpdateViewVisibility() {
        const int v = static_cast<int>(SendMessageW(view_, CB_GETCURSEL, 0, 0));
        ShowWindow(folders_.Handle(), visible_ && v == 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(files_.Handle(), visible_ && v == 1 ? SW_SHOW : SW_HIDE);
        ShowWindow(types_.Handle(), visible_ && v == 2 ? SW_SHOW : SW_HIDE);
    }

    void StartScan() {
        if (scanning_) return;
        const std::wstring root = GetText(path_);
        if (GetFileAttributesW(ToApiPath(root).c_str()) == INVALID_FILE_ATTRIBUTES) {
            ShowError(L"That folder doesn't exist or can't be opened: " + root);
            return;
        }
        scanning_ = true;
        SetWindowTextW(scan_, L"  Stop");
        EnableWindow(browse_, FALSE);
        EnableWindow(up_, FALSE);
        SetText(status_, L"Scanning " + root + L"…");
        worker_ = std::jthread([this, root](std::stop_token st) {
            ScanResult res = Scan(root, st, [this](uint64_t files, uint64_t bytes, const std::wstring& dir) {
                PostToUi([this, files, bytes, dir] {
                    SetText(status_, Format(L"Scanning… %s files · %s · %s", FormatCount(files).c_str(),
                                            FormatBytes(bytes).c_str(), dir.c_str()));
                });
            });
            PostToUi([this, res = std::move(res)]() mutable { Finished(std::move(res)); });
        });
    }

    void Finished(ScanResult res) {
        scanning_ = false;
        SetWindowTextW(scan_, L"  Scan");
        EnableWindow(browse_, TRUE);
        EnableWindow(up_, TRUE);
        result_ = std::move(res);
        const uint64_t total = result_.totalBytes;

        std::vector<Row> rows;
        for (size_t i = 0; i < result_.folders.size(); ++i) {
            const auto& f = result_.folders[i];
            Row r;
            r.tag = i;
            r.Add(f.name).Add(FormatBytes(f.bytes), double(f.bytes)).Add(Format(L"%.1f%%", Pct(f.bytes, total)), Pct(f.bytes, total))
                .Add(FormatCount(f.files), double(f.files));
            rows.push_back(std::move(r));
        }
        folders_.SetRows(std::move(rows));

        rows.clear();
        for (size_t i = 0; i < result_.files.size(); ++i) {
            const auto& f = result_.files[i];
            Row r;
            r.tag = i;
            r.Add(f.path).Add(FormatBytes(f.bytes), double(f.bytes)).Add(FormatFileTime(f.modified));
            rows.push_back(std::move(r));
        }
        files_.SetRows(std::move(rows));

        rows.clear();
        for (size_t i = 0; i < result_.exts.size(); ++i) {
            const auto& [ext, e] = result_.exts[i];
            Row r;
            r.tag = i;
            r.Add(ext).Add(FormatBytes(e.bytes), double(e.bytes)).Add(Format(L"%.1f%%", Pct(e.bytes, total)), Pct(e.bytes, total))
                .Add(FormatCount(e.files), double(e.files));
            rows.push_back(std::move(r));
        }
        types_.SetRows(std::move(rows));
        UpdateTreemap();

        SetText(status_, Format(L"%s%s — %s in %s files and %s folders · %.1f s%s", result_.cancelled ? L"Stopped: " : L"",
                                result_.root.c_str(), FormatBytes(total).c_str(), FormatCount(result_.totalFiles).c_str(),
                                FormatCount(result_.totalDirs).c_str(), result_.seconds,
                                result_.unreadable ? Format(L" · %s folders couldn't be read (try as administrator)",
                                                            FormatCount(result_.unreadable).c_str()).c_str()
                                                   : L""));
    }

    HWND path_ = nullptr, browse_ = nullptr, scan_ = nullptr, up_ = nullptr, view_ = nullptr, status_ = nullptr;
    Table folders_, files_, types_;
    Treemap treemap_;
    ScanResult result_;
    bool scanning_ = false;
    bool visible_ = false;
    std::jthread worker_;  // last member: joined before the rest is destroyed
};

}  // namespace

std::unique_ptr<Page> MakeDiskSpacePage() { return std::make_unique<DiskSpacePage>(); }

}  // namespace mtm
