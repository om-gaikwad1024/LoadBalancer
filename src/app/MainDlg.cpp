#include "MainDlg.h"

#include "engine.h"

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
END_MESSAGE_MAP()

CMainDlg::CMainDlg(CWnd* parent) : CDialogEx(IDD_MAIN, parent) {}

BOOL CMainDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();

    const auto version = lb::engine_version();
    const CStringA versionA(version.data(), static_cast<int>(version.size()));
    SetDlgItemText(IDC_ENGINE_VERSION, CString(versionA));

    return TRUE;
}
