#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"

namespace lb {

// Fields a running engine cannot change (plan IV.14 lists what can). A reload that changes
// any of them is rejected as a whole, never half-applied; the list names each one.
std::vector<std::string> restart_only_changes(const ConfigSnapshot& active, const ConfigSnapshot& next);

// FNV-1a 64 over the file's bytes: the watcher ignores a change whose content equals the
// active config (plan IV.14: a GUI save must not trigger a second reload).
std::uint64_t config_content_hash(std::string_view bytes) noexcept;

}  // namespace lb
