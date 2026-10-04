#pragma once

#include "framework.h"

#include <string_view>

namespace app {

// The engine speaks UTF-8 std::string; the UI speaks CString (UTF-16). Conversion happens
// only here, at the UI boundary (plan II.6).
inline CString from_utf8(std::string_view text) {
    if (text.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    CString out;
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.GetBuffer(n), n);
    out.ReleaseBuffer(n);
    return out;
}

inline CString format_ms(double ms) {
    CString s;
    if (ms < 10.0) s.Format(L"%.2f ms", ms);
    else if (ms < 1000.0) s.Format(L"%.1f ms", ms);
    else s.Format(L"%.0f ms", ms);
    return s;
}

}  // namespace app
