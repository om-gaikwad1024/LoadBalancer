#pragma once

#include "framework.h"
#include "resource.h"

#include <filesystem>
#include <memory>
#include <vector>

#include "engine.h"

// Carries a snapshot from the engine's publisher thread to the UI thread (plan IV.17, V):
// it only posts a message to the dialog's HWND. No UI object is touched off the UI thread.
class SnapshotBridge final : public lb::SnapshotSink {
public:
    static constexpr UINT kMessage = WM_APP + 1;  // LPARAM: lb::DashboardSnapshot*, owned by the receiver

    void attach(HWND target) noexcept { target_ = target; }
    void on_snapshot(std::unique_ptr<lb::DashboardSnapshot> snapshot) noexcept override;

private:
    HWND target_ = nullptr;
};

// Operator dashboard, phase 1 (plan IV.17): backend list with state, in-flight requests and
// weight; latency percentiles with the tail (p99, max) shown prominently; a live event list.
// It renders only copied snapshots, on a UI timer, and never reads engine state directly.
class CMainDlg : public CDialogEx {
public:
    enum { IDD = IDD_MAIN };

    CMainDlg(lb::Engine& engine, const lb::ConfigSnapshot& config, std::filesystem::path config_path,
             bool start_minimized, CWnd* parent = nullptr);

    int exit_code() const noexcept { return exit_code_; }

protected:
    void DoDataExchange(CDataExchange* dx) override;
    BOOL OnInitDialog() override;
    void OnOK() override {}      // Enter must not close the proxy
    void OnCancel() override {}  // nor Esc; only the window's close button / WM_CLOSE

    afx_msg void OnClose();
    afx_msg void OnDestroy();
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnSize(UINT type, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info);
    afx_msg LRESULT OnSnapshot(WPARAM, LPARAM snapshot);
    afx_msg void OnBackendsCustomDraw(NMHDR* header, LRESULT* result);
    DECLARE_MESSAGE_MAP()

private:
    void setup_lists();
    void layout(int cx, int cy);
    void render();
    void render_status(const lb::DashboardSnapshot& s);
    void render_latency(const lb::DashboardSnapshot& s);
    void render_backends(const lb::DashboardSnapshot& s);
    void render_events();
    void stop_engine();
    int scale(int pixels) const;

    lb::Engine& engine_;
    const std::filesystem::path config_path_;  // watched for hot reload (plan IV.14)
    const std::uint32_t refresh_ms_;
    const std::uint32_t event_rows_;
    const bool start_minimized_;
    bool engine_running_ = false;
    int exit_code_ = 0;

    SnapshotBridge bridge_;
    std::unique_ptr<lb::DashboardSnapshot> latest_;  // UI thread only
    std::vector<lb::LoggedEvent> pending_events_;     // received but not yet shown
    std::uint64_t rendered_sequence_ = 0;
    std::vector<lb::BackendState> row_states_;        // for row colors

    CListCtrl backends_;
    CListCtrl events_;
    CFont tail_font_;
};
