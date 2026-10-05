#pragma once

#include "framework.h"

#include <vector>

// A line graph drawn with GDI into an off-screen bitmap and copied to the screen in one blit,
// so it never flickers (plan IV.17: double-buffered custom control). One line per series,
// one value per time slice, newest on the right. UI thread only.
class CGraphCtrl : public CWnd {
public:
    struct Series {
        CString name;
        COLORREF color = RGB(0, 0, 0);
        std::vector<double> values;  // oldest first; NaN = no value (a gap in the line)
    };

    // Creates the control as a child of `parent` with the given control id.
    BOOL Create(CWnd* parent, UINT id, const CString& title);

    // `points`: how many values the x axis spans; `seconds_per_point` labels it.
    void set_data(std::vector<Series> series, std::size_t points, double seconds_per_point, const CString& unit);

    // Window text, for tests and accessibility: "<title>: N series, P points, max V <unit>".
    CString summary() const;

protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    afx_msg void OnSize(UINT type, int cx, int cy);
    DECLARE_MESSAGE_MAP()

private:
    void draw(CDC& dc, const CRect& area) const;
    int scale(int pixels) const;

    CString title_;
    CString unit_;
    std::vector<Series> series_;
    std::size_t points_ = 0;
    double seconds_per_point_ = 1;
    double max_ = 0;  // top of the y axis
    CFont font_;
};
