#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "mock_server.h"
#include "test_config.h"

namespace lbtest {

// Records trace events from IOCP workers (plan IV.18 step order).
class TraceCollector final : public lb::TraceSink {
public:
    void on_trace(const lb::TraceEvent& e) noexcept override {
        std::lock_guard lock(mutex_);
        events_.push_back(e);
    }

    std::vector<lb::TraceEvent> events() {
        std::lock_guard lock(mutex_);
        return events_;
    }

    std::vector<lb::TraceStep> steps_of_last_request() {
        std::lock_guard lock(mutex_);
        std::vector<lb::TraceStep> steps;
        if (events_.empty()) return steps;
        const auto id = events_.back().request_id;
        for (const auto& e : events_) {
            if (e.request_id == id) steps.push_back(e.step);
        }
        return steps;
    }

private:
    std::mutex mutex_;
    std::vector<lb::TraceEvent> events_;
};

class ProxyTest : public ::testing::Test {
protected:
    // In-process mock backend.
    std::uint16_t start_backend(const mock::MockFaults& faults = {}, const std::string& id = "b1") {
        mock::MockOptions o;
        o.id = id;
        o.faults = faults;
        backends_.push_back(std::make_unique<mock::MockServer>(std::move(o)));
        std::string error;
        EXPECT_TRUE(backends_.back()->start(&error)) << error;
        return backends_.back()->port();
    }

    mock::MockServer& backend(std::size_t i = 0) { return *backends_.at(i); }

    void start_proxy(const std::vector<std::uint16_t>& backend_ports,
                     const std::function<void(nlohmann::json&)>& tweak = {}) {
        engine_ = std::make_unique<lb::Engine>(make_proxy_config(backend_ports, tweak));
        engine_->set_trace_sink(&trace_);
        std::string error;
        ASSERT_TRUE(engine_->start(&error)) << error;
    }

    lb::Engine& engine() { return *engine_; }
    std::uint16_t proxy_port() const { return engine_->listen_port(); }
    TraceCollector& trace() { return trace_; }

    // Polls until `predicate` holds or the timeout passes (for counters updated on workers).
    static bool eventually(const std::function<bool()>& predicate,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return predicate();
    }

    void TearDown() override {
        if (engine_) engine_->stop();
        for (auto& b : backends_) b->stop();
    }

private:
    TraceCollector trace_;
    std::vector<std::unique_ptr<mock::MockServer>> backends_;
    std::unique_ptr<lb::Engine> engine_;
};

}  // namespace lbtest
