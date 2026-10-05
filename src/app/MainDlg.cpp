#include "MainDlg.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

#include "AdminDialogs.h"
#include "Text.h"

namespace {

constexpr UINT_PTR kRefreshTimer = 1;
constexpr COLORREF kUnhealthyColor = RGB(200, 30, 30);
constexpr COLORREF kDrainingColor = RGB(190, 120, 0);
constexpr COLORREF kErrorColor = RGB(200, 30, 30);
constexpr COLORREF kWarningColor = RGB(170, 105, 0);
constexpr COLORREF kPalette[] = {RGB(31, 119, 180), RGB(255, 127, 14), RGB(44, 160, 44),  RGB(214, 39, 40),
                                 RGB(148, 103, 189), RGB(140, 86, 75), RGB(227, 119, 194), RGB(127, 127, 127)};
constexpr wchar_t kAllTypes[] = L"(all event types)";
constexpr wchar_t kAllBackends[] = L"(all backends)";

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

// Keeps the "(all ...)" entry first and the rest in order.
void insert_sorted(CComboBox& combo, const CString& text) {
    int i = 1;
    CString existing;
    for (; i < combo.GetCount(); ++i) {
        combo.GetLBText(i, existing);
        if (text.Compare(existing) < 0) break;
    }
    combo.InsertString(i, text);
}

std::string selected_filter(CComboBox& combo) {
    const int i = combo.GetCurSel();
    if (i <= 0) return {};  // the "(all ...)" entry
    CString text;
    combo.GetLBText(i, text);
    return app::to_utf8(text);
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
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_EVENTS, &CMainDlg::OnEventsCustomDraw)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_BACKENDS, &CMainDlg::OnBackendSelected)
    ON_CBN_SELCHANGE(IDC_EVENT_TYPE, &CMainDlg::OnFilterChanged)
    ON_CBN_SELCHANGE(IDC_EVENT_BACKEND, &CMainDlg::OnFilterChanged)
    ON_EN_CHANGE(IDC_EVENT_SEARCH, &CMainDlg::OnFilterChanged)
    ON_BN_CLICKED(IDC_EVENT_PROBLEMS, &CMainDlg::OnFilterChanged)
    ON_BN_CLICKED(IDC_ADD_BACKEND, &CMainDlg::OnAddBackend)
    ON_BN_CLICKED(IDC_EDIT_BACKEND, &CMainDlg::OnEditBackend)
    ON_BN_CLICKED(IDC_REMOVE_BACKEND, &CMainDlg::OnRemoveBackend)
    ON_BN_CLICKED(IDC_DRAIN_BACKEND, &CMainDlg::OnDrainBackend)
    ON_BN_CLICKED(IDC_UNDRAIN_BACKEND, &CMainDlg::OnUndrainBackend)
    ON_BN_CLICKED(IDC_ROUTING, &CMainDlg::OnRoutingRules)
END_MESSAGE_MAP()

CMainDlg::CMainDlg(lb::Engine& engine, const lb::ConfigSnapshot& config, std::filesystem::path config_path,
                   bool start_minimized, CWnd* parent)
    : CDialogEx(IDD_MAIN, parent),
      engine_(engine),
      config_path_(std::move(config_path)),
      refresh_ms_(config.dashboard.publish_interval_ms),
      event_rows_(config.dashboard.event_rows),
      graph_points_(config.dashboard.graph_points),
      start_minimized_(start_minimized) {}

void CMainDlg::DoDataExchange(CDataExchange* dx) {
    CDialogEx::DoDataExchange(dx);
    DDX_Control(dx, IDC_BACKENDS, backends_);
    DDX_Control(dx, IDC_EVENTS, events_);
    DDX_Control(dx, IDC_EVENT_TYPE, event_type_);
    DDX_Control(dx, IDC_EVENT_BACKEND, event_backend_);
}

int CMainDlg::scale(int pixels) const { return ::MulDiv(pixels, static_cast<int>(::GetDpiForWindow(m_hWnd)), 96); }

