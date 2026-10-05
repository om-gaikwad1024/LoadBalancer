#pragma once

// Plain #defines only: the dashboard integration test includes this header too.

#define IDD_MAIN 101
#define IDD_BACKEND 102
#define IDD_ROUTING 103
#define IDD_RULE 104

// Main dashboard.
#define IDC_STATUS 1001
#define IDC_P99_LABEL 1002
#define IDC_MAX_LABEL 1003
#define IDC_LATENCY_DETAIL 1004
#define IDC_BACKENDS_CAPTION 1005
#define IDC_BACKENDS 1006
#define IDC_EVENTS_CAPTION 1007
#define IDC_EVENTS 1008
#define IDC_GRAPH_RATE 1009     // created at run time (CGraphCtrl)
#define IDC_GRAPH_LATENCY 1010  // created at run time (CGraphCtrl)
#define IDC_ADD_BACKEND 1011
#define IDC_EDIT_BACKEND 1012
#define IDC_REMOVE_BACKEND 1013
#define IDC_DRAIN_BACKEND 1014
#define IDC_UNDRAIN_BACKEND 1015
#define IDC_ROUTING 1016
#define IDC_EVENT_TYPE 1017
#define IDC_EVENT_BACKEND 1018
#define IDC_EVENT_SEARCH 1019
#define IDC_EVENT_PROBLEMS 1020
#define IDC_EVENT_COUNT 1021
#define IDC_ADMIN_NOTE 1022

// Add / edit backend.
#define IDC_BK_GROUP 1101
#define IDC_BK_ID 1102
#define IDC_BK_ADDRESS 1103
#define IDC_BK_PORT 1104
#define IDC_BK_WEIGHT 1105
#define IDC_BK_ERROR 1106

// Routing rules.
#define IDC_RT_DEFAULT 1201
#define IDC_RT_RULES 1202
#define IDC_RT_ADD 1203
#define IDC_RT_EDIT 1204
#define IDC_RT_REMOVE 1205
#define IDC_RT_UP 1206
#define IDC_RT_DOWN 1207
#define IDC_RT_ERROR 1208

// One routing rule.
#define IDC_RU_ID 1301
#define IDC_RU_TYPE 1302
#define IDC_RU_FIELD 1303
#define IDC_RU_VALUE 1304
#define IDC_RU_ANY 1305
#define IDC_RU_GROUP 1306
