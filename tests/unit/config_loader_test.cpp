#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "config/config_loader.h"

using json = nlohmann::json;
using lb::ConfigLoadResult;

namespace {

json good_config() {
    return json::parse(R"({
      "listen":  { "address": "0.0.0.0", "port": 8080 },
      "workers": { "threads": 4 },
      "groups": [
        { "name": "web", "backends": [
            { "id": "web-1", "address": "127.0.0.1", "port": 9001, "weight": 3 },
            { "id": "web-2", "address": "127.0.0.2", "port": 9002, "weight": 1 } ] },
        { "name": "api", "backends": [
            { "id": "api-1", "address": "10.0.0.5", "port": 7000, "weight": 1 } ] }
      ],
      "routing": { "default_group": "web" }
    })");
}

ConfigLoadResult load(const json& j) { return lb::parse_config(j.dump()); }

std::string describe(const ConfigLoadResult& r) {
    std::string out;
    for (const auto& e : r.errors) out += "  " + lb::to_string(e) + "\n";
    return out.empty() ? "  (no errors)\n" : out;
}

bool has_error(const ConfigLoadResult& r, std::string_view path, std::string_view fragment) {
    for (const auto& e : r.errors) {
        if (e.path == path && e.message.find(fragment) != std::string::npos) return true;
    }
    return false;
}

#define EXPECT_REJECTED_WITH(result, path, fragment)                                               \
    do {                                                                                           \
        const auto& r_ = (result);                                                                 \
        EXPECT_FALSE(r_.ok());                                                                     \
        EXPECT_EQ(r_.snapshot, nullptr);                                                           \
        EXPECT_TRUE(has_error(r_, path, fragment))                                                 \
            << "expected error at '" << (path) << "' containing '" << (fragment) << "', got:\n"  \
            << describe(r_);                                                                       \
    } while (0)

json with(json j, const char* pointer, json value) {
    j[json::json_pointer(pointer)] = std::move(value);
    return j;
}

// Every object member in the document, depth-first (array elements are traversed, not listed).
void collect_members(const json& j, const json::json_pointer& at, std::vector<json::json_pointer>& out) {
    if (j.is_object()) {
        for (const auto& item : j.items()) {
            const auto p = at / item.key();
            out.push_back(p);
            collect_members(item.value(), p, out);
        }
    } else if (j.is_array()) {
        for (std::size_t i = 0; i < j.size(); ++i) collect_members(j[i], at / i, out);
    }
}

}  // namespace

// ---- Accepts good configs -------------------------------------------------------------

TEST(ConfigLoader, AcceptsGoodConfigAndMapsEveryField) {
    const auto r = load(good_config());
    ASSERT_TRUE(r.ok()) << describe(r);
    EXPECT_TRUE(r.errors.empty());

    const lb::ConfigSnapshot& c = *r.snapshot;
    EXPECT_EQ(c.listen.address, "0.0.0.0");
    EXPECT_EQ(c.listen.port, 8080);
    ASSERT_TRUE(c.workers.threads.has_value());
    EXPECT_EQ(*c.workers.threads, 4u);
    ASSERT_EQ(c.groups.size(), 2u);
    EXPECT_EQ(c.groups[0].name, "web");
    ASSERT_EQ(c.groups[0].backends.size(), 2u);
    EXPECT_EQ(c.groups[0].backends[0].id, "web-1");
    EXPECT_EQ(c.groups[0].backends[0].address, "127.0.0.1");
    EXPECT_EQ(c.groups[0].backends[0].port, 9001);
    EXPECT_EQ(c.groups[0].backends[0].weight, 3u);
    EXPECT_EQ(c.groups[1].backends[0].id, "api-1");
    EXPECT_EQ(c.routing.default_group, "web");
    ASSERT_NE(c.find_group("api"), nullptr);
    EXPECT_EQ(c.find_group("nope"), nullptr);
}

TEST(ConfigLoader, ThreadsAutoMeansUnresolved) {
    const auto r = load(with(good_config(), "/workers/threads", "auto"));
    ASSERT_TRUE(r.ok()) << describe(r);
    EXPECT_FALSE(r.snapshot->workers.threads.has_value());
}

TEST(ConfigLoader, ListenPortZeroIsAllowedForEphemeralBinding) {
    EXPECT_TRUE(load(with(good_config(), "/listen/port", 0)).ok());
}

TEST(ConfigLoader, SameEndpointInDifferentGroupsIsAllowed) {
    auto j = with(good_config(), "/groups/1/backends/0/address", "127.0.0.1");
    j = with(j, "/groups/1/backends/0/port", 9001);
    const auto r = load(j);
    EXPECT_TRUE(r.ok()) << describe(r);
}

TEST(ConfigLoader, Utf8BomIsAccepted) {
    const auto r = lb::parse_config("\xEF\xBB\xBF" + good_config().dump());
    EXPECT_TRUE(r.ok()) << describe(r);
}

