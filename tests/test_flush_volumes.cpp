/**
 * @file test_flush_volumes.cpp
 * @brief Unit tests for orca::normalize_flush_volumes (extracted from
 *        SliceEngine's normalize_flush_volumes_matrix).
 *
 * Locks the pair-vector parity invariant: whatever the input length, the
 * pad/shrink loops must land on exactly 2*num_filaments entries, because the
 * matrix rebuild reads filaments[2*i + 1] (max index 2n-1). Odd-length
 * inputs are reachable from hand-crafted 3MF config blocks (the JSON array
 * deserializes 1:1 with no parity validation anywhere on the import chain);
 * with the pre-fix paired loops an odd length converges to 2n-1 and the
 * rebuild reads one element past the end (std::vector::operator[] = UB).
 */

#include <catch_amalgamated.hpp>

#include "FlushVolumes.hpp"

#include <vector>

using orca::normalize_flush_volumes;

namespace {
std::vector<double> filled(size_t n, double v) { return std::vector<double>(n, v); }
} // namespace

TEST_CASE("flush volumes converges to exactly 2n entries for every input parity", "[flush]") {
    for (size_t n : {1u, 2u, 3u, 4u}) {
        for (size_t start : {0u, 1u, 2u, 3u, 5u, 7u, 9u, 12u, 13u}) {
            std::vector<double> filaments = filled(start, 140.);
            // Empty old matrix (old_n = 0) forces every cell through the
            // synthesized branch, exercising the filaments[] reads the
            // invariant protects.
            auto matrix = normalize_flush_volumes(filaments, {}, n);
            CHECK(filaments.size() == 2 * n);
            CHECK(matrix.size() == n * n);
        }
    }
}

TEST_CASE("flush volumes shrink side normalizes pairs without reading them", "[flush]") {
    // old_n > num_filaments: every cell copies from the old matrix block, so
    // the pair vector is never read — but it is still normalized in place
    // (and written back to the config by the caller), so the invariant must
    // hold here too.
    std::vector<double> old_matrix(36, 7.); // old_n = 6
    std::vector<double> filaments = filled(13, 140.); // odd start
    auto matrix = normalize_flush_volumes(filaments, old_matrix, 4);
    CHECK(filaments.size() == 8);
    CHECK(matrix.size() == 16);
    for (double v : matrix)
        CHECK(v == 7.);
}

TEST_CASE("flush volumes synthesized cells use pair sums", "[flush]") {
    std::vector<double> filaments = {10., 20., 30., 40.}; // already 2n for n=2
    auto matrix = normalize_flush_volumes(filaments, {}, 2);
    // Everything synthesized: diagonal 0, (0,1)=f[0]+f[1]=30, (1,0)=f[2]+f[3]=70.
    CHECK(matrix.size() == 4);
    CHECK(matrix[0] == 0.);
    CHECK(matrix[3] == 0.);
    CHECK(matrix[1] == 30.);
    CHECK(matrix[2] == 70.);
    CHECK(filaments.size() == 4); // already in range: untouched
}

TEST_CASE("flush volumes odd singleton pads from default base", "[flush]") {
    // size 1 → repaired to 0 → padded with the upstream default 140. pairs
    // (there is nothing to copy from).
    std::vector<double> filaments = {999.};
    auto matrix = normalize_flush_volumes(filaments, {}, 2);
    CHECK(filaments.size() == 4);
    CHECK(filaments[0] == 140.);
    CHECK(filaments[3] == 140.);
    CHECK(matrix[1] == 280.); // 140 + 140
    CHECK(matrix[2] == 280.);
}
