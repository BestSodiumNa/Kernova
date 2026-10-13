#include "table.h"

#include "theme.h"

#include <algorithm>

namespace mtm {

void Table::Create(std::vector<Column> columns, int sortColumn, bool descending) {
    cols_ = std::move(columns);
    sortCol_ = sortColumn;
    sortDesc_ = descending;
    for (const Column& c : cols_) hasHeat_ |= c.heat != 0;
    heatScale_.assign(cols_.size(), 0);

    hwnd_ = CreateChild(WC_LISTVIEWW, L"",
                        WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SINGLESEL);
    ListView_SetExtendedListViewStyle(hwnd_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    // A 1px-wide image list is the standard way to get taller, airier rows.
    HIMAGELIST rowHeight = ImageList_Create(1, Scale(30), ILC_COLOR32, 1, 0);
    ListView_SetImageList(hwnd_, rowHeight, LVSIL_SMALL);
    SendMessageW(hwnd_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);

    for (int i = 0; i < static_cast<int>(cols_.size()); ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        c.fmt = (cols_[i].numeric && i > 0) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = Scale(cols_[i].width);
        c.pszText = const_cast<LPWSTR>(cols_[i].title.c_str());
        c.iSubItem = i;
        ListView_InsertColumn(hwnd_, i, &c);
    }
    SetWindowSubclass(ListView_GetHeader(hwnd_), HeaderProc, 1, reinterpret_cast<DWORD_PTR>(this));
    ThemeControl(hwnd_);
}

int Table::SelectedIndex() const { return ListView_GetNextItem(hwnd_, -1, LVNI_SELECTED); }

const Row* Table::SelectedRow() const { return RowAt(SelectedIndex()); }

const Row* Table::RowAt(int index) const {
    return index >= 0 && index < static_cast<int>(rows_.size()) ? &rows_[static_cast<size_t>(index)] : nullptr;
}

bool Table::SelectTag(uint64_t tag) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].tag != tag) continue;
        const int idx = static_cast<int>(i);
        ListView_SetItemState(hwnd_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(hwnd_, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hwnd_, idx, FALSE);
        return true;
    }
    return false;
}

void Table::SetRows(std::vector<Row> rows) {
    const Row* sel = SelectedRow();
    const bool hadSel = sel != nullptr;
    const uint64_t selTag = hadSel ? sel->tag : 0;
    const int oldIndex = SelectedIndex();

    rows_ = std::move(rows);
    for (size_t c = 0; c < cols_.size(); ++c) {
        const double heat = cols_[c].heat;
        if (heat > 0) {
            heatScale_[c] = heat;
        } else if (heat < 0) {
            double maxKey = -heat;
            for (const Row& r : rows_)
                if (c < r.keys.size() && !std::isnan(r.keys[c])) maxKey = std::max(maxKey, r.keys[c]);
            heatScale_[c] = maxKey;
        }
    }
    SortRows();
    ListView_SetItemCountEx(hwnd_, static_cast<int>(rows_.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);

    int newSel = -1;
    if (hadSel) {
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].tag == selTag) {
                newSel = static_cast<int>(i);
                break;
            }
        }
    }
    if (newSel != oldIndex) {
        ListView_SetItemState(hwnd_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (newSel >= 0) ListView_SetItemState(hwnd_, newSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Table::SortRows() {
    if (sortCol_ < 0 || sortCol_ >= static_cast<int>(cols_.size())) return;
    const size_t col = static_cast<size_t>(sortCol_);
    const bool desc = sortDesc_;
    const bool numeric = cols_[col].numeric;
    static const std::wstring kEmpty;
    std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
        const double ka = col < a.keys.size() ? a.keys[col] : NAN;
        const double kb = col < b.keys.size() ? b.keys[col] : NAN;
        int c;
        if (numeric && !std::isnan(ka) && !std::isnan(kb)) {
            c = ka < kb ? -1 : (ka > kb ? 1 : 0);
        } else {
            const std::wstring& ta = col < a.cells.size() ? a.cells[col] : kEmpty;
            const std::wstring& tb = col < b.cells.size() ? b.cells[col] : kEmpty;
            c = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, ta.c_str(),
                                static_cast<int>(ta.size()), tb.c_str(), static_cast<int>(tb.size()), nullptr, nullptr,
                                0) -
                CSTR_EQUAL;
        }
        return desc ? c > 0 : c < 0;
    });
}

void Table::Resort() {
    const Row* selRow = SelectedRow();
    const bool hadSel = selRow != nullptr;
    const uint64_t selTag = hadSel ? selRow->tag : 0;
    SortRows();
    ListView_SetItemState(hwnd_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (hadSel) {
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].tag == selTag) {
                const int idx = static_cast<int>(i);
                ListView_SetItemState(hwnd_, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hwnd_, idx, FALSE);
                break;
            }
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateRect(ListView_GetHeader(hwnd_), nullptr, FALSE);
}

