#pragma once

// Pure flush-volume normalization logic, extracted from SliceEngine's
// normalize_flush_volumes_matrix for unit testing. The algorithm is ported
// from desktop PresetBundle::update_multi_material_filament_presets
// (upstream OrcaSlicer PresetBundle.cpp). This header is deliberately
// dependency-light: only <cmath>/<cstddef>/<vector>. It must NOT include
// anything that pulls in libslic3r, so it can be compiled into the
// lightweight engine-tests target (which does not link libslic3r) — same
// constraint as PlateGrid.hpp / ValidateClassify.hpp.

#include <cmath>
#include <cstddef>
#include <vector>

namespace orca {

// Normalize the (load, unload) flush pair vector to exactly 2*num_filaments
// entries and rebuild the flush matrix as num_filaments x num_filaments,
// preserving the old matrix's top-left old_n x old_n block (synthesized
// diagonal is 0, off-diagonal is load+unload from the pair vector).
// filaments is normalized in place; the rebuilt matrix is returned.
// old_matrix may be any size — its effective dimension is
// floor(sqrt(size) + kEps), kEps matching libslic3r EPSILON (1e-4).
inline std::vector<double> normalize_flush_volumes(std::vector<double>& filaments,
                                                   const std::vector<double>& old_matrix,
                                                   size_t num_filaments)
{
    constexpr double kEps = 1e-4; // matches libslic3r EPSILON (libslic3r.h)
    const size_t old_n = static_cast<size_t>(std::sqrt(static_cast<double>(old_matrix.size())) + kEps);

    // Resize flush_volumes_vector to 2*num_filaments (default 140. per upstream).
    while (filaments.size() < 2 * num_filaments) {
        filaments.push_back(filaments.size() > 1 ? filaments[0] : 140.);
        filaments.push_back(filaments.size() > 1 ? filaments[1] : 140.);
    }
    while (filaments.size() > 2 * num_filaments) {
        filaments.pop_back();
        filaments.pop_back();
    }

    // Rebuild the matrix to num_filaments x num_filaments: copy old pairs that
    // still fit, synthesise the rest (diagonal=0, off-diagonal=load+unload).
    std::vector<double> new_matrix;
    new_matrix.reserve(num_filaments * num_filaments);
    for (size_t i = 0; i < num_filaments; ++i)
        for (size_t j = 0; j < num_filaments; ++j) {
            if (i < old_n && j < old_n)
                new_matrix.push_back(old_matrix[i * old_n + j]);
            else
                new_matrix.push_back(i == j ? 0. : filaments[2 * i] + filaments[2 * j + 1]);
        }
    return new_matrix;
}

} // namespace orca
