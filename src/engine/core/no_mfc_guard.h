#pragma once

// Force-included into every engine translation unit (see src/engine/CMakeLists.txt).
// The engine uses standard C++ containers only and must build and test without MFC
// (plan II.6). An MFC header included later in a /MD build without _AFXDLL also
// fails inside MFC itself, so the two checks together keep MFC out of the engine.
#if defined(_AFXDLL) || defined(_AFX) || defined(__AFXWIN_H__)
#error "MFC must not be used in src/engine (plan II.6)."
#endif