COLORREF Table::RowBackground(int index) const {
    const bool selected = (ListView_GetItemState(hwnd_, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    if (selected) return Blend(g_pal.bg, g_pal.accent, IsDark() ? 0.22 : 0.16);
    return index % 2 ? Blend(g_pal.bg, g_pal.surface, IsDark() ? 0.7 : 0.9) : g_pal.bg;
}

void Table::DrawRow(HDC dc, int index) const {
    const Row* row = RowAt(index);
    RECT full{}, client{};
    ListView_GetItemRect(hwnd_, index, &full, LVIR_BOUNDS);
    GetClientRect(hwnd_, &client);
    full.left = std::min(full.left, client.left);
    full.right = std::max(full.right, client.right);
    const COLORREF rowBg = RowBackground(index);
    FillRect(dc, &full, BrushFor(rowBg));
    if (!row) return;

    HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(hwnd_, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    const int pad = Scale(8);
    for (size_t c = 0; c < cols_.size(); ++c) {
        RECT cell{};
        ListView_GetSubItemRect(hwnd_, index, static_cast<int>(c), c == 0 ? LVIR_LABEL : LVIR_BOUNDS, &cell);
        if (c == 0) cell.left = full.left;
        if (cell.right <= cell.left) continue;
        if (heatScale_[c] > 0 && c < row->keys.size() && !std::isnan(row->keys[c])) {
            const double t = std::clamp(row->keys[c] / heatScale_[c], 0.0, 1.0);
            if (t > 0.005)
                FillRect(dc, &cell, BrushFor(Blend(rowBg, g_pal.accent, (IsDark() ? 0.06 : 0.05) + t * (IsDark() ? 0.26 : 0.24))));
        }
        COLORREF fg = c == 0 || cols_[c].numeric ? g_pal.text : g_pal.textDim;
        if (c == 0 && row->accent != CLR_NONE) fg = row->accent;
        SetTextColor(dc, fg);
        RECT text{cell.left + pad, cell.top, cell.right - pad, cell.bottom};
        if (c == 0 && row->icon) {
            const int size = GetSystemMetricsForDpi(SM_CXSMICON, g_dpi);
            DrawIconEx(dc, text.left, (cell.top + cell.bottom - size) / 2, row->icon, size, size, 0, nullptr, DI_NORMAL);
            text.left += size + Scale(8);
        }
        const bool right = cols_[c].numeric && c > 0;
        const std::wstring& s = c < row->cells.size() ? row->cells[c] : std::wstring();
        DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &text,
                  (right ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
}

void Table::FillEmptyArea(HDC dc) const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    RECT header{};
    GetWindowRect(ListView_GetHeader(hwnd_), &header);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&header), 2);
    int y = std::max(client.top, header.bottom);
    const int count = static_cast<int>(rows_.size());
    if (count > 0) {
        const int last = std::min(count - 1, ListView_GetTopIndex(hwnd_) + ListView_GetCountPerPage(hwnd_));
        RECT r{};
        if (ListView_GetItemRect(hwnd_, last, &r, LVIR_BOUNDS)) y = std::max(y, static_cast<int>(r.bottom));
    }
    if (y < client.bottom) {
        RECT empty{client.left, y, client.right, client.bottom};
        FillRect(dc, &empty, BrushFor(g_pal.bg));
    }
}

// The header is painted entirely here: small-caps titles, a thin rule and sort chevrons.
LRESULT CALLBACK Table::HeaderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Table*>(ref);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        self->PaintHeader(h, mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case HDM_LAYOUT: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        auto* layout = reinterpret_cast<HDLAYOUT*>(lp);
        const int extra = Scale(10);
        layout->pwpos->cy += extra;
        layout->prc->top += extra;
        return r;
    }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void Table::PaintHeader(HWND header, HDC dc) const {
    RECT rc;
    GetClientRect(header, &rc);
    FillRect(dc, &rc, BrushFor(g_pal.bg));
    SetBkMode(dc, TRANSPARENT);
    const int pad = Scale(8);
    const int count = Header_GetItemCount(header);
    for (int i = 0; i < count; ++i) {
        RECT item;
        Header_GetItemRect(header, i, &item);
        const bool sorted = i == sortCol_;
        const bool right = cols_[static_cast<size_t>(i)].numeric && i > 0;
        RECT text = item;
        text.left += pad;
        text.right -= pad;
        // Reserve room for the sort chevron so it never overlaps the title.
        const int chevronW = Scale(14);
        if (sorted) {
            if (right) text.left += chevronW;
            else text.right -= chevronW;
        }
        HGDIOBJ old = SelectObject(dc, CapsFont());
        SetTextColor(dc, sorted ? g_pal.text : g_pal.textFaint);
        const std::wstring upper = ToUpper(cols_[static_cast<size_t>(i)].title);
        RECT measured = text;
        DrawTextW(dc, upper.c_str(), -1, &measured, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        const int textW = std::min<int>(measured.right - measured.left, text.right - text.left);
        DrawTextW(dc, upper.c_str(), -1, &text,
                  (right ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (sorted) {
            LOGFONTW lf{};
            GetObjectW(IconFont(), sizeof lf, &lf);
            lf.lfHeight = lf.lfHeight * 6 / 10;
            HFONT small = CreateFontIndirectW(&lf);
            SelectObject(dc, small);
            const wchar_t chevron = sortDesc_ ? 0xE70D : 0xE70E;  // ChevronDown / ChevronUp
            RECT arrow = text;
            if (right) {
                arrow.right = text.right - textW - Scale(4);
                arrow.left = arrow.right - chevronW;
            } else {
                arrow.left = text.left + textW + Scale(4);
                arrow.right = arrow.left + chevronW;
            }
            SetTextColor(dc, g_pal.accent);
            DrawTextW(dc, &chevron, 1, &arrow, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, CapsFont());
            DeleteObject(small);
        }
        SelectObject(dc, old);
    }
    RECT rule{rc.left, rc.bottom - 1, rc.right, rc.bottom};
    FillRect(dc, &rule, BrushFor(g_pal.border));
}

bool Table::HandleNotify(NMHDR* nm, LRESULT& result) {
    if (nm->hwndFrom != hwnd_) return false;
    switch (nm->code) {
    case LVN_GETDISPINFOW: {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        if ((di->item.mask & LVIF_TEXT) && di->item.pszText && di->item.cchTextMax > 0) {
            const Row* row = RowAt(di->item.iItem);
            const size_t sub = static_cast<size_t>(di->item.iSubItem);
            const wchar_t* text = row && sub < row->cells.size() ? row->cells[sub].c_str() : L"";
            wcsncpy_s(di->item.pszText, static_cast<size_t>(di->item.cchTextMax), text, _TRUNCATE);
        }
        result = 0;
        return true;
    }
    case NM_CUSTOMDRAW: {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
        // Rows are painted entirely here (edge to edge), and the empty area below them is
        // filled in post-paint. Both cover the column separator lines the list theme draws.
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            result = CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            return true;
        case CDDS_ITEMPREPAINT:
            DrawRow(cd->nmcd.hdc, static_cast<int>(cd->nmcd.dwItemSpec));
            result = CDRF_SKIPDEFAULT;
            return true;
        case CDDS_POSTPAINT:
            FillEmptyArea(cd->nmcd.hdc);
            result = CDRF_DODEFAULT;
            return true;
        default:
            result = CDRF_DODEFAULT;
            return true;
        }
    }
    case LVN_COLUMNCLICK: {
        const int col = reinterpret_cast<NMLISTVIEW*>(nm)->iSubItem;
        if (col == sortCol_) {
            sortDesc_ = !sortDesc_;
        } else {
            sortCol_ = col;
            sortDesc_ = cols_[static_cast<size_t>(col)].numeric;  // numbers: biggest first
        }
        Resort();
        result = 0;
        return true;
    }
    case LVN_ODFINDITEMW: {
        auto* f = reinterpret_cast<NMLVFINDITEMW*>(nm);
        result = -1;
        if ((f->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) && f->lvfi.psz && !rows_.empty()) {
            const size_t len = wcslen(f->lvfi.psz);
            const size_t n = rows_.size();
            const size_t start = f->iStart >= 0 ? static_cast<size_t>(f->iStart) : 0;
            for (size_t k = 0; k < n; ++k) {
                const size_t i = (start + k) % n;
                const std::wstring& t = rows_[i].cells.empty() ? std::wstring() : rows_[i].cells[0];
                if (t.size() >= len && CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, t.c_str(),
                                                       static_cast<int>(len), f->lvfi.psz, static_cast<int>(len),
                                                       nullptr, nullptr, 0) == CSTR_EQUAL) {
                    result = static_cast<LRESULT>(i);
                    break;
                }
            }
        }
        return true;
    }
    case LVN_ITEMCHANGED:
        InvalidateRect(hwnd_, nullptr, FALSE);  // repaint the custom selection colour
        return false;                           // pages may still want this
    default:
        return false;
    }
}

}  // namespace mtm

namespace mtm {

int Table::HeightForRows(size_t rows) const {
    // The list knows its own row and header heights (both set at creation, DPI-scaled).
    const DWORD size = ListView_ApproximateViewRect(hwnd_, -1, -1, static_cast<int>(rows));
    return HIWORD(size) + Scale(6);
}

}  // namespace mtm
