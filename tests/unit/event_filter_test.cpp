// Step 2.7 (plan IV.17): the dashboard's searchable, filterable log view.

#include <gtest/gtest.h>

#include "log/event_filter.h"

using lb::LoggedEvent;
using lb::log::EventFilter;
using lb::log::Severity;
using lb::log::event_severity;
using lb::log::matches;

namespace {

LoggedEvent event(std::string type, std::string backend, std::string request_id, std::string message) {
    LoggedEvent e;
    e.type = std::move(type);
    e.backend = std::move(backend);
    e.request_id = std::move(request_id);
    e.message = std::move(message);
    return e;
}

}  // namespace

TEST(EventSeverity, ProblemsAreWarningsOrErrors) {
    EXPECT_EQ(event_severity("backend_marked_unhealthy"), Severity::Error);
    EXPECT_EQ(event_severity("no_backend_available"), Severity::Error);
    EXPECT_EQ(event_severity("drain_timed_out"), Severity::Error);
    EXPECT_EQ(event_severity("config_reload_rejected"), Severity::Warning);
    EXPECT_EQ(event_severity("sticky_reassigned"), Severity::Warning);
    EXPECT_EQ(event_severity("backend_marked_healthy"), Severity::Info);
    EXPECT_EQ(event_severity("config_reload_accepted"), Severity::Info);
    EXPECT_EQ(event_severity("request_step"), Severity::Info);
    EXPECT_EQ(event_severity("something_new"), Severity::Info);
}

TEST(EventFilter, EmptyFilterMatchesEverything) {
    EXPECT_TRUE(matches(event("engine_started", "", "", "listening"), EventFilter{}));
}

TEST(EventFilter, TypeAndBackendAreExact) {
    const auto e = event("backend_error", "web-1", "abc123", "web-1 failed: connect refused -> 502");
    EXPECT_TRUE(matches(e, {"backend_error", "", "", false}));
    EXPECT_FALSE(matches(e, {"backend_err", "", "", false}));
    EXPECT_TRUE(matches(e, {"", "web-1", "", false}));
    EXPECT_FALSE(matches(e, {"", "web-10", "", false}));
    EXPECT_FALSE(matches(e, {"", "web", "", false}));
}

TEST(EventFilter, TextSearchesMessageTypeBackendAndRequestIdIgnoringCase) {
    const auto e = event("backend_error", "web-1", "0f1e2d3c", "web-1 failed: Connect Refused -> 502");
    EXPECT_TRUE(matches(e, {"", "", "connect refused", false}));
    EXPECT_TRUE(matches(e, {"", "", "BACKEND_ERR", false}));
    EXPECT_TRUE(matches(e, {"", "", "0F1E", false}));  // a request id, to follow one request
    EXPECT_TRUE(matches(e, {"", "", "WEB-1", false}));
    EXPECT_FALSE(matches(e, {"", "", "timeout", false}));
}

TEST(EventFilter, ProblemsOnlyHidesInformation) {
    EventFilter problems;
    problems.problems_only = true;
    EXPECT_FALSE(matches(event("backend_marked_healthy", "b1", "", "b1 marked healthy"), problems));
    EXPECT_TRUE(matches(event("backend_marked_unhealthy", "b1", "", "b1 marked unhealthy"), problems));
    EXPECT_TRUE(matches(event("config_reload_rejected", "", "", "rejected"), problems));
}

TEST(EventFilter, AllConditionsMustHold) {
    const auto e = event("backend_marked_unhealthy", "b2", "", "b2 marked unhealthy after 3 failed probes");
    EXPECT_TRUE(matches(e, {"backend_marked_unhealthy", "b2", "probes", true}));
    EXPECT_FALSE(matches(e, {"backend_marked_unhealthy", "b1", "probes", true}));
    EXPECT_FALSE(matches(e, {"backend_marked_unhealthy", "b2", "requests", true}));
}
