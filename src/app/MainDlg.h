#pragma once

#include "framework.h"
#include "resource.h"

class CMainDlg : public CDialogEx {
public:
    enum { IDD = IDD_MAIN };

    explicit CMainDlg(CWnd* parent = nullptr);

protected:
    BOOL OnInitDialog() override;

    DECLARE_MESSAGE_MAP()
};
