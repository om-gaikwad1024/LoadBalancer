#pragma once

#include <cstdint>

namespace lb::health {

// Plan IV.10 hysteresis: a backend goes down after N consecutive failed probes and comes
// back after M consecutive successful ones, so a single blip does not flap it.
class Hysteresis {
public:
    enum class Change : std::uint8_t { None, MarkDown, MarkUp };

    Hysteresis(std::uint32_t down_after, std::uint32_t up_after) noexcept : down_after_(down_after), up_after_(up_after) {}

    // `currently_up`: the backend currently counts as up (healthy or draining).
    Change record(bool success, bool currently_up) noexcept {
        if (success) {
            failures_ = 0;
            if (successes_ < up_after_) ++successes_;
            return !currently_up && successes_ >= up_after_ ? Change::MarkUp : Change::None;
        }
        successes_ = 0;
        if (failures_ < down_after_) ++failures_;
        return currently_up && failures_ >= down_after_ ? Change::MarkDown : Change::None;
    }

    std::uint32_t failures_in_a_row() const noexcept { return failures_; }
    std::uint32_t successes_in_a_row() const noexcept { return successes_; }

private:
    std::uint32_t down_after_;
    std::uint32_t up_after_;
    std::uint32_t failures_ = 0;
    std::uint32_t successes_ = 0;
};

}  // namespace lb::health
