#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"

#include "Types.hpp"

struct EngineConfig;
struct PlateSliceResult;

/**
 * @file EngineContext.hpp
 * @brief Shared mutable state bundle and transient storage for SliceEngine
 *        sub-components (PresetManager, PlateProcessor, StatisticsBuilder).
 *
 * EngineContext is owned by SliceEngine and passed by non-const reference
 * to sub-components so they can collaborate without holding references to
 * each other.  All mutable state flows through this struct.
 */

namespace Slic3r {
    class DynamicPrintConfig;
    class Preset;
    class PresetBundle;
    struct PlateData;
}

using PlateDataPtrs = std::vector<Slic3r::PlateData*>;

/**
 * Transient storage for model transforms baked during apply_model().
 *
 * Instance Z translation is moved into volume Z offset (divided by
 * Z_scale to account for Print::apply() preserving Z_scale but dropping
 * Z translation).  Restored after slicing so model state is clean for
 * the next plate.
 */
struct BakedInstanceZ
{
    BakedInstanceZ() : inst(nullptr) {}

    Slic3r::ModelInstance* inst;
    Slic3r::Vec3d          inst_offset;
    std::vector<std::pair<Slic3r::ModelVolume*, Slic3r::Vec3d>> volume_offsets;
};

struct EngineContext
{
    // --- Configuration (read-only after init) ---
    const EngineConfig&     cfg;
    std::vector<std::string>& temp_files;

    // --- Mutable pipeline state ---
    Slic3r::Model&          model;
    Slic3r::DynamicPrintConfig& config;
    SliceOutputStats&       stats;
    PlateDataPtrs&          plate_data;
    std::vector<Slic3r::Preset*>& project_presets;
    std::map<int, PlateSliceResult>& plate_results;
    std::string&            output_path;

    // --- Error tracking ---
    bool&                   any_error;
    bool&                   any_postprocess_warning;
    int&                    error_type;

    // --- Per-plate transient state ---
    std::vector<BakedInstanceZ>& baked_instance_z;

    // --- Timeout ---
    bool&                   has_timeout;
    std::chrono::steady_clock::time_point& timeout_deadline;
};
