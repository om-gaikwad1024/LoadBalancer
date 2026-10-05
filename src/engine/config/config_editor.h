#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "config/config.h"

namespace lb::admin {

// The config file as an editable document; ordered so a saved file keeps its field order.
using Document = nlohmann::ordered_json;

struct BackendFields {
    std::string group;
    std::string id;
    std::string address;
    std::uint16_t port = 0;
    std::uint32_t weight = 0;
};

// Admin edits (plan IV.17). Each changes `doc` in place, or returns an error and leaves it
// unchanged. They only find things and set fields: whether the result is a valid config is
// decided by the one validation path every reload goes through (plan IV.14).
std::string add_backend(Document& doc, const BackendFields& backend);
std::string update_backend(Document& doc, const BackendFields& backend);  // by id, in its group
std::string remove_backend(Document& doc, std::string_view id);
std::string set_backend_drain(Document& doc, std::string_view id, std::string_view directive);
std::string set_routing(Document& doc, const RoutingConfig& routing);

// Writes a temporary file next to `path`, then renames it over `path`, so a reader (or the
// file watcher) never sees a half-written config.
bool write_file_atomically(const std::filesystem::path& path, std::string_view text, std::string* error);

}  // namespace lb::admin
