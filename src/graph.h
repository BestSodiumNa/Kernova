#pragma once

#include "common.h"

namespace mtm {

// Base for custom-painted child windows (double-buffered, themed).
class Widget {
public:
    virtual ~Widget();
    static void Register();
    // Called by the main window's animation timer (~60 fps): repaints every visible
    // widget that is currently animating.
    static void AnimateFrame();
    HWND Handle() const { return hwnd_; }
    void Invalidate() const {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

protected:
    void CreateWidget();
    // Simple widgets paint everything here.
    virtual void Paint(HDC /*dc*/, const RECT& /*rc*/) {}
    // Layered widgets (Layered() == true) split painting: PaintStatic is rendered into a
    // cache and only redone after InvalidateStatic(), a resize or a theme/DPI change;
    // PaintDynamic draws the moving parts on top every frame. This keeps 60 fps
    // animation cheap: text, cards and grids aren't re-rendered per frame.
    virtual bool Layered() const { return false; }
    virtual void PaintStatic(HDC /*dc*/, const RECT& /*rc*/) {}
    virtual void PaintDynamic(HDC /*dc*/, const RECT& /*rc*/) {}
    void InvalidateStatic() {
        staticValid_ = false;
        Invalidate();
    }
    // Mouse moves, button presses, double-clicks and WM_MOUSELEAVE (x = y = -1).
    virtual void OnMouse(UINT /*msg*/, int /*x*/, int /*y*/) {}
    // Mouse wheel over the widget (delta in WHEEL_DELTA units). Return true if handled.
    virtual bool OnWheel(int /*delta*/) { return false; }
    // True while the widget wants animation frames (see AnimateFrame).
    virtual bool Animating() const { return false; }
    HWND hwnd_ = nullptr;

private:
    // An offscreen bitmap kept between paints instead of being re-created each frame.
    struct Surface {
        HDC dc = nullptr;
        HBITMAP bmp = nullptr;
        HGDIOBJ old = nullptr;
        SIZE size{};
        bool Ensure(HDC ref, int w, int h);  // true if (re)allocated
        void Free();
    };
    void OnPaint();
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    Surface back_, static_;
    bool staticValid_ = false;
    unsigned staticGeneration_ = 0;
};

// Rounded card with border on the page background; returns the inner content rect.
RECT PaintCard(HDC dc, const RECT& rc);

// A metric card: caps title, big value, a smooth glowing history graph and detail lines.
class Graph : public Widget {
public:
    enum class Tint { Cpu, Gpu, Memory, Disk, Network };
    void Create(Tint tint, double fixedMax /* 0 = autoscale */);
    void Push(double value);
    void SetText(std::wstring title, std::wstring value, std::wstring details);

    // Several lines on one graph (one per GPU / disk), each in its own colour with a
    // legend. Calling it with a different set of labels resets the history.
    struct Series {
        std::wstring label;
        Tint tint;
    };
    void SetSeries(std::vector<Series> series);
    // One value (and legend read-out) per series, in SetSeries order.
    void PushMany(const std::vector<double>& values, std::vector<std::wstring> legendValues);

