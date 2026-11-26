// tests/gitcore/crash_guard.cpp — no dialog, ever, from this harness.
//
// A claim that does not hold reports and keeps running; a broken invariant that
// takes the process down writes to stderr and exits. The Debug Assertion dialog,
// the abort dialog, and Windows Error Reporting would each raise a modal window
// on the operator's screen and block the run until someone clicked it away.
//
// This is its own translation unit because the guard needs windows.h: that
// header defines `small` as `char`, which poisons every later name in a unity
// translation unit (see the note in main.cpp).
//
// _CrtSetReportMode routes the CRT's own assert/error/warning reports to a file
// — stderr here — instead of the message box, so an assert inside a Debug CRT
// (a vector subscript out of range, among them) prints and dies.

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <stdlib.h>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace ols_test {

void install_crash_guard()
{
#ifdef _WIN32
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    _set_abort_behavior(0u, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#if defined(_MSC_VER)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
}

} // namespace ols_test
