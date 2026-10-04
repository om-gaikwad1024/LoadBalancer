#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"

namespace lb {

struct ConfigError {
    std::string path;  // JSON pointer of the offending field ("" = document root)
    std::string message;
};

std::string to_string(const ConfigError& error);

struct ConfigLoadResult {
    std::shared_ptr<const ConfigSnapshot> snapshot;  // null when errors is non-empty
    std::vector<ConfigError> errors;

    bool ok() const noexcept { return snapshot != nullptr; }
};

// Parses and fully validates a config document before anything is built (plan IV.14).
// Every field is required; unknown fields, duplicate JSON keys, wrong types and
// out-of-range values are rejected. All errors found are reported, not just the first.
ConfigLoadResult parse_config(std::string_view json_text);

ConfigLoadResult load_config_file(const std::filesystem::path& path);

}  // namespace lb