    // Secondary read-outs such as temperatures, drawn smaller and dimmer: beside the big value for a
    // single series, in each legend row (before the percentage) for several. Empty strings hide one.
    void SetTemps(std::vector<std::wstring> temps);

private:
    bool Layered() const override { return true; }
    void PaintStatic(HDC dc, const RECT& rc) override;   // card, text, grid
    void PaintDynamic(HDC dc, const RECT& rc) override;  // the scrolling series
    bool Animating() const override {
        return SmoothAnimation() && (data_.size() >= 2 || (!multi_.empty() && multi_[0].size() >= 2));
    }
    std::vector<Series> series_;
    std::vector<std::vector<double>> multi_;
    std::vector<std::wstring> legendValues_, temps_;
    static constexpr size_t kCapacity = 60;  // visible samples (one extra is kept off the left edge)
    RECT plot_{};
    Tint tint_ = Tint::Cpu;
    double fixedMax_ = 100;
    std::vector<double> data_;
    std::wstring title_, value_, details_;
    Stopwatch sincePush_;  // drives the scroll between samples
};

// Per-logical-processor usage tiles.
class CoreGrid : public Widget {
public:
    struct Core {
        double usage = 0;  // 0..100
        ULONG mhz = 0;
    };
    void Create() { CreateWidget(); }
    void SetCores(std::vector<Core> cores, std::wstring subtitle);

private:
    bool Layered() const override { return true; }
    void PaintStatic(HDC dc, const RECT& rc) override;   // card, labels, empty tracks
    void PaintDynamic(HDC dc, const RECT& rc) override;  // eased bar fills
    bool Animating() const override;
    std::vector<RECT> bars_;  // track rectangles, laid out in PaintStatic
    std::vector<Core> cores_;
    std::vector<double> shown_;     // eased bar heights / usage read-outs
    std::vector<double> shownMhz_;  // eased clock read-outs
    std::vector<double> vel_, velMhz_;  // spring velocities (units per second)
    std::vector<RECT> labels_;      // read-out rectangles under each bar
    std::wstring subtitle_;
    Stopwatch frameClock_;
};

// Slim rounded progress bar.
class Bar : public Widget {
public:
    void Create() { CreateWidget(); }
    void SetFraction(double f);

private:
    void Paint(HDC dc, const RECT& rc) override;
    double fraction_ = 0;
};

// Compact dashboard tile: caps title, big value, one detail line and a sparkline.
class Tile : public Widget {
public:
    void Create(Graph::Tint tint, double fixedMax /* 0 = autoscale */);
    void Push(double value);
    void SetText(std::wstring title, std::wstring value, std::wstring detail);

private:
    bool Layered() const override { return true; }
    void PaintStatic(HDC dc, const RECT& rc) override;
    void PaintDynamic(HDC dc, const RECT& rc) override;
    bool Animating() const override { return SmoothAnimation() && data_.size() >= 2; }
    static constexpr size_t kCapacity = 60;
    RECT plot_{};
    Graph::Tint tint_ = Graph::Tint::Cpu;
    double fixedMax_ = 100;
    std::vector<double> data_;
    std::wstring title_, value_, detail_;
    Stopwatch sincePush_;
};

// Dashboard card listing label/value lines, each with an optional icon and meter bar.
class ListCard : public Widget {
public:
    struct Line {
        std::wstring label, value;
        double fraction = -1;        // 0..1 draws a meter under the label; < 0 = none
        COLORREF color = CLR_NONE;   // meter colour (default accent) and value tint
        HICON icon = nullptr;
    };
    void Create(std::wstring title);
    void SetLines(std::vector<Line> lines, std::wstring subtitle = L"");

private:
    void Paint(HDC dc, const RECT& rc) override;
    std::wstring title_, subtitle_;
    std::vector<Line> lines_;
};

// Clickable summary card: glyph badge, caps title, big value and two detail lines.
class ActionCard : public Widget {
public:
    void Create(wchar_t glyph, std::wstring title, Graph::Tint tint, std::function<void()> onClick);
    void SetText(std::wstring value, std::wstring line1, std::wstring line2);
    void SetSelected(bool selected);

private:
    void Paint(HDC dc, const RECT& rc) override;
    void OnMouse(UINT msg, int x, int y) override;
    wchar_t glyph_ = 0;
    std::wstring title_, value_, line1_, line2_;
    Graph::Tint tint_ = Graph::Tint::Cpu;
    std::function<void()> onClick_;
    bool hover_ = false, selected_ = false;
};

// Static line chart of a finished measurement with axes, an average line and a
// hover read-out.
class Chart : public Widget {
public:
    void Create(Graph::Tint tint);
    void SetTint(Graph::Tint tint) {
        tint_ = tint;
        Invalidate();
    }
    // `interval` is the time between samples in seconds.
    void SetSeries(std::vector<double> values, double interval, std::wstring title, std::wstring unit, std::wstring note);

private:
    void Paint(HDC dc, const RECT& rc) override;
    void OnMouse(UINT msg, int x, int y) override;
    Graph::Tint tint_ = Graph::Tint::Cpu;
    std::vector<double> values_;
    double interval_ = 1;
    std::wstring title_, unit_, note_;
    RECT plot_{};
    int hoverX_ = -1;
};

// Themed rich tooltip: a bold title and captioned paragraphs, shown next to a screen
// rectangle (below it, or above when there is no room). One shared popup window.
struct TipSection {
    std::wstring heading, text;
};
void ShowRichTip(const RECT& anchorScreen, const std::wstring& title, const std::vector<TipSection>& sections);
void HideRichTip();

// Small "i" glyph that shows a rich tooltip while hovered: long explanations without
// spending screen space on them.
class InfoBadge : public Widget {
public:
    void Create(std::wstring title, std::vector<TipSection> tip);
    bool onCard = false;  // sits on a card: paint the card colour behind the glyph

private:
    void Paint(HDC dc, const RECT& rc) override;
    void OnMouse(UINT msg, int x, int y) override;
    std::wstring title_;
    std::vector<TipSection> tip_;
    bool hover_ = false;
};

// Card with a caps title and word-wrapped paragraphs, each with an optional heading.
// Scrolls (mouse wheel, or drag the thumb) when the text is taller than the card.
class TextCard : public Widget {
public:
    void Create(std::wstring title);
    void SetSections(std::vector<TipSection> sections, std::wstring subtitle = L"");

private:
    void Paint(HDC dc, const RECT& rc) override;
    void OnMouse(UINT msg, int x, int y) override;
    bool OnWheel(int delta) override;
    void ScrollTo(int offset);
    std::wstring title_, subtitle_;
    std::vector<TipSection> sections_;
    int scroll_ = 0;          // pixels scrolled
    int content_ = 0;         // total text height, measured in Paint
    int view_ = 0;            // visible text height
    RECT track_{}, thumb_{};  // scrollbar, laid out in Paint
    int dragFrom_ = -1, dragScroll_ = 0;
    bool hoverBar_ = false;
};

// Grid of selectable option tiles (DNS providers): title, a mono detail line, tag line
// and a right-aligned badge; hover shows a rich tooltip.
class OptionGrid : public Widget {
public:
    struct Item {
        std::wstring title, detail, tags, badge;
        COLORREF badgeColor = CLR_NONE;
        bool marked = false;  // e.g. "in use": drawn with a green chip
        std::wstring markText;
        std::wstring tipTitle;
        std::vector<TipSection> tip;
    };
    void Create(std::function<void(size_t)> onSelect);
    void SetItems(std::vector<Item> items);
    void SetBadge(size_t index, std::wstring badge, COLORREF color);
    void SetMarked(size_t index, bool marked) {
        if (index < items_.size() && items_[index].marked != marked) {
            items_[index].marked = marked;
            Invalidate();
        }
    }
    void SetSelected(size_t index);
    size_t Selected() const { return selected_; }
    // Height needed to show every tile at the given width.
    int HeightFor(int width) const;

private:
    void Paint(HDC dc, const RECT& rc) override;
    void OnMouse(UINT msg, int x, int y) override;
    bool OnWheel(int delta) override;
    int scroll_ = 0;  // pixels, when the tiles are taller than the widget
    int HitTest(int x, int y) const;
    void Grid(int width, int& cols, int& tileW) const;
    std::function<void(size_t)> onSelect_;
    std::vector<Item> items_;
    std::vector<RECT> tiles_;
    size_t selected_ = SIZE_MAX;
    int hover_ = -1;
};

// Squarified treemap: each block's area is its share of the total.
// Click selects (onPick(tag, false)); double-click opens (onPick(tag, true)).
class Treemap : public Widget {
public:
    struct Item {
        std::wstring label;
        uint64_t size = 0;
        uint64_t tag = 0;
    };
    static constexpr uint64_t kOtherTag = ~0ull;  // the aggregated "everything else" block