TEST(ConfigLoader, ShippedExampleConfigIsValid) {
    const auto r = lb::load_config_file(LB_SOURCE_DIR "/config/lb.example.json");
    EXPECT_TRUE(r.ok()) << describe(r);
}

// ---- Rejects each class of bad config (plan IX "Unit": config validation) ------------

TEST(ConfigLoader, EveryFieldIsRequired) {
    const json base = good_config();
    std::vector<json::json_pointer> members;
    collect_members(base, json::json_pointer(), members);
    ASSERT_GT(members.size(), 20u);

    for (const auto& p : members) {
        SCOPED_TRACE(p.to_string());
        json j = base;
        j[p.parent_pointer()].erase(p.back());
        EXPECT_REJECTED_WITH(load(j), p.to_string(), "missing required field");
    }
}

TEST(ConfigLoader, UnknownFieldsAreRejectedAtEveryLevel) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/extra", 1)), "/extra", "unknown field");
    EXPECT_REJECTED_WITH(load(with(good_config(), "/listen/backlog", 1)), "/listen/backlog", "unknown field");
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups/1/backends/0/note", "x")),
                         "/groups/1/backends/0/note", "unknown field");
}

TEST(ConfigLoader, ErrorPathsEscapeJsonPointerCharacters) {
    EXPECT_REJECTED_WITH(lb::parse_config(R"({"a/b~c": 1})"), "/a~1b~0c", "unknown field");
}

struct BadValueCase {
    const char* pointer;
    json value;
    const char* message_fragment;
};

void PrintTo(const BadValueCase& c, std::ostream* os) {
    *os << c.pointer << " = " << c.value.dump(-1, ' ', /*ensure_ascii=*/true);
}

// Readable ctest names: "<pointer>_<index>", e.g. listen_port_2.
std::string bad_value_name(const ::testing::TestParamInfo<BadValueCase>& info) {
    std::string name;
    for (const char* p = info.param.pointer; *p != '\0'; ++p) {
        const bool alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (alnum) name += *p;
        else if (!name.empty() && name.back() != '_') name += '_';
    }
    return name + "_" + std::to_string(info.index);
}

class ConfigLoaderBadValue : public ::testing::TestWithParam<BadValueCase> {};

TEST_P(ConfigLoaderBadValue, IsRejectedAtItsPath) {
    const auto& c = GetParam();
    EXPECT_REJECTED_WITH(load(with(good_config(), c.pointer, c.value)), c.pointer, c.message_fragment);
}

INSTANTIATE_TEST_SUITE_P(WrongType, ConfigLoaderBadValue, ::testing::Values(
    BadValueCase{"/listen", "x", "expected an object"},
    BadValueCase{"/listen/port", "8080", "expected an integer"},
    BadValueCase{"/listen/port", 80.5, "expected an integer"},
    BadValueCase{"/listen/port", true, "expected an integer"},
    BadValueCase{"/listen/port", nullptr, "expected an integer"},
    BadValueCase{"/listen/address", 127, "expected a string"},
    BadValueCase{"/workers/threads", 2.0, "expected an integer"},
    BadValueCase{"/workers/threads", "AUTO", "must be \"auto\" or an integer"},
    BadValueCase{"/groups", json::object(), "expected an array"},
    BadValueCase{"/groups/0", json::array(), "expected an object"},
    BadValueCase{"/groups/0/backends", "web-1", "expected an array"},
    BadValueCase{"/groups/0/backends/1", 5, "expected an object"},
    BadValueCase{"/groups/0/backends/0/weight", "3", "expected an integer"},
    BadValueCase{"/routing/default_group", 5, "expected a string"}), bad_value_name);

INSTANTIATE_TEST_SUITE_P(OutOfRange, ConfigLoaderBadValue, ::testing::Values(
    BadValueCase{"/listen/port", 65536, "between 0 and 65535"},
    BadValueCase{"/listen/port", -1, "between 0 and 65535"},
    BadValueCase{"/listen/port", json::parse("18446744073709551616"), "expected an integer"},
    BadValueCase{"/groups/0/backends/0/port", 0, "between 1 and 65535"},
    BadValueCase{"/workers/threads", 0, "between 1 and 256"},
    BadValueCase{"/workers/threads", 257, "between 1 and 256"},
    BadValueCase{"/groups/0/backends/0/weight", 0, "between 1 and 1000"},
    BadValueCase{"/groups/0/backends/0/weight", 1001, "between 1 and 1000"}), bad_value_name);

INSTANTIATE_TEST_SUITE_P(BadAddress, ConfigLoaderBadValue, ::testing::Values(
    BadValueCase{"/listen/address", "localhost", "IPv4 address literal"},
    BadValueCase{"/listen/address", "", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", "1.2.3", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", "256.1.1.1", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", "::1", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", " 127.0.0.1", "IPv4 address literal"},
    BadValueCase{"/groups/0/backends/0/address", "0.0.0.0", "not a connectable backend address"}), bad_value_name);

