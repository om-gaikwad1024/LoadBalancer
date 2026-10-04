#include "MainDlg.h"

#include <algorithm>
#include <string>

#include "Text.h"

namespace {

constexpr UINT_PTR kRefreshTimer = 1;
constexpr COLORREF kUnhealthyColor = RGB(200, 30, 30);
constexpr COLORREF kDrainingColor = RGB(190, 120, 0);

enum BackendColumn { kId, kGroup, kEndpoint, kState, kInFlight, kWeight, kRate, kP50, kP99, kMax, kErrors, kPool, kProbe };
enum EventColumn { kTime, kType, kBackend, kRequest, kMessage };

const lb::SeriesMetrics* find_series(const lb::MetricsSnapshot& m, const std::string& id) {
    for (const auto& s : m.backends) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

CString number(std::uint64_t v) {
    CString s;
    s.Format(L"%llu", static_cast<unsigned long long>(v));
    return s;
}

CString uptime(double seconds) {
    const auto total = static_cast<long long>(seconds);
    CString s;
    s.Format(L"%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    return s;
}

}  // namespace

void SnapshotBridge::on_snapshot(std::unique_ptr<lb::DashboardSnapshot> snapshot) noexcept {
    lb::DashboardSnapshot* raw = snapshot.release();
    if (target_ == nullptr || !::PostMessageW(target_, kMessage, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
}

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
    ON_WM_CLOSE()
    ON_WM_DESTROY()
    ON_WM_TIMER()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_MESSAGE(SnapshotBridge::kMessage, &CMainDlg::OnSnapshot)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_BACKENDS, &CMainDlg::OnBackendsCustomDraw)
END_MESSAGE_MAP()

CMainDlg::CMainDlg(lb::Engine& engine, const lb::ConfigSnapshot& config, std::filesystem::path config_path,
                   bool start_minimized, CWnd* parent)
    : CDialogEx(IDD_MAIN, parent),
      engine_(engine),
      config_path_(std::move(config_path)),
      refresh_ms_(config.dashboard.publish_interval_ms),
      event_rows_(config.dashboard.event_rows),
      start_minimized_(start_minimized) {}

void CMainDlg::DoDataExchange(CDataExchange* dx) {
    CDialogEx::DoDataExchange(dx);
    DDX_Control(dx, IDC_BACKENDS, backends_);
    DDX_Control(dx, IDC_EVENTS, events_);
}

int CMainDlg::scale(int pixels) const { return ::MulDiv(pixels, static_cast<int>(::GetDpiForWindow(m_hWnd)), 96); }

BOOL CMainDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    setup_lists();

    // The tail is what matters (plan IV.15): p99 and max get a large bold font.
    LOGFONTW lf{};
    GetFont()->GetLogFont(&lf);
    lf.lfHeight = lf.lfHeight * 2;
    lf.lfWeight = FW_BOLD;
    tail_font_.CreateFontIndirectW(&lf);
    GetDlgItem(IDC_P99_LABEL)->SetFont(&tail_font_);
    GetDlgItem(IDC_MAX_LABEL)->SetFont(&tail_font_);
    SetDlgItemTextW(IDC_P99_LABEL, L"p99  –");
    SetDlgItemTextW(IDC_MAX_LABEL, L"max  –");

    bridge_.attach(m_hWnd);
    engine_.set_snapshot_sink(&bridge_);
    std::string error;
    if (!engine_.start(&error)) {
        AfxMessageBox(L"The proxy could not start:\n\n" + app::from_utf8(error), MB_ICONERROR | MB_OK);
        exit_code_ = 1;
        EndDialog(IDABORT);
        return TRUE;
    }
    engine_running_ = true;
    SetTimer(kRefreshTimer, refresh_ms_, nullptr);
    // Hot reload: the proxy keeps running without it, so a watch failure is only a warning.
    if (!engine_.watch_config_file(config_path_, &error)) {
        AfxMessageBox(L"The proxy is running, but config changes will not be picked up:\n\n" + app::from_utf8(error),
                      MB_ICONWARNING | MB_OK);
    }

    CRect client;
    GetClientRect(&client);
    layout(client.Width(), client.Height());
    if (start_minimized_) ShowWindow(SW_SHOWMINNOACTIVE);
    return TRUE;
}

void CMainDlg::setup_lists() {
    const DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES;
    backends_.SetExtendedStyle(ex);
    events_.SetExtendedStyle(ex);
    const struct {
        const wchar_t* title;
        int width;
        int format;
    } backend_columns[] = {
        {L"Backend", 90, LVCFMT_LEFT},   {L"Group", 70, LVCFMT_LEFT},     {L"Endpoint", 120, LVCFMT_LEFT},
        {L"State", 80, LVCFMT_LEFT},     {L"In flight", 65, LVCFMT_RIGHT}, {L"Weight", 55, LVCFMT_RIGHT},
        {L"Req/s", 65, LVCFMT_RIGHT},    {L"p50", 70, LVCFMT_RIGHT},       {L"p99", 75, LVCFMT_RIGHT},
        {L"Max", 75, LVCFMT_RIGHT},      {L"Errors", 60, LVCFMT_RIGHT},    {L"Pool open/idle", 95, LVCFMT_RIGHT},
        {L"Last probe", 260, LVCFMT_LEFT},
    };
    int i = 0;
    for (const auto& c : backend_columns) backends_.InsertColumn(i++, c.title, c.format, scale(c.width));

    const struct {
        const wchar_t* title;
        int width;
    } event_columns[] = {
        {L"Time (UTC)", 95}, {L"Event", 170}, {L"Backend", 80}, {L"Request id", 120}, {L"Message", 600},
    };
    i = 0;
    for (const auto& c : event_columns) events_.InsertColumn(i++, c.title, LVCFMT_LEFT, scale(c.width));
}

void CMainDlg::layout(int cx, int cy) {
    if (backends_.GetSafeHwnd() == nullptr || cx <= 0 || cy <= 0) return;
    const int m = scale(8);
    const int line = scale(18);
    const int big = scale(34);
    const int w = cx - 2 * m;
    int y = m;
    GetDlgItem(IDC_STATUS)->MoveWindow(m, y, w, line);
    y += line + scale(4);
    GetDlgItem(IDC_P99_LABEL)->MoveWindow(m, y, w / 2, big);
    GetDlgItem(IDC_MAX_LABEL)->MoveWindow(m + w / 2, y, w - w / 2, big);
    y += big;
    GetDlgItem(IDC_LATENCY_DETAIL)->MoveWindow(m, y, w, line);
    y += line + scale(6);

    const int lists = std::max(cy - y - m - 2 * line - scale(6), scale(100));
    const int backend_height = lists * 2 / 5;
    GetDlgItem(IDC_BACKENDS_CAPTION)->MoveWindow(m, y, w, line);
    y += line;
    backends_.MoveWindow(m, y, w, backend_height);
    y += backend_height + scale(6);
    GetDlgItem(IDC_EVENTS_CAPTION)->MoveWindow(m, y, w, line);
    y += line;
    events_.MoveWindow(m, y, w, std::max(cy - y - m, scale(40)));
    Invalidate();
}

void CMainDlg::OnSize(UINT type, int cx, int cy) {
    CDialogEx::OnSize(type, cx, cy);
    if (type != SIZE_MINIMIZED) layout(cx, cy);
}

void CMainDlg::OnGetMinMaxInfo(MINMAXINFO* info) {
    CDialogEx::OnGetMinMaxInfo(info);
    if (m_hWnd != nullptr) {
        info->ptMinTrackSize.x = scale(720);
        info->ptMinTrackSize.y = scale(480);
    }
}

// UI thread: take ownership and keep only the newest snapshot; its events are queued so
// none is lost when several snapshots arrive between two repaints.
LRESULT CMainDlg::OnSnapshot(WPARAM, LPARAM snapshot) {
    std::unique_ptr<lb::DashboardSnapshot> s(reinterpret_cast<lb::DashboardSnapshot*>(snapshot));
    for (auto& e : s->new_events) pending_events_.push_back(std::move(e));
    s->new_events.clear();
    latest_ = std::move(s);
    return 0;
}

void CMainDlg::OnTimer(UINT_PTR id) {
    if (id == kRefreshTimer) render();
    CDialogEx::OnTimer(id);
}

void CMainDlg::render() {
    if (!pending_events_.empty()) render_events();
    if (!latest_ || latest_->sequence == rendered_sequence_) return;
    render_status(*latest_);
    render_latency(*latest_);
    render_backends(*latest_);
    rendered_sequence_ = latest_->sequence;
}

void CMainDlg::render_status(const lb::DashboardSnapshot& s) {
    std::size_t healthy = 0;
    std::size_t unhealthy = 0;
    std::size_t draining = 0;
    for (const auto& b : s.backends) {
        if (b.state == lb::BackendState::Healthy) ++healthy;
        else if (b.state == lb::BackendState::Unhealthy) ++unhealthy;
        else ++draining;
    }
    const auto& sys = s.metrics.system;
    CString text;
    text.Format(L"Listening on %s:%u · %u workers · up %s · backends: %zu healthy, %zu unhealthy, "
                L"%zu draining · %llu open connections · %.1f req/s · %.2f%% errors (last %.0f s) · "
                L"reloads: %llu applied, %llu rejected",
                app::from_utf8(s.listen_address).GetString(), s.listen_port, s.workers, uptime(s.uptime_seconds).GetString(),
                healthy, unhealthy, draining, static_cast<unsigned long long>(s.stats.connections_active),
                sys.requests_per_second, 100.0 * sys.error_rate, s.metrics.window_seconds,
                static_cast<unsigned long long>(s.stats.reloads_accepted),
                static_cast<unsigned long long>(s.stats.reloads_rejected));
    SetDlgItemTextW(IDC_STATUS, text);
}

void CMainDlg::render_latency(const lb::DashboardSnapshot& s) {
    const auto& total = s.metrics.system.total_window;
    const auto& backend = s.metrics.system.backend_window;
    if (total.count == 0) {
        SetDlgItemTextW(IDC_P99_LABEL, L"p99  –");
        SetDlgItemTextW(IDC_MAX_LABEL, L"max  –");
        CString idle;
        idle.Format(L"No requests in the last %.0f s", s.metrics.window_seconds);
        SetDlgItemTextW(IDC_LATENCY_DETAIL, idle);
        return;
    }
    SetDlgItemTextW(IDC_P99_LABEL, L"p99  " + app::format_ms(total.p99_ms));
    SetDlgItemTextW(IDC_MAX_LABEL, L"max  " + app::format_ms(total.max_ms));
    CString detail;
    detail.Format(L"Last %.0f s, %llu requests · p50 %s · p95 %s · mean %s · backend only: p50 %s, "
                  L"p99 %s · since start: p99 %s, max %s",
                  s.metrics.window_seconds, static_cast<unsigned long long>(total.count),
                  app::format_ms(total.p50_ms).GetString(), app::format_ms(total.p95_ms).GetString(),
                  app::format_ms(total.mean_ms).GetString(), app::format_ms(backend.p50_ms).GetString(),
                  app::format_ms(backend.p99_ms).GetString(),
                  app::format_ms(s.metrics.system.total_since_start.p99_ms).GetString(),
                  app::format_ms(s.metrics.system.total_since_start.max_ms).GetString());
    SetDlgItemTextW(IDC_LATENCY_DETAIL, detail);
}

void CMainDlg::render_backends(const lb::DashboardSnapshot& s) {
    backends_.SetRedraw(FALSE);
    const int rows = static_cast<int>(s.backends.size());
    while (backends_.GetItemCount() > rows) backends_.DeleteItem(backends_.GetItemCount() - 1);
    while (backends_.GetItemCount() < rows) backends_.InsertItem(backends_.GetItemCount(), L"");
    row_states_.resize(s.backends.size());

    for (int r = 0; r < rows; ++r) {
        const lb::BackendStats& b = s.backends[static_cast<std::size_t>(r)];
        row_states_[static_cast<std::size_t>(r)] = b.state;
        const lb::SeriesMetrics* m = find_series(s.metrics, b.id);
        const bool has_latency = m != nullptr && m->total_window.count > 0;
        CString rate;
        CString errors;
        rate.Format(L"%.1f", m != nullptr ? m->requests_per_second : 0.0);
        errors.Format(L"%.1f%%", m != nullptr ? 100.0 * m->error_rate : 0.0);
        CString pool;
        pool.Format(L"%llu / %llu", static_cast<unsigned long long>(b.open_connections),
                    static_cast<unsigned long long>(b.idle_connections));
        CString probe = b.last_probe_error.empty() ? CString(L"ok") : app::from_utf8(b.last_probe_error);
        if (b.health_probes == 0) probe = L"not probed yet";

        backends_.SetItemText(r, kId, app::from_utf8(b.id));
        backends_.SetItemText(r, kGroup, app::from_utf8(b.group));
        backends_.SetItemText(r, kEndpoint, app::from_utf8(b.endpoint));
        backends_.SetItemText(r, kState, app::from_utf8(std::string(lb::to_string(b.state))));
        backends_.SetItemText(r, kInFlight, number(b.in_flight));
        backends_.SetItemText(r, kWeight, number(b.weight));
        backends_.SetItemText(r, kRate, rate);
        backends_.SetItemText(r, kP50, has_latency ? app::format_ms(m->total_window.p50_ms) : CString(L"–"));
        backends_.SetItemText(r, kP99, has_latency ? app::format_ms(m->total_window.p99_ms) : CString(L"–"));
        backends_.SetItemText(r, kMax, has_latency ? app::format_ms(m->total_window.max_ms) : CString(L"–"));
        backends_.SetItemText(r, kErrors, errors);
        backends_.SetItemText(r, kPool, pool);
        backends_.SetItemText(r, kProbe, probe);
    }
    backends_.SetRedraw(TRUE);
    backends_.Invalidate(FALSE);
}

// Newest first, capped at dashboard.event_rows.
void CMainDlg::render_events() {
    events_.SetRedraw(FALSE);
    for (const auto& e : pending_events_) {
        const CString time = e.wall_time.size() >= 23 ? app::from_utf8(e.wall_time.substr(11, 12)) : app::from_utf8(e.wall_time);
        events_.InsertItem(0, time);
        events_.SetItemText(0, kType, app::from_utf8(e.type));
        events_.SetItemText(0, kBackend, app::from_utf8(e.backend));
        events_.SetItemText(0, kRequest, app::from_utf8(e.request_id));
        events_.SetItemText(0, kMessage, app::from_utf8(e.message));
    }
    pending_events_.clear();
    while (events_.GetItemCount() > static_cast<int>(event_rows_)) events_.DeleteItem(events_.GetItemCount() - 1);
    events_.SetRedraw(TRUE);
    events_.Invalidate(FALSE);
}

void CMainDlg::OnBackendsCustomDraw(NMHDR* header, LRESULT* result) {
    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
    *result = CDRF_DODEFAULT;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
        *result = CDRF_NOTIFYITEMDRAW;
    } else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const std::size_t row = cd->nmcd.dwItemSpec;
        if (row < row_states_.size()) {
            if (row_states_[row] == lb::BackendState::Unhealthy) cd->clrText = kUnhealthyColor;
            else if (row_states_[row] == lb::BackendState::Draining) cd->clrText = kDrainingColor;
        }
    }
}

void CMainDlg::stop_engine() {
    if (!engine_running_) return;
    engine_running_ = false;
    KillTimer(kRefreshTimer);
    CWaitCursor wait;
    SetDlgItemTextW(IDC_STATUS, L"Stopping: finishing in-flight requests...");
    UpdateWindow();
    engine_.stop();  // joins the publisher: no further snapshots after this
    // Free snapshots that were posted but never handled.
    MSG msg;
    while (::PeekMessageW(&msg, m_hWnd, SnapshotBridge::kMessage, SnapshotBridge::kMessage, PM_REMOVE)) {
        delete reinterpret_cast<lb::DashboardSnapshot*>(msg.lParam);
    }
}

void CMainDlg::OnClose() {
    stop_engine();
    EndDialog(IDOK);
}

void CMainDlg::OnDestroy() {
    stop_engine();  // whatever closed the dialog, the engine stops before the window goes
    CDialogEx::OnDestroy();
}
