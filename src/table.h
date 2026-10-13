#pragma once

#include "common.h"

namespace mtm {

struct Column {
    std::wstring title;
    int width = 100;       // at 96 DPI
    bool numeric = false;  // sorts by Row::keys and right-aligns
    // Heat shading like Windows Task Manager: 0 = off, > 0 = fixed full-scale value,
    // < 0 = relative to the column's largest value (with |heat| as the minimum scale).
    double heat = 0;
};

struct Row {
    std::vector<std::wstring> cells;
    std::vector<double> keys;  // numeric sort key per cell (NAN = sort by text)
    uint64_t tag = 0;          // stable identity, used to keep the selection across refreshes
    COLORREF accent = CLR_NONE;  // optional colour for the first cell's text (status rows)
    HICON icon = nullptr;        // optional small icon drawn before the first cell (not owned)

    Row& Add(std::wstring text, double key = NAN) {
        cells.push_back(std::move(text));
        keys.push_back(key);
        return *this;
    }
};

// A virtual (owner-data) report ListView with click-to-sort columns, drawn in the app theme.
class Table {
public:
    void Create(std::vector<Column> columns, int sortColumn = -1, bool descending = false);
    HWND Handle() const { return hwnd_; }

    void SetRows(std::vector<Row> rows);
    size_t Size() const { return rows_.size(); }
    // Height that shows `rows` rows plus the header without a vertical scrollbar.
    int HeightForRows(size_t rows) const;
    int SelectedIndex() const;
    const Row* SelectedRow() const;
    const Row* RowAt(int index) const;
    // Selects and scrolls to the row with this tag; returns false if it isn't present.
    bool SelectTag(uint64_t tag);

    // Handles the list's own notifications (display text, custom draw, sorting, type-to-find).
    // Returns false for anything the page should handle itself (double-click etc.).
    bool HandleNotify(NMHDR* nm, LRESULT& result);

private:
    static LRESULT CALLBACK HeaderProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void PaintHeader(HWND header, HDC dc) const;
    void DrawRow(HDC dc, int index) const;
    void FillEmptyArea(HDC dc) const;
    void SortRows();
    void Resort();
    COLORREF RowBackground(int index) const;

    HWND hwnd_ = nullptr;
    std::vector<Column> cols_;
    std::vector<Row> rows_;
    std::vector<double> heatScale_;
    bool hasHeat_ = false;
    int sortCol_ = -1;
    bool sortDesc_ = false;
};

}  // namespace mtm
