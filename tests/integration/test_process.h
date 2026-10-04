#pragma once

#include <windows.h>

#include <chrono>
#include <string>
#include <thread>

namespace lbtest {

// Child process with a captured stdout, for tests that run real executables and kill
// them (plan IX "scripted process kills").
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess() { kill(); }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::wstring& exe, const std::wstring& args) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE write_end = nullptr;
        if (!::CreatePipe(&stdout_read_, &write_end, &sa, 0)) return false;
        ::SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_end;
        si.hStdError = write_end;
        si.hStdInput = nullptr;

        std::wstring command = L"\"" + exe + L"\" " + args;
        const BOOL ok = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                         nullptr, &si, &info_);
        ::CloseHandle(write_end);
        return ok != FALSE;
    }

    // Reads stdout until a full line arrives or the timeout passes.
    bool read_line(std::string* line, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto eol = output_.find('\n');
            if (eol != std::string::npos) {
                *line = output_.substr(0, eol);
                output_.erase(0, eol + 1);
                return true;
            }
            DWORD available = 0;
            if (!::PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            char buf[1024];
            DWORD n = 0;
            if (!::ReadFile(stdout_read_, buf, sizeof(buf) < available ? sizeof(buf) : available, &n, nullptr)) return false;
            output_.append(buf, n);
        }
        return false;
    }

    // Hard kill, like a crashed backend: no graceful shutdown of its sockets.
    void kill() {
        if (info_.hProcess != nullptr) {
            ::TerminateProcess(info_.hProcess, 1);
            ::WaitForSingleObject(info_.hProcess, 5000);
            ::CloseHandle(info_.hProcess);
            ::CloseHandle(info_.hThread);
            info_ = {};
        }
        if (stdout_read_ != nullptr) {
            ::CloseHandle(stdout_read_);
            stdout_read_ = nullptr;
        }
    }

private:
    PROCESS_INFORMATION info_{};
    HANDLE stdout_read_ = nullptr;
    std::string output_;
};

}  // namespace lbtest
