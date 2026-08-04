#pragma once

// Pure slicing-deadline check, extracted from SliceEngine for unit testing.
// Only <chrono>; must NOT pull in libslic3r (compiled into lightweight engine-tests).

#include <chrono>

namespace orca {

// True once the steady-clock deadline has strictly passed.
//
// Strict greater-than is intentional: at the exact instant now == deadline the
// slice is still considered in-time (the caller continues). The wrapper in
// SliceEngine additionally guards on m_has_timeout, so a default-constructed
// (epoch) deadline is never consulted unless a timeout was actually requested.
inline bool deadline_expired(std::chrono::steady_clock::time_point now,
                             std::chrono::steady_clock::time_point deadline)
{
    return now > deadline;
}

} // namespace orca
