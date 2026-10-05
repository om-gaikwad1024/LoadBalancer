#include "GraphCtrl.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr wchar_t kClassName[] = L"LbGraph";

// The smallest "nice" value (1, 2 or 5 times a power of ten) at or above v.
double nice_ceiling(double v) {
    if (!(v > 0)) return 1;
    const double magnitude = std::pow(10.0, std::floor(std::log10(v)));
    for (const double step : {1.0, 2.0, 5.0, 10.0}) {
        if (v <= step * magnitude) return step * magnitude;
    }
    return 10 * magnitude;
}

CString format_value(double v) {
    CString s;
    if (v >= 100 || v == std::floor(v)) s.Format(L"%.0f", v);
    else if (v >= 10) s.Format(L"%.1f", v);
    else s.Format(L"%.2f", v);
    return s;
}

}  // namespace

BEGIN_MESSAGE_MAP(CGraphCtrl, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_SIZE()
END_MESSAGE_MAP()

BOOL CGraphCtrl::Create(CWnd* parent, UINT id, const CString& title) {
    WNDCLASSW wc{};
    if (!::GetClassInfoW(AfxGetInstanceHandle(), kClassName, &wc)) {
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = AfxGetInstanceHandle();
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!AfxRegisterClass(&wc)) return FALSE;
    }
    title_ = title;
    if (!CWnd::Create(kClassName, title, WS_CHILD | WS_VISIBLE | WS_BORDER, CRect(0, 0, 10, 10), parent, id)) return FALSE;
    LOGFONTW lf{};
    parent->GetFont()->GetLogFont(&lf);
    font_.CreateFontIndirectW(&lf);
    return TRUE;
}

int CGraphCtrl::scale(int pixels) const { return ::MulDiv(pixels, static_cast<int>(::GetDpiForWindow(m_hWnd)), 96); }

void CGraphCtrl::set_data(std::vector<Series> series, std::size_t points, double seconds_per_point, const CString& unit) {
    series_ = std::move(series);
    points_ = points;
    seconds_per_point_ = seconds_per_point;
    unit_ = unit;
    double highest = 0;
    for (const auto& s : series_) {
        for (const double v : s.values) {
            if (!std::isnan(v)) highest = std::max(highest, v);
        }
    }
    max_ = nice_ceiling(highest);
    SetWindowTextW(summary());
    Invalidate(FALSE);
}

CString CGraphCtrl::summary() const {
    std::size_t values = 0;
    double highest = 0;
    for (const auto& s : series_) {
        for (const double v : s.values) {
            if (std::isnan(v)) continue;
            ++values;
            highest = std::max(highest, v);
        }
    }
    CString text;
    text.Format(L"%s: %zu series, %zu points, max %s %s", title_.GetString(), series_.size(), values,
                format_value(highest).GetString(), unit_.GetString());
    return text;
}

BOOL CGraphCtrl::OnEraseBkgnd(CDC*) { return TRUE; }  // OnPaint covers every pixel

void CGraphCtrl::OnSize(UINT type, int cx, int cy) {
    CWnd::OnSize(type, cx, cy);
    Invalidate(FALSE);
}

void CGraphCtrl::OnPaint() {
    CPaintDC screen(this);
    CRect area;
    GetClientRect(&area);
    if (area.IsRectEmpty()) return;
    // Double buffering: draw everything off screen, then one copy.
    CDC memory;
    memory.CreateCompatibleDC(&screen);
    CBitmap bitmap;
    bitmap.CreateCompatibleBitmap(&screen, area.Width(), area.Height());
    CBitmap* old_bitmap = memory.SelectObject(&bitmap);
    draw(memory, area);
    screen.BitBlt(0, 0, area.Width(), area.Height(), &memory, 0, 0, SRCCOPY);
    memory.SelectObject(old_bitmap);
}

void CGraphCtrl::draw(CDC& dc, const CRect& area) const {
    dc.FillSolidRect(area, RGB(255, 255, 255));
    CFont* old_font = dc.SelectObject(const_cast<CFont*>(&font_));
    dc.SetBkMode(TRANSPARENT);
    const int line = scale(16);
    const int pad = scale(6);

    // Title and legend on top.
    dc.SetTextColor(RGB(40, 40, 40));
    dc.TextOutW(pad, pad / 2, title_);
    int legend_x = pad + dc.GetTextExtent(title_).cx + scale(16);
    for (const auto& s : series_) {
        CPen pen(PS_SOLID, scale(2), s.color);
        CPen* old = dc.SelectObject(&pen);
        dc.MoveTo(legend_x, pad / 2 + line / 2);
        dc.LineTo(legend_x + scale(14), pad / 2 + line / 2);
        dc.SelectObject(old);
        dc.TextOutW(legend_x + scale(18), pad / 2, s.name);
        legend_x += scale(18) + dc.GetTextExtent(s.name).cx + scale(12);
    }

    // Plot area with a y axis on the left and a time axis below.
    const CString top_label = format_value(max_) + L" " + unit_;
    const int axis_width = dc.GetTextExtent(top_label).cx + pad;
    // Below the title row, with room for the top label, which is centred on the top gridline.
    CRect plot(area.left + pad + axis_width, area.top + line + line / 2 + pad, area.right - pad, area.bottom - line - pad);
    if (plot.Width() < scale(20) || plot.Height() < scale(20)) {
        dc.SelectObject(old_font);
        return;
    }
    CPen grid(PS_SOLID, 1, RGB(225, 225, 225));
    CPen* old_pen = dc.SelectObject(&grid);
    dc.SetTextColor(RGB(110, 110, 110));
    for (int i = 0; i <= 4; ++i) {
        const int y = plot.bottom - plot.Height() * i / 4;
        dc.MoveTo(plot.left, y);
        dc.LineTo(plot.right, y);
        const CString label = i == 4 ? top_label : format_value(max_ * i / 4);
        dc.TextOutW(plot.left - pad - dc.GetTextExtent(label).cx, y - line / 2, label);
    }
    dc.SelectObject(old_pen);
    CString span;
    span.Format(L"last %.0f s", static_cast<double>(points_) * seconds_per_point_);
    dc.TextOutW(plot.left, plot.bottom + pad / 2, span);
    dc.TextOutW(plot.right - dc.GetTextExtent(L"now").cx, plot.bottom + pad / 2, L"now");

    // One line per series; values are right-aligned so the newest point is at "now".
    if (points_ > 1) {
        for (const auto& s : series_) {
            CPen pen(PS_SOLID, scale(2), s.color);
            CPen* old = dc.SelectObject(&pen);
            const std::size_t n = std::min(s.values.size(), points_);
            const std::size_t first_slot = points_ - n;
            bool drawing = false;
            for (std::size_t i = 0; i < n; ++i) {
                const double v = s.values[s.values.size() - n + i];
                if (std::isnan(v)) {
                    drawing = false;
                    continue;
                }
                const int x = plot.left + static_cast<int>((plot.Width() - 1) * (first_slot + i) / (points_ - 1));
                const int y = plot.bottom - static_cast<int>(std::lround(std::min(v, max_) / max_ * plot.Height()));
                if (drawing) dc.LineTo(x, y);
                else dc.MoveTo(x, y);
                drawing = true;
            }
            dc.SelectObject(old);
        }
    }
    dc.SelectObject(old_font);
}
