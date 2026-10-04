#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "log/event_log.h"

using lb::log::EventLog;
namespace fs = std::filesystem;

namespace {

lb::EventLogConfig config(const std::string& path, std::uint32_t max_bytes = 1024 * 1024, std::uint32_t files = 3) {
    lb::EventLogConfig c;
    c.path = path;
    c.max_file_bytes = max_bytes;
    c.max_files = files;
    c.max_queue = 100000;
    c.recent_events = 100;
    c.trace_requests = false;
    return c;
}

// A fresh directory per test under the system temp folder.
fs::path temp_dir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("lb-event-log-test-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::vector<nlohmann::json> read_lines(const fs::path& file) {
    std::vector<nlohmann::json> out;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        out.push_back(nlohmann::json::parse(line));  // throws (fails the test) on a malformed line
    }
    return out;
}

}  // namespace

TEST(EventLog, EntriesAreStructuredJsonWithBothClocks) {
    EventLog log(config(""), lb::Clock::now());
    std::string error;
    ASSERT_TRUE(log.start(&error)) << error;
    log.emit("backend_marked_unhealthy", "b1 marked unhealthy", {{"failures", 3}, {"reason", "refused"}}, "b1", "req-1");
    log.emit("engine_stopped", "stopped");

    const auto events = log.recent();
    ASSERT_EQ(events.size(), 2u);
    const auto& e = events[0];
    EXPECT_EQ(e.type, "backend_marked_unhealthy");
    EXPECT_EQ(e.backend, "b1");
    EXPECT_EQ(e.request_id, "req-1");
    EXPECT_EQ(e.message, "b1 marked unhealthy");
    EXPECT_EQ(e.wall_time.size(), 24u);  // 2026-10-04T12:34:56.789Z
    EXPECT_EQ(e.wall_time.back(), 'Z');
    EXPECT_LT(e.sequence, events[1].sequence);
    EXPECT_LE(e.mono_ms, events[1].mono_ms);

    const auto j = nlohmann::json::parse(e.json);
    EXPECT_EQ(j["event"], "backend_marked_unhealthy");
    EXPECT_EQ(j["backend"], "b1");
    EXPECT_EQ(j["request_id"], "req-1");
    EXPECT_EQ(j["failures"], 3);
    EXPECT_EQ(j["reason"], "refused");
    EXPECT_TRUE(j.contains("ts"));
    EXPECT_TRUE(j.contains("mono_ms"));
    EXPECT_TRUE(j.contains("seq"));
    EXPECT_FALSE(nlohmann::json::parse(events[1].json).contains("backend"));  // absent, not empty
}

TEST(EventLog, WritesOneJsonObjectPerLineAndFlushesOnStop) {
    const auto dir = temp_dir("write");
    const auto file = dir / "logs" / "events.jsonl";  // the directory is created
    {
        EventLog log(config(file.string()), lb::Clock::now());
        std::string error;
        ASSERT_TRUE(log.start(&error)) << error;
        for (int i = 0; i < 25; ++i) log.emit("e", "event " + std::to_string(i), {{"i", i}});
        log.stop();
    }
    const auto lines = read_lines(file);
    ASSERT_EQ(lines.size(), 25u);
    for (int i = 0; i < 25; ++i) EXPECT_EQ(lines[static_cast<std::size_t>(i)]["i"], i);  // in order
}

TEST(EventLog, RotatesBySizeAndKeepsMaxFiles) {
    const auto dir = temp_dir("rotate");
    const auto file = dir / "events.jsonl";
    {
        EventLog log(config(file.string(), 4096, 2), lb::Clock::now());
        std::string error;
        ASSERT_TRUE(log.start(&error)) << error;
        for (int i = 0; i < 300; ++i) log.emit("e", std::string(100, 'x'), {{"i", i}});
        log.stop();
    }
    EXPECT_TRUE(fs::exists(file));
    EXPECT_TRUE(fs::exists(dir / "events.jsonl.1"));
    EXPECT_TRUE(fs::exists(dir / "events.jsonl.2"));
    EXPECT_FALSE(fs::exists(dir / "events.jsonl.3"));
    for (const auto& f : {file, dir / "events.jsonl.1", dir / "events.jsonl.2"}) {
        EXPECT_LE(fs::file_size(f), 4096u) << f;
        EXPECT_FALSE(read_lines(f).empty()) << f;  // every line still valid JSON
    }
    EXPECT_EQ(read_lines(file).back()["i"], 299);  // newest entries in the live file
}

TEST(EventLog, RecentEventsAreCapped) {
    auto c = config("");
    c.recent_events = 5;
    EventLog log(c, lb::Clock::now());
    std::string error;
    ASSERT_TRUE(log.start(&error));
    for (int i = 0; i < 12; ++i) log.emit("e", std::to_string(i));
    const auto events = log.recent();
    ASSERT_EQ(events.size(), 5u);
    EXPECT_EQ(events.front().message, "7");
    EXPECT_EQ(events.back().message, "11");
}

TEST(EventLog, InvalidUtf8IsReplacedNotFatal) {
    EventLog log(config(""), lb::Clock::now());
    std::string error;
    ASSERT_TRUE(log.start(&error));
    log.emit("e", "bad \xC3\x28 bytes", {{"peer_text", std::string("\xFF\xFE")}});
    const auto events = log.recent();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_NO_THROW((void)nlohmann::json::parse(events[0].json));
}

TEST(EventLog, UnwritablePathFailsToStart) {
    const auto dir = temp_dir("unwritable");
    const auto blocker = dir / "not-a-directory";
    std::ofstream(blocker) << "x";
    EventLog log(config((blocker / "events.jsonl").string()), lb::Clock::now());
    std::string error;
    EXPECT_FALSE(log.start(&error));
    EXPECT_NE(error.find("cannot open event log file"), std::string::npos) << error;
}
