#pragma once

#include "framework.h"
#include "resource.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "engine.h"

// Admin console dialogs (plan IV.17). They only collect what the operator typed: every
// change is validated by the engine's config path (plan IV.14) when OK is pressed, and an
// error keeps the dialog open with the message, so nothing is lost.

class CBackendDlg : public CDialogEx {
public:
    using Apply = std::function<lb::AdminResult(const lb::BackendEdit&)>;

    // `existing` set: edit that backend (its id and group are fixed). Otherwise add one.
    CBackendDlg(CWnd* parent, std::vector<std::string> groups, std::optional<lb::BackendEdit> existing, Apply apply);

    const std::string& summary() const noexcept { return summary_; }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;

private:
    std::vector<std::string> groups_;
    std::optional<lb::BackendEdit> existing_;
    Apply apply_;
    std::string summary_;
};

class CRuleDlg : public CDialogEx {
public:
    CRuleDlg(CWnd* parent, std::vector<std::string> groups, lb::RouteRule rule);
    const lb::RouteRule& rule() const noexcept { return rule_; }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnTypeChanged();
    afx_msg void OnAnyClicked();
    DECLARE_MESSAGE_MAP()

private:
    void update_enabled();

    std::vector<std::string> groups_;
    lb::RouteRule rule_;
};

class CRoutingDlg : public CDialogEx {
public:
    using Apply = std::function<lb::AdminResult(const lb::RoutingConfig&)>;

    CRoutingDlg(CWnd* parent, std::vector<std::string> groups, lb::RoutingConfig routing, Apply apply);
    const std::string& summary() const noexcept { return summary_; }

protected:
    void DoDataExchange(CDataExchange* dx) override;
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnAdd();
    afx_msg void OnEdit();
    afx_msg void OnRemove();
    afx_msg void OnUp();
    afx_msg void OnDown();
    afx_msg void OnRulesDoubleClick(NMHDR* header, LRESULT* result);
    DECLARE_MESSAGE_MAP()

private:
    void render(int select);
    int selected() const;

    std::vector<std::string> groups_;
    lb::RoutingConfig routing_;
    Apply apply_;
    std::string summary_;
    CListCtrl rules_;
};

// Display names of rule types, as in the config file.
const wchar_t* rule_type_name(lb::RouteRule::Type type);
