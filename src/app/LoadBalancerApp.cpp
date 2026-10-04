#include "LoadBalancerApp.h"

#include <shellapi.h>

#include <filesystem>
#include <string>

#include "MainDlg.h"
#include "Text.h"
#include "config/config_loader.h"
#include "engine.h"

// Common Controls v6 for the list controls.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

CLoadBalancerApp theApp;

namespace {

struct Options {
    CString config_path;
    bool minimized = false;
};

Options parse_command_line() {
    Options o;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    for (int i = 1; argv != nullptr && i < argc; ++i) {
        const CString arg = argv[i];
        if (arg == L"--config" && i + 1 < argc) o.config_path = argv[++i];
        else if (arg == L"--minimized") o.minimized = true;
    }
    ::LocalFree(argv);
    return o;
}

}  // namespace

BOOL CLoadBalancerApp::InitInstance() {
    CWinApp::InitInstance();
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    ::InitCommonControlsEx(&icc);

    Options options = parse_command_line();
    if (options.config_path.IsEmpty()) {
        CFileDialog pick(TRUE, L"json", nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                         L"Load balancer configuration (*.json)|*.json|All files (*.*)|*.*||");
        pick.m_ofn.lpstrTitle = L"Open load balancer configuration";
        if (pick.DoModal() != IDOK) {
            exit_code_ = 2;
            return FALSE;
        }
        options.config_path = pick.GetPathName();
    }

    const auto loaded = lb::load_config_file(std::filesystem::path(std::wstring(options.config_path)));
    if (!loaded.ok()) {
        CString message = L"The configuration was rejected; nothing was started.\n\n" + options.config_path + L"\n\n";
        for (const auto& e : loaded.errors) message += app::from_utf8(lb::to_string(e)) + L"\n";
        AfxMessageBox(message, MB_ICONERROR | MB_OK);
        exit_code_ = 1;
        return FALSE;
    }

    lb::Engine engine(loaded.snapshot);
    CMainDlg dlg(engine, *loaded.snapshot, options.minimized);
    m_pMainWnd = &dlg;
    dlg.DoModal();
    exit_code_ = dlg.exit_code();
    m_pMainWnd = nullptr;
    // The engine stopped when the dialog closed; it is destroyed here, on the UI thread.
    return FALSE;
}

int CLoadBalancerApp::ExitInstance() {
    CWinApp::ExitInstance();
    // MFC would otherwise return the last message's wParam.
    return exit_code_;
}
