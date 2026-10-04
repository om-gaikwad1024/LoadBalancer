#pragma once

#include "framework.h"

class CLoadBalancerApp : public CWinApp {
public:
    BOOL InitInstance() override;
    int ExitInstance() override;
};
