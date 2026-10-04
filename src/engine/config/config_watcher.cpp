#include "config/config_watcher.h"

#include "core/clock.h"

namespace lb {

namespace {

std::string utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string last_error_text(const char* what) {
    return std::string(what) + " failed (Windows error " + std::to_string(::GetLastError()) + ")";
}

}  // namespace

bool ConfigWatcher::start(const std::filesystem::path& file, std::chrono::milliseconds debounce,
                          std::function<void()> on_change, std::string* error) {
    if (dir_ != INVALID_HANDLE_VALUE) {
        *error = "already watching a config file";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path full = std::filesystem::absolute(file, ec);
    if (ec) {
        *error = "cannot resolve config path " + utf8(file) + ": " + ec.message();
        return false;
    }
    directory_ = full.parent_path().wstring();
    file_name_ = full.filename().wstring();
    debounce_ = debounce;
    on_change_ = std::move(on_change);

    dir_ = ::CreateFileW(directory_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir_ == INVALID_HANDLE_VALUE) {
        *error = "cannot watch " + utf8(full.parent_path()) + ": " + last_error_text("CreateFileW");
        return false;
    }
    changed_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stop_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (changed_ == nullptr || stop_ == nullptr || !arm()) {
        *error = "cannot watch " + utf8(full) + ": " + last_error_text("ReadDirectoryChangesW");
        stop();
        return false;
    }
    thread_ = std::thread([this] {
        ::SetThreadDescription(::GetCurrentThread(), L"lb-config-watcher");
        run();
    });
    return true;
}

void ConfigWatcher::stop() {
    if (stop_ != nullptr) ::SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    if (dir_ != INVALID_HANDLE_VALUE) {
        // Cancel the outstanding read and wait for it, so the kernel no longer writes buffer_.
        if (::CancelIoEx(dir_, &overlapped_) || ::GetLastError() != ERROR_NOT_FOUND) {
            DWORD ignored = 0;
            ::GetOverlappedResult(dir_, &overlapped_, &ignored, TRUE);
        }
        ::CloseHandle(dir_);
        dir_ = INVALID_HANDLE_VALUE;
    }
    if (changed_ != nullptr) ::CloseHandle(changed_);
    if (stop_ != nullptr) ::CloseHandle(stop_);
    changed_ = stop_ = nullptr;
}

bool ConfigWatcher::arm() {
    ::ResetEvent(changed_);
    overlapped_ = {};
    overlapped_.hEvent = changed_;
    return ::ReadDirectoryChangesW(dir_, buffer_, sizeof(buffer_), FALSE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                       FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION,
                                   nullptr, &overlapped_, nullptr) != FALSE;
}

bool ConfigWatcher::names_our_file(DWORD bytes) const {
    if (bytes == 0) return true;  // the buffer overflowed: some changes were lost, so check anyway
    const unsigned char* p = buffer_;
    for (;;) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        const int len = static_cast<int>(info->FileNameLength / sizeof(WCHAR));
        if (info->Action != FILE_ACTION_REMOVED && info->Action != FILE_ACTION_RENAMED_OLD_NAME &&
            ::CompareStringOrdinal(info->FileName, len, file_name_.c_str(), static_cast<int>(file_name_.size()),
                                   TRUE) == CSTR_EQUAL) {
            return true;
        }
        if (info->NextEntryOffset == 0) return false;
        p += info->NextEntryOffset;
    }
}

void ConfigWatcher::run() {
    bool pending = false;     // a change is waiting for the file to go quiet
    TimePoint quiet_at{};     // when the debounce ends
    bool armed = true;
    for (;;) {
        DWORD timeout = INFINITE;
        if (pending) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(quiet_at - Clock::now()).count();
            timeout = left > 0 ? static_cast<DWORD>(left) : 0;
        }
        const HANDLE handles[2] = {stop_, changed_};
        const DWORD n = armed ? 2 : 1;
        const DWORD w = ::WaitForMultipleObjects(n, handles, FALSE, timeout);
        if (w == WAIT_OBJECT_0) return;
        if (w == WAIT_OBJECT_0 + 1) {
            DWORD bytes = 0;
            const bool ok = ::GetOverlappedResult(dir_, &overlapped_, &bytes, FALSE) != FALSE;
            if (!ok || names_our_file(bytes)) {
                pending = true;  // every further write restarts the quiet period
                quiet_at = Clock::now() + debounce_;
            }
            armed = arm();
            if (!armed && !pending) return;  // the directory went away: nothing more to watch
            continue;
        }
        if (w == WAIT_TIMEOUT && pending) {
            pending = false;
            on_change_();
            if (!armed) return;
            continue;
        }
        if (w == WAIT_FAILED) return;
    }
}

}  // namespace lb