    void Create(std::function<void(uint64_t tag, bool open)> onPick);
    void SetItems(std::vector<Item> items, std::wstring title);
    void SetSelected(uint64_t tag);

private:
    struct Block {
        RECT rc;
        size_t item;
    };
    void Paint(HDC dc, const RECT& rc) override;
    void OnMouse(UINT msg, int x, int y) override;
    int HitTest(int x, int y) const;

    std::function<void(uint64_t, bool)> onPick_;
    std::vector<Item> items_;
    std::vector<Block> blocks_;  // laid out during Paint, used for hit-testing
    std::wstring title_;
    uint64_t total_ = 0;
    int hover_ = -1;
    uint64_t selected_ = kOtherTag;
};

// Scrubbable overview of a recording: CPU and memory over time with a draggable cursor.
class Timeline : public Widget {
public:
    void Create(std::function<void(size_t)> onSeek);
    void SetData(std::vector<float> cpu, std::vector<float> mem, uint64_t firstTime, uint64_t lastTime);
    void SetCursor(size_t index);

private:
    void Paint(HDC dc, const RECT& rc) override;
    void OnMouse(UINT msg, int x, int y) override;
    RECT PlotRect(const RECT& client) const;
    std::function<void(size_t)> onSeek_;
    std::vector<float> cpu_, mem_;
    uint64_t first_ = 0, last_ = 0;
    size_t cursor_ = 0;
    bool dragging_ = false;
};

}  // namespace mtm
