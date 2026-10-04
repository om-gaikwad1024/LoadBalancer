#include "config/config_loader.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

namespace lb {

const GroupConfig* ConfigSnapshot::find_group(std::string_view name) const noexcept {
    for (const auto& g : groups) {
        if (g.name == name) return &g;
    }
    return nullptr;
}

bool ConfigSnapshot::is_trusted_proxy(std::uint32_t address_host_order) const noexcept {
    for (const auto& c : trusted_proxies) {
        if (c.contains(address_host_order)) return true;
    }
    return false;
}

std::string to_string(const ConfigError& error) {
    return (error.path.empty() ? std::string("(root)") : error.path) + ": " + error.message;
}

namespace {

using json = nlohmann::json;

// Schema bounds. These define what a valid config is (documented in
// docs/config-reference.md); they are not runtime tunables.
constexpr std::uint64_t kMaxPort = 65535;
constexpr std::uint64_t kMinWorkerThreads = 1;
constexpr std::uint64_t kMaxWorkerThreads = 256;
constexpr std::uint64_t kMinWeight = 1;
constexpr std::uint64_t kMaxWeight = 1000;
constexpr std::size_t kMaxNameLength = 64;
constexpr std::uint64_t kMinRequestLineBytes = 64;
constexpr std::uint64_t kMinHeaderBytes = 256;
constexpr std::uint64_t kMaxHeaderBytes = 1024 * 1024;
constexpr std::uint64_t kMaxHeaderCount = 10000;
constexpr std::uint64_t kMinChunkLineBytes = 16;
constexpr std::uint64_t kMaxChunkLineBytes = 64 * 1024;
constexpr std::uint64_t kMaxBacklog = 65535;
constexpr std::uint64_t kMaxPendingAccepts = 1024;
constexpr std::uint64_t kMaxClientConnections = 1'000'000;
constexpr std::uint64_t kMinReadBufferBytes = 1024;
constexpr std::uint64_t kMaxReadBufferBytes = 1024 * 1024;
constexpr std::uint64_t kMaxTimeoutMs = 600'000;
constexpr std::uint64_t kMaxPoolConnections = 65535;
constexpr std::uint64_t kMaxPoolWaiters = 1'000'000;
constexpr std::uint64_t kMaxIdleTimeoutMs = 3'600'000;
constexpr std::uint64_t kMinMaintenanceMs = 10;
constexpr std::uint64_t kMaxMaintenanceMs = 60'000;
constexpr std::string_view kAutoThreads = "auto";
constexpr std::string_view kAnyAddress = "0.0.0.0";

std::string escape_pointer_token(std::string_view token) {
    std::string out;
    for (char c : token) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

std::string child(const std::string& path, std::string_view key) {
    return path + "/" + escape_pointer_token(key);
}

std::string child(const std::string& path, std::size_t index) {
    return path + "/" + std::to_string(index);
}

// nlohmann/json keeps the last value of a duplicated key silently. A duplicated key
// is almost always an editing mistake, so the parse callback records each one.
class DuplicateKeyDetector {
public:
    bool on_event(json::parse_event_t event, const json& parsed) {
        switch (event) {
            case json::parse_event_t::object_start:
                frames_.push_back(Frame{false, {}, {}, 0});
                break;
            case json::parse_event_t::array_start:
                frames_.push_back(Frame{true, {}, {}, 0});
                break;
            case json::parse_event_t::key: {
                Frame& top = frames_.back();
                top.current_key = parsed.get<std::string>();
                if (!top.keys.insert(top.current_key).second) {
                    duplicates_.push_back(child(parent_path(), top.current_key));
                }
                break;
            }
            case json::parse_event_t::value:
                if (!frames_.empty() && frames_.back().is_array) ++frames_.back().index;
                break;
            case json::parse_event_t::object_end:
            case json::parse_event_t::array_end:
                frames_.pop_back();
                if (!frames_.empty() && frames_.back().is_array) ++frames_.back().index;
                break;
        }
        return true;
    }

    const std::vector<std::string>& duplicates() const noexcept { return duplicates_; }

private:
    struct Frame {
        bool is_array;
        std::set<std::string> keys;
        std::string current_key;
        std::size_t index;
    };

    // Pointer to the innermost open container (the object that owns the current key).
    std::string parent_path() const {
        std::string path;
        for (std::size_t i = 0; i + 1 < frames_.size(); ++i) {
            path = frames_[i].is_array ? child(path, frames_[i].index)
                                       : child(path, frames_[i].current_key);
        }
        return path;
    }

    std::vector<Frame> frames_;
    std::vector<std::string> duplicates_;
};

class Validator {
public:
    std::vector<ConfigError> errors;

    void error(std::string path, std::string message) {
        errors.push_back({std::move(path), std::move(message)});
    }

    // Requires an object with exactly the listed keys: each missing or unknown key is an error.
    bool check_object(const json& j, const std::string& path, std::initializer_list<std::string_view> keys) {
        if (!j.is_object()) {
            error(path, "expected an object");
            return false;
        }
        for (const auto& item : j.items()) {
            if (std::find(keys.begin(), keys.end(), item.key()) == keys.end()) {
                error(child(path, item.key()), "unknown field");
            }
        }
        for (std::string_view key : keys) {
            if (!j.contains(std::string(key))) error(child(path, key), "missing required field");
        }
        return true;
    }

    // Returns the member or nullptr; missing members were already reported by check_object.
    static const json* field(const json& obj, std::string_view key) {
        const auto it = obj.find(std::string(key));
        return it == obj.end() ? nullptr : &*it;
    }

    std::optional<std::uint64_t> get_uint(const json& obj, const std::string& path, std::string_view key,
                                          std::uint64_t min, std::uint64_t max) {
        const json* v = field(obj, key);
        if (v == nullptr) return std::nullopt;
        const std::string p = child(path, key);
        if (!v->is_number_integer()) {
            error(p, "expected an integer");
            return std::nullopt;
        }
        const bool negative = !v->is_number_unsigned() && v->get<std::int64_t>() < 0;
        const std::uint64_t value = negative ? 0 : v->get<std::uint64_t>();
        if (negative || value < min || value > max) {
            error(p, "must be between " + std::to_string(min) + " and " + std::to_string(max));
            return std::nullopt;
        }
        return value;
    }

    std::optional<std::string> get_string(const json& obj, const std::string& path, std::string_view key) {
        const json* v = field(obj, key);
        if (v == nullptr) return std::nullopt;
        if (!v->is_string()) {
            error(child(path, key), "expected a string");
            return std::nullopt;
        }
        return v->get<std::string>();
    }

    // Group names and backend ids: 1-64 characters of [A-Za-z0-9._-].
    std::optional<std::string> get_name(const json& obj, const std::string& path, std::string_view key) {
        auto value = get_string(obj, path, key);
        if (!value) return std::nullopt;
        const bool valid_chars = std::all_of(value->begin(), value->end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                   c == '_' || c == '-';
        });
        if (value->empty() || value->size() > kMaxNameLength || !valid_chars) {
            error(child(path, key), "must be 1-" + std::to_string(kMaxNameLength) + " characters of [A-Za-z0-9._-]");
            return std::nullopt;
        }
        return value;
    }

    std::optional<std::string> get_ipv4(const json& obj, const std::string& path, std::string_view key,
                                        bool allow_any) {
        auto value = get_string(obj, path, key);
        if (!value) return std::nullopt;
        in_addr addr{};
        if (::inet_pton(AF_INET, value->c_str(), &addr) != 1) {
            error(child(path, key), "must be an IPv4 address literal (e.g. 127.0.0.1)");
            return std::nullopt;
        }
        if (!allow_any && *value == kAnyAddress) {
            error(child(path, key), "0.0.0.0 is not a connectable backend address");
            return std::nullopt;
        }
        return value;
    }
};

void build_listen(const json& root, Validator& v, ConfigSnapshot& out) {
    const std::string path = "/listen";
    const json* j = Validator::field(root, "listen");
    if (j == nullptr || !v.check_object(*j, path, {"address", "port", "backlog", "pending_accepts"})) return;
    if (auto a = v.get_ipv4(*j, path, "address", /*allow_any=*/true)) out.listen.address = *a;
    if (auto p = v.get_uint(*j, path, "port", 0, kMaxPort)) out.listen.port = static_cast<std::uint16_t>(*p);
    if (auto b = v.get_uint(*j, path, "backlog", 1, kMaxBacklog)) out.listen.backlog = static_cast<std::uint32_t>(*b);
    if (auto a = v.get_uint(*j, path, "pending_accepts", 1, kMaxPendingAccepts)) {
        out.listen.pending_accepts = static_cast<std::uint32_t>(*a);
    }
}

void build_buffers(const json& root, Validator& v, ConfigSnapshot& out) {
    const std::string path = "/buffers";
    const json* j = Validator::field(root, "buffers");
    if (j == nullptr || !v.check_object(*j, path, {"client_read_bytes", "backend_read_bytes"})) return;
    if (auto n = v.get_uint(*j, path, "client_read_bytes", kMinReadBufferBytes, kMaxReadBufferBytes)) {
        out.buffers.client_read_bytes = static_cast<std::uint32_t>(*n);
    }
    if (auto n = v.get_uint(*j, path, "backend_read_bytes", kMinReadBufferBytes, kMaxReadBufferBytes)) {
        out.buffers.backend_read_bytes = static_cast<std::uint32_t>(*n);
    }
}

void build_pool(const json& root, Validator& v, ConfigSnapshot& out) {
    const std::string path = "/pool";
    const json* j = Validator::field(root, "pool");
    if (j == nullptr || !v.check_object(*j, path,
                                        {"max_connections_per_backend", "max_idle_per_backend", "idle_timeout_ms",
                                         "max_waiters_per_backend", "wait_timeout_ms"})) {
        return;
    }
    PoolConfig& p = out.pool;
    const auto set = [&](std::string_view key, std::uint64_t min, std::uint64_t max, std::uint32_t& field) {
        if (auto value = v.get_uint(*j, path, key, min, max)) field = static_cast<std::uint32_t>(*value);
    };
    set("max_connections_per_backend", 1, kMaxPoolConnections, p.max_connections_per_backend);
    set("max_idle_per_backend", 0, kMaxPoolConnections, p.max_idle_per_backend);
    set("idle_timeout_ms", 1, kMaxIdleTimeoutMs, p.idle_timeout_ms);
    set("max_waiters_per_backend", 0, kMaxPoolWaiters, p.max_waiters_per_backend);
    set("wait_timeout_ms", 1, kMaxTimeoutMs, p.wait_timeout_ms);
    if (p.max_connections_per_backend != 0 && p.max_idle_per_backend > p.max_connections_per_backend) {
        v.error(child(path, "max_idle_per_backend"), "must not exceed max_connections_per_backend");
    }
}

void build_maintenance(const json& root, Validator& v, ConfigSnapshot& out) {
    const std::string path = "/maintenance";
    const json* j = Validator::field(root, "maintenance");
    if (j == nullptr || !v.check_object(*j, path, {"interval_ms"})) return;
    if (auto n = v.get_uint(*j, path, "interval_ms", kMinMaintenanceMs, kMaxMaintenanceMs)) {
        out.maintenance.interval_ms = static_cast<std::uint32_t>(*n);
    }
}

void build_timeouts(const json& root, Validator& v, ConfigSnapshot& out) {
    const std::string path = "/timeouts";
    const json* j = Validator::field(root, "timeouts");
    if (j == nullptr ||
        !v.check_object(*j, path,
                        {"client_header_ms", "client_body_idle_ms", "client_keepalive_idle_ms", "client_write_idle_ms",
                         "backend_connect_ms", "backend_response_ms", "backend_idle_ms", "shutdown_grace_ms"})) {
        return;
    }
    TimeoutsConfig& t = out.timeouts;
    const auto set = [&](std::string_view key, std::uint64_t min, std::uint32_t& field) {
        if (auto value = v.get_uint(*j, path, key, min, kMaxTimeoutMs)) field = static_cast<std::uint32_t>(*value);
    };
    set("client_header_ms", 1, t.client_header_ms);
    set("client_body_idle_ms", 1, t.client_body_idle_ms);
    set("client_keepalive_idle_ms", 1, t.client_keepalive_idle_ms);
    set("client_write_idle_ms", 1, t.client_write_idle_ms);
    set("backend_connect_ms", 1, t.backend_connect_ms);
    set("backend_response_ms", 1, t.backend_response_ms);
    set("backend_idle_ms", 1, t.backend_idle_ms);
    set("shutdown_grace_ms", 0, t.shutdown_grace_ms);
}

// "a.b.c.d/n" or a single address ("a.b.c.d" = /32). Host bits beyond the prefix are an error,
// since they usually mean a typo.
std::optional<Ipv4Cidr> parse_cidr(std::string_view text) {
    const auto slash = text.find('/');
    const std::string address(text.substr(0, slash));
    in_addr addr{};
    if (::inet_pton(AF_INET, address.c_str(), &addr) != 1) return std::nullopt;
    unsigned prefix = 32;
    if (slash != std::string_view::npos) {
        const std::string_view p = text.substr(slash + 1);
        if (p.empty() || p.size() > 2 || !std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        prefix = static_cast<unsigned>(std::stoul(std::string(p)));
        if (prefix > 32) return std::nullopt;
    }
    Ipv4Cidr c;
    c.mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
    const std::uint32_t host = ::ntohl(addr.s_addr);
    if ((host & ~c.mask) != 0) return std::nullopt;
    c.network = host;
    return c;
}

void build_trusted_proxies(const json& root, Validator& v, ConfigSnapshot& out) {
    const std::string path = "/trusted_proxies";
    const json* j = Validator::field(root, "trusted_proxies");
    if (j == nullptr) return;
    if (!j->is_array()) {
        v.error(path, "expected an array of IPv4 CIDR strings (may be empty)");
        return;
    }
    for (std::size_t i = 0; i < j->size(); ++i) {
        const json& e = (*j)[i];
        const auto parsed = e.is_string() ? parse_cidr(e.get<std::string>()) : std::nullopt;
        if (!parsed) {
            v.error(child(path, i), "must be an IPv4 address or CIDR such as 10.0.0.0/8, without host bits");
            continue;
        }
        out.trusted_proxies.push_back(*parsed);
    }
}

void build_workers(const json& root, Validator& v, ConfigSnapshot& out) {
    const std::string path = "/workers";
    const json* j = Validator::field(root, "workers");
    if (j == nullptr || !v.check_object(*j, path, {"threads"})) return;
    const json* threads = Validator::field(*j, "threads");
    if (threads == nullptr) return;
    if (threads->is_string()) {
        if (threads->get<std::string>() != kAutoThreads) {
            v.error(child(path, "threads"), "must be \"auto\" or an integer");
        }
        out.workers.threads = std::nullopt;
        return;
    }
    if (auto n = v.get_uint(*j, path, "threads", kMinWorkerThreads, kMaxWorkerThreads)) {
        out.workers.threads = static_cast<std::uint32_t>(*n);
    }
}

void build_limits(const json& root, Validator& v, ConfigSnapshot& out) {
    const std::string path = "/limits";
    const json* j = Validator::field(root, "limits");
    if (j == nullptr || !v.check_object(*j, path,
                                        {"max_request_line_bytes", "max_request_header_bytes",
                                         "max_request_header_count", "max_response_header_bytes",
                                         "max_response_header_count", "max_chunk_line_bytes",
                                         "max_client_connections"})) {
        return;
    }
    const auto set = [&](std::string_view key, std::uint64_t min, std::uint64_t max, std::uint32_t& field) {
        if (auto value = v.get_uint(*j, path, key, min, max)) field = static_cast<std::uint32_t>(*value);
    };
    LimitsConfig& l = out.limits;
    set("max_request_line_bytes", kMinRequestLineBytes, kMaxHeaderBytes, l.max_request_line_bytes);
    set("max_request_header_bytes", kMinHeaderBytes, kMaxHeaderBytes, l.max_request_header_bytes);
    set("max_request_header_count", 1, kMaxHeaderCount, l.max_request_header_count);
    set("max_response_header_bytes", kMinHeaderBytes, kMaxHeaderBytes, l.max_response_header_bytes);
    set("max_response_header_count", 1, kMaxHeaderCount, l.max_response_header_count);
    set("max_chunk_line_bytes", kMinChunkLineBytes, kMaxChunkLineBytes, l.max_chunk_line_bytes);
    set("max_client_connections", 1, kMaxClientConnections, l.max_client_connections);
}

// Returns true when the groups section is fully valid (routing checks depend on it).
bool build_groups(const json& root, Validator& v, ConfigSnapshot& out) {
    const std::string path = "/groups";
    const json* j = Validator::field(root, "groups");
    if (j == nullptr) return false;
    if (!j->is_array()) {
        v.error(path, "expected an array");
        return false;
    }
    if (j->empty()) {
        v.error(path, "must contain at least one group");
        return false;
    }

    const std::size_t errors_before = v.errors.size();
    std::map<std::string, std::string> group_names;  // name -> first path
    std::map<std::string, std::string> backend_ids;  // id -> first path

    for (std::size_t gi = 0; gi < j->size(); ++gi) {
        const json& gj = (*j)[gi];
        const std::string gpath = child(path, gi);
        if (!v.check_object(gj, gpath, {"name", "strategy", "host_header", "backends"})) continue;

        GroupConfig group;
        if (auto name = v.get_name(gj, gpath, "name")) {
            const auto [it, inserted] = group_names.emplace(*name, child(gpath, "name"));
            if (!inserted) v.error(child(gpath, "name"), "duplicate group name (first at " + it->second + ")");
            group.name = *name;
        }
        if (auto strategy = v.get_string(gj, gpath, "strategy")) {
            if (*strategy == "round_robin") group.strategy = Strategy::RoundRobin;
            else if (*strategy == "least_connections") group.strategy = Strategy::LeastConnections;
            else v.error(child(gpath, "strategy"), "must be \"round_robin\" or \"least_connections\"");
        }
        if (auto mode = v.get_string(gj, gpath, "host_header")) {
            if (*mode == "preserve") group.host_header = HostHeaderMode::Preserve;
            else if (*mode == "backend") group.host_header = HostHeaderMode::Backend;
            else v.error(child(gpath, "host_header"), "must be \"preserve\" or \"backend\"");
        }

        const json* bj = Validator::field(gj, "backends");
        const std::string bpath = child(gpath, "backends");
        if (bj != nullptr && !bj->is_array()) {
            v.error(bpath, "expected an array");
        } else if (bj != nullptr && bj->empty()) {
            v.error(bpath, "must contain at least one backend");
        } else if (bj != nullptr) {
            std::map<std::pair<std::string, std::uint16_t>, std::string> endpoints;
            for (std::size_t bi = 0; bi < bj->size(); ++bi) {
                const json& b = (*bj)[bi];
                const std::string p = child(bpath, bi);
                if (!v.check_object(b, p, {"id", "address", "port", "weight"})) continue;

                BackendConfig backend;
                if (auto id = v.get_name(b, p, "id")) {
                    const auto [it, inserted] = backend_ids.emplace(*id, child(p, "id"));
                    if (!inserted) v.error(child(p, "id"), "duplicate backend id (first at " + it->second + ")");
                    backend.id = *id;
                }
                const auto address = v.get_ipv4(b, p, "address", /*allow_any=*/false);
                const auto port = v.get_uint(b, p, "port", 1, kMaxPort);
                if (const auto weight = v.get_uint(b, p, "weight", kMinWeight, kMaxWeight)) {
                    backend.weight = static_cast<std::uint32_t>(*weight);
                }
                if (address && port) {
                    backend.address = *address;
                    backend.port = static_cast<std::uint16_t>(*port);
                    const auto [it, inserted] = endpoints.emplace(std::pair{backend.address, backend.port}, p);
                    if (!inserted) v.error(p, "duplicate backend address:port in this group (first at " + it->second + ")");
                }
                group.backends.push_back(std::move(backend));
            }
        }
        out.groups.push_back(std::move(group));
    }
    return v.errors.size() == errors_before;
}

void build_routing(const json& root, Validator& v, ConfigSnapshot& out, bool groups_valid) {
    const std::string path = "/routing";
    const json* j = Validator::field(root, "routing");
    if (j == nullptr || !v.check_object(*j, path, {"default_group"})) return;
    if (auto name = v.get_string(*j, path, "default_group")) {
        if (groups_valid && out.find_group(*name) == nullptr) {
            v.error(child(path, "default_group"), "unknown group \"" + *name + "\"");
        }
        out.routing.default_group = *name;
    }
}

}  // namespace

ConfigLoadResult parse_config(std::string_view json_text) {
    ConfigLoadResult result;

    DuplicateKeyDetector duplicates;
    json root;
    try {
        root = json::parse(
            json_text.begin(), json_text.end(),
            [&duplicates](int /*depth*/, json::parse_event_t event, json& parsed) {
                return duplicates.on_event(event, parsed);
            },
            /*allow_exceptions=*/true, /*ignore_comments=*/false);
    } catch (const json::parse_error& e) {
        result.errors.push_back({"", std::string("invalid JSON: ") + e.what()});
        return result;
    }

    Validator v;
    for (const auto& path : duplicates.duplicates()) v.error(path, "duplicate key");

    auto snapshot = std::make_shared<ConfigSnapshot>();
    if (v.check_object(root, "", {"listen", "workers", "limits", "buffers", "pool", "maintenance", "timeouts",
                                  "trusted_proxies", "groups", "routing"})) {
        build_trusted_proxies(root, v, *snapshot);
        build_listen(root, v, *snapshot);
        build_workers(root, v, *snapshot);
        build_limits(root, v, *snapshot);
        build_buffers(root, v, *snapshot);
        build_pool(root, v, *snapshot);
        build_maintenance(root, v, *snapshot);
        build_timeouts(root, v, *snapshot);
        const bool groups_valid = build_groups(root, v, *snapshot);
        build_routing(root, v, *snapshot, groups_valid);
    }

    if (v.errors.empty()) {
        result.snapshot = std::move(snapshot);
    } else {
        result.errors = std::move(v.errors);
    }
    return result;
}

ConfigLoadResult load_config_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ConfigLoadResult result;
        result.errors.push_back({"", "cannot open config file: " + path.string()});
        return result;
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        ConfigLoadResult result;
        result.errors.push_back({"", "error reading config file: " + path.string()});
        return result;
    }
    return parse_config(text);
}

}  // namespace lb
