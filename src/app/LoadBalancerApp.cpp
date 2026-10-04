#include "LoadBalancerApp.h"

#include "MainDlg.h"

// Common Controls v6 for the list and graph controls the dashboard adds later.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

CLoadBalancerApp theApp;

BOOL CLoadBalancerApp::InitInstance() {
    CWinApp::InitInstance();

    CMainDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();

    // Dialog-based app: returning FALSE ends the message pump and exits.
    return FALSE;
}

int CLoadBalancerApp::ExitInstance() {
    CWinApp::ExitInstance();
    // MFC would otherwise return the last message's wParam (the dialog's IDCANCEL).
    return 0;
}
