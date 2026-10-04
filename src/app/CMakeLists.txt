# MFC executable: shell, dashboard, admin console. The only place MFC types appear (plan II.1, II.6).
set(CMAKE_MFC_FLAG 2)  # shared MFC DLL (used by Visual Studio generators; Ninja relies on _AFXDLL below)

add_executable(LoadBalancer WIN32
  framework.h
  resource.h
  LoadBalancerApp.h
  LoadBalancerApp.cpp
  MainDlg.h
  MainDlg.cpp
  LoadBalancer.rc
)

target_link_libraries(LoadBalancer PRIVATE lb_engine)
lb_set_project_options(LoadBalancer)
target_compile_definitions(LoadBalancer PRIVATE _AFXDLL)
target_link_options(LoadBalancer PRIVATE /ENTRY:wWinMainCRTStartup)