INSTANTIATE_TEST_SUITE_P(BadName, ConfigLoaderBadValue, ::testing::Values(
    BadValueCase{"/groups/0/name", "", "1-64 characters"},
    BadValueCase{"/groups/0/name", "has space", "1-64 characters"},
    BadValueCase{"/groups/0/name", std::string(65, 'a'), "1-64 characters"},
    BadValueCase{"/groups/0/backends/0/id", "web/1", "1-64 characters"},
    BadValueCase{"/groups/0/backends/0/id", "w\xC3\xA9" "b", "1-64 characters"}), bad_value_name);

TEST(ConfigLoader, EmptyGroupListIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups", json::array())), "/groups", "at least one group");
}

TEST(ConfigLoader, GroupWithoutBackendsIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups/1/backends", json::array())), "/groups/1/backends",
                         "at least one backend");
}

TEST(ConfigLoader, DuplicateGroupNameIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups/1/name", "web")), "/groups/1/name",
                         "duplicate group name (first at /groups/0/name)");
}

TEST(ConfigLoader, DuplicateBackendIdAcrossGroupsIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/groups/1/backends/0/id", "web-1")), "/groups/1/backends/0/id",
                         "duplicate backend id (first at /groups/0/backends/0/id)");
}

TEST(ConfigLoader, DuplicateEndpointWithinGroupIsRejected) {
    auto j = with(good_config(), "/groups/0/backends/1/address", "127.0.0.1");
    j = with(j, "/groups/0/backends/1/port", 9001);
    EXPECT_REJECTED_WITH(load(j), "/groups/0/backends/1", "duplicate backend address:port");
}

TEST(ConfigLoader, UnknownDefaultGroupIsRejected) {
    EXPECT_REJECTED_WITH(load(with(good_config(), "/routing/default_group", "nope")), "/routing/default_group",
                         "unknown group \"nope\"");
}

TEST(ConfigLoader, DuplicateJsonKeysAreRejected) {
    std::string text = good_config().dump();
    const auto replace = [&text](std::string_view from, std::string_view to) {
        const auto pos = text.find(from);
        ASSERT_NE(pos, std::string::npos) << from;
        text.replace(pos, from.size(), to);
    };
    replace(R"("port":8080)", R"("port":8080,"port":8081)");
    replace(R"("id":"api-1")", R"("id":"api-1","id":"api-9")");

    const auto r = lb::parse_config(text);
    EXPECT_REJECTED_WITH(r, "/listen/port", "duplicate key");
    EXPECT_REJECTED_WITH(r, "/groups/1/backends/0/id", "duplicate key");
}

TEST(ConfigLoader, MalformedJsonIsRejectedWithPosition) {
    const auto r = lb::parse_config("{\n  \"listen\": }");
    EXPECT_REJECTED_WITH(r, "", "invalid JSON");
    EXPECT_REJECTED_WITH(r, "", "line 2");
}

TEST(ConfigLoader, NonObjectDocumentsAreRejected) {
    EXPECT_REJECTED_WITH(lb::parse_config(""), "", "invalid JSON");
    EXPECT_REJECTED_WITH(lb::parse_config("[]"), "", "expected an object");
    EXPECT_REJECTED_WITH(lb::parse_config("null"), "", "expected an object");
}

TEST(ConfigLoader, CommentsAreNotJson) {
    EXPECT_REJECTED_WITH(lb::parse_config("// note\n" + good_config().dump()), "", "invalid JSON");
}

TEST(ConfigLoader, HalfWrittenFileIsRejected) {
    const std::string full = good_config().dump(2);
    EXPECT_REJECTED_WITH(lb::parse_config(full.substr(0, full.size() / 2)), "", "invalid JSON");
}

TEST(ConfigLoader, ReportsAllErrorsNotJustTheFirst) {
    auto j = with(good_config(), "/listen/port", "x");
    j = with(j, "/groups/0/backends/1/weight", 0);
    j = with(j, "/workers/extra", true);
    const auto r = load(j);
    EXPECT_REJECTED_WITH(r, "/listen/port", "expected an integer");
    EXPECT_REJECTED_WITH(r, "/groups/0/backends/1/weight", "between 1 and 1000");
    EXPECT_REJECTED_WITH(r, "/workers/extra", "unknown field");
}

TEST(ConfigLoader, MissingFileIsRejected) {
    EXPECT_REJECTED_WITH(lb::load_config_file(LB_SOURCE_DIR "/config/does-not-exist.json"), "",
                         "cannot open config file");
}

TEST(ConfigLoader, ErrorToStringShowsRootAndPath) {
    EXPECT_EQ(lb::to_string({"", "bad"}), "(root): bad");
    EXPECT_EQ(lb::to_string({"/listen/port", "bad"}), "/listen/port: bad");
}