BOOL CMainDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    setup_lists();
    rate_graph_.Create(this, IDC_GRAPH_RATE, L"Requests per second");
    latency_graph_.Create(this, IDC_GRAPH_LATENCY, L"p99 latency");
    event_type_.AddString(kAllTypes);
    event_type_.SetCurSel(0);
    event_backend_.AddString(kAllBackends);
    event_backend_.SetCurSel(0);
    SetDlgItemTextW(IDC_EVENT_SEARCH, L"");
    ::SendMessageW(GetDlgItem(IDC_EVENT_SEARCH)->m_hWnd, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"Search message, request id..."));

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
    update_admin_buttons();

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
    // Hot reload and admin saves need the file; the proxy keeps running without them.
    if (!engine_.watch_config_file(config_path_, &error)) {
        AfxMessageBox(L"The proxy is running, but config changes will not be picked up or saved:\n\n" +
                          app::from_utf8(error),
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
    if (backends_.GetSafeHwnd() == nullptr || rate_graph_.GetSafeHwnd() == nullptr || cx <= 0 || cy <= 0) return;
    const int m = scale(8);
    const int gap = scale(6);
    const int line = scale(18);
    const int row = scale(24);  // buttons, combo boxes
    const int big = scale(34);
    const int w = cx - 2 * m;
    int y = m;
    GetDlgItem(IDC_STATUS)->MoveWindow(m, y, w, line);
    y += line + scale(4);
    GetDlgItem(IDC_P99_LABEL)->MoveWindow(m, y, w / 2, big);
    GetDlgItem(IDC_MAX_LABEL)->MoveWindow(m + w / 2, y, w - w / 2, big);
    y += big;
    GetDlgItem(IDC_LATENCY_DETAIL)->MoveWindow(m, y, w, line);
    y += line + gap;

    // Backends caption with the admin buttons on the same row.
    int x = m;
    const auto place = [&](int id, int width, int height) {
        GetDlgItem(id)->MoveWindow(x, y, width, height);
        x += width + scale(4);
    };
    GetDlgItem(IDC_BACKENDS_CAPTION)->MoveWindow(x, y + scale(4), scale(70), line);  // on the buttons' baseline
    x += scale(74);
    place(IDC_ADD_BACKEND, scale(110), row);
    place(IDC_EDIT_BACKEND, scale(64), row);
    place(IDC_REMOVE_BACKEND, scale(72), row);
    place(IDC_DRAIN_BACKEND, scale(60), row);
    place(IDC_UNDRAIN_BACKEND, scale(124), row);
    place(IDC_ROUTING, scale(116), row);
    GetDlgItem(IDC_ADMIN_NOTE)->MoveWindow(x + scale(4), y + scale(3), std::max(m + w - x - scale(4), scale(40)), line);
    y += row + scale(4);

    const int remaining = std::max(cy - y - m - row - 2 * gap, scale(240));
    const int backend_height = remaining * 26 / 100;
    const int graph_height = remaining * 30 / 100;
    backends_.MoveWindow(m, y, w, backend_height);
    y += backend_height + gap;
    rate_graph_.MoveWindow(m, y, (w - gap) / 2, graph_height);
    latency_graph_.MoveWindow(m + (w - gap) / 2 + gap, y, w - (w - gap) / 2 - gap, graph_height);
    y += graph_height + gap;

    // Log caption with the filters on the same row.
    x = m;
    GetDlgItem(IDC_EVENTS_CAPTION)->MoveWindow(x, y + scale(4), scale(130), line);
    x += scale(134);
    place(IDC_EVENT_TYPE, scale(190), scale(300));  // a drop-down list's height includes the list
    place(IDC_EVENT_BACKEND, scale(130), scale(300));
    place(IDC_EVENT_SEARCH, scale(200), scale(22));
    GetDlgItem(IDC_EVENT_PROBLEMS)->MoveWindow(x, y + scale(3), scale(110), line);
    x += scale(114);
    GetDlgItem(IDC_EVENT_COUNT)->MoveWindow(x, y + scale(3), std::max(m + w - x, scale(40)), line);
    y += row + scale(4);
    events_.MoveWindow(m, y, w, std::max(cy - y - m, scale(60)));
    Invalidate();
}

void CMainDlg::OnSize(UINT type, int cx, int cy) {
    CDialogEx::OnSize(type, cx, cy);
    if (type != SIZE_MINIMIZED) layout(cx, cy);
}

void CMainDlg::OnGetMinMaxInfo(MINMAXINFO* info) {
    CDialogEx::OnGetMinMaxInfo(info);
    if (m_hWnd != nullptr) {
        info->ptMinTrackSize.x = scale(980);
        info->ptMinTrackSize.y = scale(640);
    }
}

// UI thread: take ownership and keep only the newest snapshot; its events are queued so
// none is lost when several snapshots arrive between two repaints.
LRESULT CMainDlg::OnSnapshot(WPARAM, LPARAM snapshot) {
    std::unique_ptr<lb::DashboardSnapshot> s(reinterpret_cast<lb::DashboardSnapshot*>(snapshot));
    for (auto& e : s->new_events) pending_events_.push_back(std::move(e));
    s->new_events.clear();
    update_history(*s);  // every snapshot's slices, so none is missed between repaints
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
    render_graphs(*latest_);
    update_backend_filter(*latest_);
    update_admin_buttons();
    rendered_sequence_ = latest_->sequence;
}

void CMainDlg::render_status(const lb::DashboardSnapshot& s) {
    std::size_t healthy = 0;
    std::size_t unhealthy = 0;
    std::size_t draining = 0;
    std::size_t drained = 0;
    for (const auto& b : s.backends) {
        if (b.state == lb::BackendState::Healthy) ++healthy;
        else if (b.state == lb::BackendState::Unhealthy) ++unhealthy;
        else if (b.state == lb::BackendState::Draining) ++draining;
        else ++drained;
    }
    const auto& sys = s.metrics.system;
    CString text;
    text.Format(L"Listening on %s:%u · %u workers · up %s · backends: %zu healthy, %zu unhealthy, "
                L"%zu draining, %zu drained · %llu open connections · %.1f req/s · %.2f%% errors (last %.0f s) · "
                L"reloads: %llu applied, %llu rejected",
                app::from_utf8(s.listen_address).GetString(), s.listen_port, s.workers, uptime(s.uptime_seconds).GetString(),
                healthy, unhealthy, draining, drained, static_cast<unsigned long long>(s.stats.connections_active),
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
        if (b.state == lb::BackendState::Drained) probe = L"not probed (drained)";

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

// ---- Graphs (plan IV.17 phase 2) --------------------------------------------------------

void CMainDlg::update_history(const lb::DashboardSnapshot& s) {
    for (const auto& series : s.metrics.backends) {
        auto& slices = history_[series.id];
        for (const auto& p : series.slices) slices[p.slice] = p;  // a slice seen again is replaced: it is final
        while (slices.size() > graph_points_) slices.erase(slices.begin());
    }
}

void CMainDlg::render_graphs(const lb::DashboardSnapshot& s) {
    std::int64_t newest = -1;
    for (const auto& series : s.metrics.backends) {
        if (!series.slices.empty()) newest = std::max(newest, series.slices.back().slice);
    }
    const double slice_seconds = s.metrics.slice_seconds > 0 ? s.metrics.slice_seconds : 1.0;
    std::vector<CGraphCtrl::Series> rate;
    std::vector<CGraphCtrl::Series> latency;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::size_t color = 0;
    for (const auto& b : s.backends) {  // current backends only, in the list's order
        CGraphCtrl::Series r{app::from_utf8(b.id), kPalette[color % std::size(kPalette)], {}};
        CGraphCtrl::Series l = r;
        ++color;
        const auto it = history_.find(b.id);
        if (newest >= 0) {
            for (std::int64_t slice = newest - static_cast<std::int64_t>(graph_points_) + 1; slice <= newest; ++slice) {
                const lb::SlicePoint* p = nullptr;
                if (it != history_.end()) {
                    const auto found = it->second.find(slice);
                    if (found != it->second.end()) p = &found->second;
                }
                r.values.push_back(p != nullptr ? static_cast<double>(p->requests) / slice_seconds : nan);
                l.values.push_back(p != nullptr && p->requests > 0 ? p->p99_ms : nan);
            }
        }
        rate.push_back(std::move(r));
        latency.push_back(std::move(l));
    }
    rate_graph_.set_data(std::move(rate), graph_points_, slice_seconds, L"req/s");
    latency_graph_.set_data(std::move(latency), graph_points_, slice_seconds, L"ms");
}

// ---- Log view (plan IV.17 phase 2: searchable, filterable) -------------------------------

void CMainDlg::insert_event_row(int row, const lb::LoggedEvent& e) {
    const CString time = e.wall_time.size() >= 23 ? app::from_utf8(e.wall_time.substr(11, 12)) : app::from_utf8(e.wall_time);
    events_.InsertItem(row, time);
    events_.SetItemText(row, kType, app::from_utf8(e.type));
    events_.SetItemText(row, kBackend, app::from_utf8(e.backend));
    events_.SetItemText(row, kRequest, app::from_utf8(e.request_id));
    events_.SetItemText(row, kMessage, app::from_utf8(e.message));
    events_.SetItemData(row, static_cast<DWORD_PTR>(lb::log::event_severity(e.type)));
}

// Newest first; at most dashboard.event_rows kept, whether shown or filtered out.
void CMainDlg::render_events() {
    events_.SetRedraw(FALSE);
    for (auto& e : pending_events_) {
        if (event_types_.insert(e.type).second) insert_sorted(event_type_, app::from_utf8(e.type));
        if (!e.backend.empty() && event_backends_.insert(e.backend).second) {
            insert_sorted(event_backend_, app::from_utf8(e.backend));
        }
        if (lb::log::matches(e, filter_)) insert_event_row(0, e);
        all_events_.push_front(std::move(e));
        if (all_events_.size() > event_rows_) all_events_.pop_back();
    }
    pending_events_.clear();
    while (events_.GetItemCount() > static_cast<int>(event_rows_)) events_.DeleteItem(events_.GetItemCount() - 1);
    events_.SetRedraw(TRUE);
    events_.Invalidate(FALSE);
    update_event_count();
}

void CMainDlg::rebuild_events() {
    events_.SetRedraw(FALSE);
    events_.DeleteAllItems();
    int row = 0;
    for (const auto& e : all_events_) {
        if (lb::log::matches(e, filter_)) insert_event_row(row++, e);
    }
    events_.SetRedraw(TRUE);
    events_.Invalidate(FALSE);
    update_event_count();
}

void CMainDlg::update_event_count() {
    CString text;
    text.Format(L"Showing %d of %zu events", events_.GetItemCount(), all_events_.size());
    SetDlgItemTextW(IDC_EVENT_COUNT, text);
}

void CMainDlg::OnFilterChanged() {
    CString search;
    GetDlgItemTextW(IDC_EVENT_SEARCH, search);
    filter_.type = selected_filter(event_type_);
    filter_.backend = selected_filter(event_backend_);
    filter_.text = app::to_utf8(search.Trim());
    filter_.problems_only = IsDlgButtonChecked(IDC_EVENT_PROBLEMS) == BST_CHECKED;
    rebuild_events();
}

void CMainDlg::update_backend_filter(const lb::DashboardSnapshot& s) {
    for (const auto& b : s.backends) {
        if (event_backends_.insert(b.id).second) insert_sorted(event_backend_, app::from_utf8(b.id));
    }
}

void CMainDlg::OnEventsCustomDraw(NMHDR* header, LRESULT* result) {
    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
    *result = CDRF_DODEFAULT;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
        *result = CDRF_NOTIFYITEMDRAW;
    } else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const auto severity = static_cast<lb::log::Severity>(cd->nmcd.lItemlParam);
        if (severity == lb::log::Severity::Error) cd->clrText = kErrorColor;
        else if (severity == lb::log::Severity::Warning) cd->clrText = kWarningColor;
    }
}

// ---- Admin console (plan IV.17 phase 2) --------------------------------------------------

const lb::BackendStats* CMainDlg::selected_backend() const {
    const int row = backends_.GetNextItem(-1, LVNI_SELECTED);
    if (!latest_ || row < 0 || row >= static_cast<int>(latest_->backends.size())) return nullptr;
    return &latest_->backends[static_cast<std::size_t>(row)];
}

std::vector<std::string> CMainDlg::group_names() const {
    std::vector<std::string> names;
    if (latest_ && latest_->config) {
        for (const auto& g : latest_->config->groups) names.push_back(g.name);
    }
    return names;
}

void CMainDlg::update_admin_buttons() {
    const bool available = latest_ && latest_->admin_available && latest_->config;
    const lb::BackendStats* b = available ? selected_backend() : nullptr;
    const bool in_service = b != nullptr && (b->state == lb::BackendState::Healthy || b->state == lb::BackendState::Unhealthy);
    GetDlgItem(IDC_ADD_BACKEND)->EnableWindow(available);
    GetDlgItem(IDC_ROUTING)->EnableWindow(available);
    GetDlgItem(IDC_EDIT_BACKEND)->EnableWindow(b != nullptr);
    GetDlgItem(IDC_REMOVE_BACKEND)->EnableWindow(b != nullptr);
    GetDlgItem(IDC_DRAIN_BACKEND)->EnableWindow(in_service);
    GetDlgItem(IDC_UNDRAIN_BACKEND)->EnableWindow(b != nullptr && !in_service);
    CString current;
    GetDlgItemTextW(IDC_ADMIN_NOTE, current);
    if (latest_ && !latest_->admin_available && current.IsEmpty()) {
        SetDlgItemTextW(IDC_ADMIN_NOTE, L"Admin edits need the config file (start with --config)");
    }
}

void CMainDlg::OnBackendSelected(NMHDR*, LRESULT* result) {
    update_admin_buttons();
    *result = 0;
}

void CMainDlg::note(const std::string& text) { SetDlgItemTextW(IDC_ADMIN_NOTE, L"Saved: " + app::from_utf8(text)); }

void CMainDlg::report(const lb::AdminResult& result) {
    if (result.ok) {
        note(result.summary);
    } else {
        AfxMessageBox(L"Not applied:\n\n" + app::from_utf8(result.error), MB_ICONWARNING | MB_OK);
    }
}

void CMainDlg::OnAddBackend() {
    if (!latest_ || !latest_->config) return;
    CBackendDlg dlg(this, group_names(), std::nullopt,
                    [this](const lb::BackendEdit& e) { return engine_.admin_add_backend(e); });
    if (dlg.DoModal() == IDOK) note(dlg.summary());
}

void CMainDlg::OnEditBackend() {
    const lb::BackendStats* b = selected_backend();
    if (b == nullptr || !latest_->config) return;
    std::optional<lb::BackendEdit> existing;
    for (const auto& g : latest_->config->groups) {
        for (const auto& c : g.backends) {
            if (c.id == b->id) existing = lb::BackendEdit{g.name, c.id, c.address, c.port, c.weight};
        }
    }
    if (!existing) return;
    CBackendDlg dlg(this, group_names(), existing,
                    [this](const lb::BackendEdit& e) { return engine_.admin_update_backend(e); });
    if (dlg.DoModal() == IDOK) note(dlg.summary());
}

void CMainDlg::OnRemoveBackend() {
    const lb::BackendStats* b = selected_backend();
    if (b == nullptr) return;
    const std::string id = b->id;
    const CString question = L"Remove backend " + app::from_utf8(id) +
                             L"?\n\nIt gets no new requests at once; requests already in flight finish. "
                             L"The change is saved to the config file.";
    if (AfxMessageBox(question, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    CWaitCursor wait;
    report(engine_.admin_remove_backend(id));
}

void CMainDlg::OnDrainBackend() {
    const lb::BackendStats* b = selected_backend();
    if (b == nullptr) return;
    CWaitCursor wait;
    report(engine_.admin_drain_backend(b->id));
}

void CMainDlg::OnUndrainBackend() {
    const lb::BackendStats* b = selected_backend();
    if (b == nullptr) return;
    CWaitCursor wait;
    report(engine_.admin_undrain_backend(b->id));
}

void CMainDlg::OnRoutingRules() {
    if (!latest_ || !latest_->config) return;
    CRoutingDlg dlg(this, group_names(), latest_->config->routing,
                    [this](const lb::RoutingConfig& r) { return engine_.admin_set_routing(r); });
    if (dlg.DoModal() == IDOK) note(dlg.summary());
}

// ---- Rows, shutdown -----------------------------------------------------------------------

void CMainDlg::OnBackendsCustomDraw(NMHDR* header, LRESULT* result) {
    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
    *result = CDRF_DODEFAULT;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
        *result = CDRF_NOTIFYITEMDRAW;
    } else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const std::size_t row = cd->nmcd.dwItemSpec;
        if (row < row_states_.size()) {
            if (row_states_[row] == lb::BackendState::Unhealthy) cd->clrText = kUnhealthyColor;
            else if (row_states_[row] == lb::BackendState::Draining || row_states_[row] == lb::BackendState::Drained) {
                cd->clrText = kDrainingColor;
            }
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
