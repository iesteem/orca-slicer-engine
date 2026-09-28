/**
 * @file test_slicing_deadline.cpp
 * @brief Unit tests for orca::deadline_expired (pure steady-clock comparison).
 *
 * The critical case is now == deadline → NOT expired. The original SliceEngine
 * logic returns true (continue slicing) while now <= deadline, so the boundary
 * instant itself must read as in-time. An off-by-one (>=) here would
 * incorrectly abort a slice the instant its deadline arrived.
 *
 * Time points are constructed from a fixed base + std::chrono::seconds offsets
 * so the tests do not depend on the wall clock.
 */

#include <catch_amalgamated.hpp>

#include <chrono>

#include "SlicingDeadline.hpp"

using orca::deadline_expired;
using sc = std::chrono::steady_clock;

TEST_CASE("deadline_expired: now well before deadline → false", "[deadline]") {
    auto deadline = sc::time_point{} + std::chrono::seconds(100);
    auto now      = sc::time_point{} + std::chrono::seconds(10);
    CHECK(!deadline_expired(now, deadline));
}

TEST_CASE("deadline_expired: now well after deadline → true", "[deadline]") {
    auto deadline = sc::time_point{} + std::chrono::seconds(10);
    auto now      = sc::time_point{} + std::chrono::seconds(100);
    CHECK(deadline_expired(now, deadline));
}

TEST_CASE("deadline_expired: now == deadline → false (off-by-one guard)", "[deadline]") {
    // The boundary instant itself is still in-time. >= would break this.
    auto t = sc::time_point{} + std::chrono::seconds(500);
    CHECK(!deadline_expired(t, t));
}

TEST_CASE("deadline_expired: now one tick before deadline → false", "[deadline]") {
    auto deadline = sc::time_point{} + std::chrono::seconds(500);
    auto now      = deadline - std::chrono::nanoseconds(1);
    CHECK(!deadline_expired(now, deadline));
}

TEST_CASE("deadline_expired: now one tick after deadline → true", "[deadline]") {
    auto deadline = sc::time_point{} + std::chrono::seconds(500);
    auto now      = deadline + std::chrono::nanoseconds(1);
    CHECK(deadline_expired(now, deadline));
}

TEST_CASE("deadline_expired: default (epoch) deadline, late now → true", "[deadline]") {
    // Pure-function truth: any now after epoch is past a default-constructed
    // deadline. (The SliceEngine wrapper guards this with m_has_timeout so the
    // epoch deadline is never consulted unless a timeout was requested.)
    auto epoch = sc::time_point{};
    auto now   = sc::time_point{} + std::chrono::seconds(1);
    CHECK(deadline_expired(now, epoch));
}
