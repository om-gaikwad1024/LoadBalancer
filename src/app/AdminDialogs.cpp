#include "AdminDialogs.h"

#include <cwchar>

#include "Text.h"

namespace {

constexpr lb::RouteRule::Type kRuleTypes[] = {lb::RouteRule::Type::PathPrefix, lb::RouteRule::Type::PathGlob,
                                              lb::RouteRule::Type::Header, lb::RouteRule::Type::Cookie};

void fill_groups(CComboBox& combo, const std::vector<std::string>& groups, const std::string& selected) {
    combo.ResetContent();
    int pick = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        combo.AddString(app::from_utf8(groups[i]));
        if (groups[i] == selected) pick = static_cast<int>(i);
    }
    combo.SetCurSel(groups.empty() ? -1 : pick);
}

CString combo_text(CComboBox& combo) {
    CString text;
    if (combo.GetCurSel() >= 0) combo.GetLBText(combo.GetCurSel(), text);
    return text;
}

// A whole decimal number in [min, max], or nothing.
std::optional<unsigned long> parse_number(const CString& text, unsigned long min, unsigned long max) {
    if (text.IsEmpty()) return std::nullopt;
    wchar_t* end = nullptr;
    const unsigned long v = std::wcstoul(text.GetString(), &end, 10);
    if (end == nullptr || *end != L'\0' || v < min || v > max) return std::nullopt;
    return v;
}

}  // namespace

const wchar_t* rule_type_name(lb::RouteRule::Type type) {
    switch (type) {
        case lb::RouteRule::Type::PathPrefix: return L"path_prefix";
        case lb::RouteRule::Type::PathGlob: return L"path_glob";
        case lb::RouteRule::Type::Header: return L"header";
        case lb::RouteRule::Type::Cookie: return L"cookie";
    }
    return L"path_prefix";
}

// ---- Backend ---------------------------------------------------------------------------

CBackendDlg::CBackendDlg(CWnd* parent, std::vector<std::string> groups, std::optional<lb::BackendEdit> existing,
                         Apply apply)
    : CDialogEx(IDD_BACKEND, parent), groups_(std::move(groups)), existing_(std::move(existing)), apply_(std::move(apply)) {}

BOOL CBackendDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    auto* group = static_cast<CComboBox*>(GetDlgItem(IDC_BK_GROUP));
    fill_groups(*group, groups_, existing_ ? existing_->group : std::string());
    if (existing_) {
        SetWindowTextW(L"Edit backend " + app::from_utf8(existing_->id));
        SetDlgItemTextW(IDC_BK_ID, app::from_utf8(existing_->id));
        SetDlgItemTextW(IDC_BK_ADDRESS, app::from_utf8(existing_->address));
        SetDlgItemInt(IDC_BK_PORT, existing_->port, FALSE);
        SetDlgItemInt(IDC_BK_WEIGHT, existing_->weight, FALSE);
        GetDlgItem(IDC_BK_ID)->EnableWindow(FALSE);  // the identity of live state (plan IV.4)
        group->EnableWindow(FALSE);
    } else {
        SetWindowTextW(L"Add backend");
        SetDlgItemTextW(IDC_BK_ADDRESS, L"127.0.0.1");
        SetDlgItemInt(IDC_BK_WEIGHT, 1, FALSE);
    }
    return TRUE;
}

void CBackendDlg::OnOK() {
    CString id;
    CString address;
    CString port_text;
    CString weight_text;
    GetDlgItemTextW(IDC_BK_ID, id);
    GetDlgItemTextW(IDC_BK_ADDRESS, address);
    GetDlgItemTextW(IDC_BK_PORT, port_text);
    GetDlgItemTextW(IDC_BK_WEIGHT, weight_text);
    const auto port = parse_number(port_text, 0, 65535);
    const auto weight = parse_number(weight_text, 0, 1'000'000);
    if (!port || !weight) {
        SetDlgItemTextW(IDC_BK_ERROR, L"Port and weight must be whole numbers.");
        return;
    }
    lb::BackendEdit edit;
    edit.group = app::to_utf8(combo_text(*static_cast<CComboBox*>(GetDlgItem(IDC_BK_GROUP))));
    edit.id = app::to_utf8(id.Trim());
    edit.address = app::to_utf8(address.Trim());
    edit.port = static_cast<std::uint16_t>(*port);
    edit.weight = static_cast<std::uint32_t>(*weight);
    CWaitCursor wait;
    const lb::AdminResult r = apply_(edit);
    if (!r.ok) {
        SetDlgItemTextW(IDC_BK_ERROR, L"Not applied: " + app::from_utf8(r.error));
        return;
    }
    summary_ = r.summary;
    CDialogEx::OnOK();
}

