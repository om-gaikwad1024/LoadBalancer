#pragma once

#include <string_view>

namespace lb {

// Engine facade. Grows step by step; the app talks to the engine only through
// this header and the snapshot types it exposes (plan IV.17, V).
std::string_view engine_version() noexcept;

}  // namespace lb
