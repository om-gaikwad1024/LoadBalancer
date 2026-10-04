#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace lb {

// Watches one config file for changes (plan IV.14) on its own thread with
// ReadDirectoryChangesW on the file's directory, so saves that replace the file (write a
// temp file, then rename it over) are seen too. Editors often save in several writes, so
// `on_change` runs once the file has been quiet for `debounce`. It runs on the watcher
// thread; stop() waits for a running call to return.
class ConfigWatcher {
public:
    ConfigWatcher() = default;
    ~ConfigWatcher() { stop(); }
    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    bool start(const std::filesystem::path& file, std::chrono::milliseconds debounce, std::function<void()> on_change,
               std::string* error);
    void stop();

private:
    void run();
    bool arm();  // queues the next overlapped ReadDirectoryChangesW
    bool names_our_file(DWORD bytes) const;

    std::wstring directory_;
    std::wstring file_name_;
    std::chrono::milliseconds debounce_{};
    std::function<void()> on_change_;
    HANDLE dir_ = INVALID_HANDLE_VALUE;
    HANDLE changed_ = nullptr;  // overlapped completion event
    HANDLE stop_ = nullptr;
    OVERLAPPED overlapped_{};
    alignas(DWORD) unsigned char buffer_[16 * 1024]{};
    std::thread thread_;
};

}  // namespace lb