// ---- One rule --------------------------------------------------------------------------

BEGIN_MESSAGE_MAP(CRuleDlg, CDialogEx)
    ON_CBN_SELCHANGE(IDC_RU_TYPE, &CRuleDlg::OnTypeChanged)
    ON_BN_CLICKED(IDC_RU_ANY, &CRuleDlg::OnAnyClicked)
END_MESSAGE_MAP()

CRuleDlg::CRuleDlg(CWnd* parent, std::vector<std::string> groups, lb::RouteRule rule)
    : CDialogEx(IDD_RULE, parent), groups_(std::move(groups)), rule_(std::move(rule)) {}

BOOL CRuleDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    auto* type = static_cast<CComboBox*>(GetDlgItem(IDC_RU_TYPE));
    for (const auto t : kRuleTypes) {
        const int i = type->AddString(rule_type_name(t));
        if (t == rule_.type) type->SetCurSel(i);
    }
    fill_groups(*static_cast<CComboBox*>(GetDlgItem(IDC_RU_GROUP)), groups_, rule_.group);
    SetDlgItemTextW(IDC_RU_ID, app::from_utf8(rule_.id));
    SetDlgItemTextW(IDC_RU_FIELD, app::from_utf8(rule_.field));
    SetDlgItemTextW(IDC_RU_VALUE, rule_.value ? app::from_utf8(*rule_.value) : CString());
    CheckDlgButton(IDC_RU_ANY, rule_.value || rule_.id.empty() ? BST_UNCHECKED : BST_CHECKED);
    update_enabled();
    return TRUE;
}

void CRuleDlg::update_enabled() {
    const int t = static_cast<CComboBox*>(GetDlgItem(IDC_RU_TYPE))->GetCurSel();
    const bool path_rule = t <= 1;  // path_prefix, path_glob
    GetDlgItem(IDC_RU_FIELD)->EnableWindow(!path_rule);
    GetDlgItem(IDC_RU_ANY)->EnableWindow(!path_rule);
    if (path_rule) CheckDlgButton(IDC_RU_ANY, BST_UNCHECKED);
    GetDlgItem(IDC_RU_VALUE)->EnableWindow(IsDlgButtonChecked(IDC_RU_ANY) != BST_CHECKED);
}

void CRuleDlg::OnTypeChanged() { update_enabled(); }
void CRuleDlg::OnAnyClicked() { update_enabled(); }

void CRuleDlg::OnOK() {
    CString id;
    CString field;
    CString value;
    GetDlgItemTextW(IDC_RU_ID, id);
    GetDlgItemTextW(IDC_RU_FIELD, field);
    GetDlgItemTextW(IDC_RU_VALUE, value);
    const int t = static_cast<CComboBox*>(GetDlgItem(IDC_RU_TYPE))->GetCurSel();
    rule_.id = app::to_utf8(id.Trim());
    rule_.type = kRuleTypes[t < 0 ? 0 : t];
    const bool path_rule = t <= 1;
    rule_.field = path_rule ? std::string() : app::to_utf8(field.Trim());
    if (!path_rule && IsDlgButtonChecked(IDC_RU_ANY) == BST_CHECKED) rule_.value.reset();
    else rule_.value = app::to_utf8(value);
    rule_.group = app::to_utf8(combo_text(*static_cast<CComboBox*>(GetDlgItem(IDC_RU_GROUP))));
    CDialogEx::OnOK();  // the whole rule set is validated when the routing dialog saves
}

// ---- Routing ---------------------------------------------------------------------------

BEGIN_MESSAGE_MAP(CRoutingDlg, CDialogEx)
    ON_BN_CLICKED(IDC_RT_ADD, &CRoutingDlg::OnAdd)
    ON_BN_CLICKED(IDC_RT_EDIT, &CRoutingDlg::OnEdit)
    ON_BN_CLICKED(IDC_RT_REMOVE, &CRoutingDlg::OnRemove)
    ON_BN_CLICKED(IDC_RT_UP, &CRoutingDlg::OnUp)
    ON_BN_CLICKED(IDC_RT_DOWN, &CRoutingDlg::OnDown)
    ON_NOTIFY(NM_DBLCLK, IDC_RT_RULES, &CRoutingDlg::OnRulesDoubleClick)
