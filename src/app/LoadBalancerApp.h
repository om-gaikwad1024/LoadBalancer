#pragma once

#include "framework.h"

// One MFC process hosts both the proxy engine and the operator dashboard (plan II.1).
//   LoadBalancer.exe --config <file.json> [--minimized]
// Without --config, a file dialog asks for the configuration.
class CLoadBalancerApp : public CWinApp {
public:
    BOOL InitInstance() override;
    int ExitInstance() override;

private:
    int exit_code_ = 0;
};
