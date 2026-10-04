#pragma once

#include <atomic>
#include <memory>

#include "config/config.h"

namespace lb {

// Holds the active configuration snapshot (plan II.7, V). Each request calls
// current() once when it starts and keeps that pointer for its whole lifetime, so a
// later publish() never changes the config a request is already using.
//
// Note: MSVC implements std::atomic<std::shared_ptr> with an internal spin bit rather
// than lock-free; readers hold it only for the duration of a pointer copy.
class ConfigStore {
public:
    explicit ConfigStore(std::shared_ptr<const ConfigSnapshot> initial) noexcept
        : current_(std::move(initial)) {}

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    std::shared_ptr<const ConfigSnapshot> current() const noexcept {
        return current_.load(std::memory_order_acquire);
    }

    // Swaps in a fully validated snapshot. Used by hot reload (step 2.1).
    void publish(std::shared_ptr<const ConfigSnapshot> next) noexcept {
        current_.store(std::move(next), std::memory_order_release);
    }

private:
    std::atomic<std::shared_ptr<const ConfigSnapshot>> current_;
};

}  // namespace lb