END_MESSAGE_MAP()

CRoutingDlg::CRoutingDlg(CWnd* parent, std::vector<std::string> groups, lb::RoutingConfig routing, Apply apply)
    : CDialogEx(IDD_ROUTING, parent), groups_(std::move(groups)), routing_(std::move(routing)), apply_(std::move(apply)) {}

void CRoutingDlg::DoDataExchange(CDataExchange* dx) {
    CDialogEx::DoDataExchange(dx);
    DDX_Control(dx, IDC_RT_RULES, rules_);
}

BOOL CRoutingDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    fill_groups(*static_cast<CComboBox*>(GetDlgItem(IDC_RT_DEFAULT)), groups_, routing_.default_group);
    rules_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const struct {
        const wchar_t* title;
        int width;
    } columns[] = {{L"Id", 80}, {L"Match", 76}, {L"Header / cookie", 104}, {L"Value / path", 124}, {L"Group", 76}};
    int i = 0;
    for (const auto& c : columns) {
        rules_.InsertColumn(i++, c.title, LVCFMT_LEFT, ::MulDiv(c.width, static_cast<int>(::GetDpiForWindow(m_hWnd)), 96));
    }
    render(-1);
    return TRUE;
}

void CRoutingDlg::render(int select) {
    rules_.DeleteAllItems();
    for (std::size_t r = 0; r < routing_.rules.size(); ++r) {
        const auto& rule = routing_.rules[r];
        const int row = static_cast<int>(r);
        rules_.InsertItem(row, app::from_utf8(rule.id));
        rules_.SetItemText(row, 1, rule_type_name(rule.type));
        rules_.SetItemText(row, 2, app::from_utf8(rule.field));
        rules_.SetItemText(row, 3, rule.value ? app::from_utf8(*rule.value) : CString(L"(present)"));
        rules_.SetItemText(row, 4, app::from_utf8(rule.group));
    }
    if (select >= 0 && select < rules_.GetItemCount()) {
        rules_.SetItemState(select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        rules_.EnsureVisible(select, FALSE);
    }
}

int CRoutingDlg::selected() const { return rules_.GetNextItem(-1, LVNI_SELECTED); }

void CRoutingDlg::OnAdd() {
    lb::RouteRule rule;
    rule.group = groups_.empty() ? std::string() : groups_.front();
    CRuleDlg dlg(this, groups_, rule);
    if (dlg.DoModal() != IDOK) return;
    routing_.rules.push_back(dlg.rule());
    render(static_cast<int>(routing_.rules.size()) - 1);
}

void CRoutingDlg::OnEdit() {
    const int i = selected();
    if (i < 0) return;
    CRuleDlg dlg(this, groups_, routing_.rules[static_cast<std::size_t>(i)]);
    if (dlg.DoModal() != IDOK) return;
    routing_.rules[static_cast<std::size_t>(i)] = dlg.rule();
    render(i);
}

void CRoutingDlg::OnRulesDoubleClick(NMHDR*, LRESULT* result) {
    OnEdit();
    *result = 0;
}

void CRoutingDlg::OnRemove() {
    const int i = selected();
    if (i < 0) return;
    routing_.rules.erase(routing_.rules.begin() + i);
    render(std::min(i, static_cast<int>(routing_.rules.size()) - 1));
}

void CRoutingDlg::OnUp() {
    const int i = selected();
    if (i <= 0) return;
    std::swap(routing_.rules[static_cast<std::size_t>(i)], routing_.rules[static_cast<std::size_t>(i - 1)]);
    render(i - 1);
}

void CRoutingDlg::OnDown() {
    const int i = selected();
    if (i < 0 || i + 1 >= static_cast<int>(routing_.rules.size())) return;
    std::swap(routing_.rules[static_cast<std::size_t>(i)], routing_.rules[static_cast<std::size_t>(i + 1)]);
    render(i + 1);
}

void CRoutingDlg::OnOK() {
    routing_.default_group = app::to_utf8(combo_text(*static_cast<CComboBox*>(GetDlgItem(IDC_RT_DEFAULT))));
    CWaitCursor wait;
    const lb::AdminResult r = apply_(routing_);
    if (!r.ok) {
        SetDlgItemTextW(IDC_RT_ERROR, L"Not saved: " + app::from_utf8(r.error));
        return;
    }
    summary_ = r.summary;
    CDialogEx::OnOK();
}
