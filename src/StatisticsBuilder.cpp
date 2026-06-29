#include "StatisticsBuilder.hpp"
#include "SliceEngine.hpp"   // for EngineConfig + PlateSliceResult full defs

#include <cmath>
#include <iomanip>
#include <sstream>

#include <boost/filesystem.hpp>

#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "EngineContext.hpp"
#include "SliceEngine.hpp"
#include "Types.hpp"

using namespace Slic3r;

namespace {

// RAII guard to temporarily adjust boost::log severity threshold.
// Uses a static depth counter so nested guards don't fight over the
// filter.  On outermost exit the filter is restored to >= info so
// engine progress markers remain visible while library trace/debug
// stay suppressed.
class ScopedLogLevel {
    static int s_depth;
public:
    explicit ScopedLogLevel(boost::log::trivial::severity_level lv) {
        if (s_depth++ == 0) {
            namespace expr = boost::log::expressions;
            boost::log::core::get()->set_filter(
                expr::attr<boost::log::trivial::severity_level>("Severity") >= lv
            );
        }
    }
    ~ScopedLogLevel() {
        if (--s_depth == 0) {
            namespace expr = boost::log::expressions;
            boost::log::core::get()->set_filter(
                expr::attr<boost::log::trivial::severity_level>("Severity")
                >= boost::log::trivial::info
            );
        }
    }
    ScopedLogLevel(const ScopedLogLevel&) = delete;
    ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;
};

int ScopedLogLevel::s_depth = 0;

} // namespace

StatisticsBuilder::StatisticsBuilder(EngineContext& ctx)
    : m_ctx(ctx)
{}

// ============================================================================
// Package output as gcode.3mf
// ============================================================================

void StatisticsBuilder::package_output() {
    BOOST_LOG_TRIVIAL(info) << "Creating gcode.3mf package...";

    std::string printer_model_id;
    std::string nozzle_diameters_str;

    if (m_ctx.config.has("printer_model"))
        printer_model_id = m_ctx.config.opt_string("printer_model");

    if (m_ctx.config.has("nozzle_diameter")) {
        auto nozzle_opt = m_ctx.config.option<ConfigOptionFloats>("nozzle_diameter");
        if (nozzle_opt && !nozzle_opt->values.empty()) {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2);
            for (size_t i = 0; i < nozzle_opt->values.size(); ++i) {
                if (i > 0) ss << ",";
                ss << nozzle_opt->values[i];
            }
            nozzle_diameters_str = ss.str();
        }
    }

    const ConfigOptionStrings* filament_types = nullptr;
    const ConfigOptionStrings* filament_colors = nullptr;
    const ConfigOptionStrings* filament_ids = nullptr;

    if (m_ctx.config.has("filament_type"))
        filament_types = m_ctx.config.option<ConfigOptionStrings>("filament_type");
    if (m_ctx.config.has("filament_colour"))
        filament_colors = m_ctx.config.option<ConfigOptionStrings>("filament_colour");
    if (m_ctx.config.has("filament_ids"))
        filament_ids = m_ctx.config.option<ConfigOptionStrings>("filament_ids");

    for (auto& pd : m_ctx.plate_data) {
        auto it = m_ctx.plate_results.find(pd->plate_index);
        if (it == m_ctx.plate_results.end()) continue;

        PlateSliceResult& result = it->second;
        pd->gcode_file = result.gcode_path;
        pd->is_sliced_valid = true;
        pd->printer_model_id = printer_model_id;
        pd->nozzle_diameters = nozzle_diameters_str;

        auto& modes = result.gcode_result.print_statistics.modes;
        const int print_time = static_cast<int>(
            modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
        pd->gcode_prediction = std::to_string(print_time);

        if (result.total_weight != 0.0) {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << result.total_weight;
            pd->gcode_weight = ss.str();
        }

        pd->toolpath_outside = result.gcode_result.toolpath_outside;
        pd->timelapse_warning_code = result.gcode_result.timelapse_warning_code;
        pd->is_support_used = result.support_used;
        pd->is_label_object_enabled = result.gcode_result.label_object_enabled;

        pd->parse_filament_info(&result.gcode_result);

        for (auto& info : pd->slice_filaments_info) {
            size_t idx = static_cast<size_t>(info.id);
            if (filament_types && idx < filament_types->values.size())
                info.type = filament_types->values[idx];
            if (filament_colors && idx < filament_colors->values.size())
                info.color = filament_colors->values[idx];
            if (filament_ids && idx < filament_ids->values.size())
                info.filament_id = filament_ids->values[idx];
        }

        // Rebuild objects_and_instances using model.objects array indices
        std::set<int> plate_plate_instance_ids;
        for (const auto& entry : pd->obj_inst_map)
            plate_plate_instance_ids.insert(entry.second.second);

        pd->objects_and_instances.clear();
        for (size_t obj_idx = 0; obj_idx < m_ctx.model.objects.size(); ++obj_idx) {
            const ModelObject* obj = m_ctx.model.objects[obj_idx];
            for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx) {
                const ModelInstance* inst = obj->instances[inst_idx];
                if (plate_plate_instance_ids.count(static_cast<int>(inst->loaded_id)))
                    pd->objects_and_instances.emplace_back(
                        static_cast<int>(obj_idx),
                        static_cast<int>(inst_idx));
            }
        }

    }

    StoreParams params;
    params.path = m_ctx.output_path.c_str();
    params.plate_data_list = m_ctx.plate_data;
    params.model = &m_ctx.model;
    params.config = &m_ctx.config;
    params.project_presets = m_ctx.project_presets;

    if (m_ctx.cfg.single_plate)
        params.export_plate_idx = m_ctx.cfg.plate_id - 1;
    else
        params.export_plate_idx = -1;

    params.strategy = SaveStrategy::Silence | SaveStrategy::SplitModel |
                     SaveStrategy::WithGcode | SaveStrategy::SkipModel |
                     SaveStrategy::Zip64;

    std::vector<ThumbnailData*> thumbnail_data;
    std::vector<ThumbnailData*> no_light_thumbnail_data;
    std::vector<ThumbnailData*> top_thumbnail_data;
    std::vector<ThumbnailData*> pick_thumbnail_data;
    std::vector<ThumbnailData*> calibration_thumbnail_data;

    // Populate thumbnail vectors from decoded plate data so they are
    // embedded into the output .3mf package (Metadata/plate_*.png).
    for (const auto& pd : m_ctx.plate_data) {
        thumbnail_data.push_back(
            pd->plate_thumbnail.is_valid() ? &pd->plate_thumbnail : nullptr);
        // no_light / top / pick / calibration thumbnails are not available
        // from input .3mf — pass nullptr to skip those variants.
        no_light_thumbnail_data.push_back(nullptr);
        top_thumbnail_data.push_back(nullptr);
        pick_thumbnail_data.push_back(nullptr);
        calibration_thumbnail_data.push_back(nullptr);
    }

    std::vector<PlateBBoxData*> id_bboxes;
    std::vector<std::unique_ptr<PlateBBoxData>> id_bboxes_owned;
    id_bboxes_owned.reserve(m_ctx.plate_data.size());
    for (size_t i = 0; i < m_ctx.plate_data.size(); ++i) {
        id_bboxes_owned.push_back(std::make_unique<PlateBBoxData>());
        id_bboxes.push_back(id_bboxes_owned.back().get());
    }

    params.thumbnail_data = thumbnail_data;
    params.no_light_thumbnail_data = no_light_thumbnail_data;
    params.top_thumbnail_data = top_thumbnail_data;
    params.pick_thumbnail_data = pick_thumbnail_data;
    params.calibration_thumbnail_data = calibration_thumbnail_data;
    params.id_bboxes = id_bboxes;
    params.project = nullptr;
    params.profile = nullptr;

    try {
        ScopedLogLevel quiet_pkg(boost::log::trivial::warning);
        bool success = store_bbs_3mf(params);
        if (!success) {
            BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package";
            m_ctx.any_error = true;
            set_error_type(EXIT_EXPORT_ERROR);
            std::string msg = "Failed to create output package. The slicing result could not be saved.";
            m_ctx.stats.issues.push_back(make_error(-1, "PACKAGE_EXPORT_ERROR", msg));
            if (m_ctx.stats.error_message.empty())
                m_ctx.stats.error_message = msg;
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "gcode.3mf package created: " << m_ctx.output_path;
    }
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package: " << e.what();
        m_ctx.any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        std::string msg = "Failed to create output package due to an internal error.";
        m_ctx.stats.issues.push_back(make_error(-1, "PACKAGE_EXPORT_ERROR", msg));
        if (m_ctx.stats.error_message.empty())
            m_ctx.stats.error_message = msg;
    }
}

// ============================================================================
// Exit code derivation
// ============================================================================

void StatisticsBuilder::report_error(int plate_id, int exit_code, const std::string& code,
                                     const std::string& message, bool set_main_message) {
    m_ctx.any_error = true;
    set_error_type(exit_code);
    m_ctx.stats.issues.push_back(make_error(plate_id, code, message));
    if (set_main_message && m_ctx.stats.error_message.empty())
        m_ctx.stats.error_message = message;
}

void StatisticsBuilder::set_error_type(int code) {
    if (code > m_ctx.error_type)
        m_ctx.error_type = code;
}

int StatisticsBuilder::exit_code() const {
    if (m_ctx.error_type > EXIT_OK)
        return m_ctx.error_type;
    if (m_ctx.any_error)
        return EXIT_PREPROCESS_ERROR;
    // Post-processing warnings are non-fatal and gcode has been generated.
    // Treat as success (exit 0) per alignment with desktop behavior.
    if (m_ctx.any_postprocess_warning)
        return EXIT_OK;
    return EXIT_OK;
}

// ============================================================================
// Build statistics for JSON output
// ============================================================================

void StatisticsBuilder::build_statistics() {
    for (const auto& [plate_id, result] : m_ctx.plate_results) {
        SliceOutputStats::PlateStats plate_stats;
        plate_stats.plate_id = plate_id;

        bool plate_has_error = false;
        bool plate_has_warning = false;
        for (const auto& issue : result.issues) {
            if (issue.level == "error") plate_has_error = true;
            if (issue.level == "warning") plate_has_warning = true;
            m_ctx.stats.issues.push_back(issue);
        }

        plate_stats.success = !plate_has_error;
        if (plate_has_error) {
            m_ctx.any_error = true;
            set_error_type(EXIT_PREPROCESS_ERROR);
        }
        if (plate_has_warning || result.has_postprocess_warning)
            m_ctx.any_postprocess_warning = true;

        plate_stats.issues = result.issues;

        if (plate_stats.success) {
            plate_stats.gcode_file = m_ctx.output_path;

            auto& modes = result.gcode_result.print_statistics.modes;
            auto& normal_mode = modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)];
            plate_stats.total_time = normal_mode.time;
            plate_stats.prepare_time = normal_mode.prepare_time;
            plate_stats.print_time = normal_mode.time - normal_mode.prepare_time;
            if (!std::isfinite(plate_stats.print_time)) {
                BOOST_LOG_TRIVIAL(warning)
                    << "Plate " << plate_id << " print time is non-finite ("
                    << normal_mode.time << " - " << normal_mode.prepare_time
                    << "), falling back to 0";
                plate_stats.print_time = 0.0f;
            }

            plate_stats.total_filament_m = result.total_used_filament;
            plate_stats.total_filament_g = result.total_weight;
            plate_stats.total_cost = result.total_cost;
            plate_stats.support_used = result.support_used;
            plate_stats.toolpath_outside = result.gcode_result.toolpath_outside;
            plate_stats.long_retraction_when_cut = result.gcode_result.long_retraction_when_cut;

            // Calculate model filament
            double total_support_m = 0.0, total_support_g = 0.0;
            double total_flush_m = 0.0, total_flush_g = 0.0;
            double total_wipe_tower_m = 0.0, total_wipe_tower_g = 0.0;

            const auto& fd = result.gcode_result.filament_diameters;
            const auto& fdens = result.gcode_result.filament_densities;

            for (const auto& [extruder_id, volume] : result.filament_volumes) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double diameter = fd[extruder_id];
                    double density = fdens[extruder_id];
                    double cross_section = M_PI * 0.25 * diameter * diameter;
                    double used_m = (volume / cross_section) * 0.001;
                    double used_g = volume * density * 0.001;
                    plate_stats.filament_used_m[extruder_id] = used_m;
                    plate_stats.filament_used_g[extruder_id] = used_g;
                }
            }

            auto& ps = result.gcode_result.print_statistics;
            for (const auto& [extruder_id, volume] : ps.support_volumes_per_extruder) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_support_m += (volume / cross_section) * 0.001;
                    total_support_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            for (const auto& [extruder_id, volume] : ps.wipe_tower_volumes_per_extruder) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_wipe_tower_m += (volume / cross_section) * 0.001;
                    total_wipe_tower_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            for (const auto& [extruder_id, volume] : ps.flush_per_filament) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_flush_m += (volume / cross_section) * 0.001;
                    total_flush_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            plate_stats.model_filament_m = plate_stats.total_filament_m - total_support_m - total_flush_m - total_wipe_tower_m;
            plate_stats.model_filament_g = plate_stats.total_filament_g - total_support_g - total_flush_g - total_wipe_tower_g;

            if (m_ctx.config.has("nozzle_diameter")) {
                auto nozzle_opt = m_ctx.config.option<ConfigOptionFloats>("nozzle_diameter");
                if (nozzle_opt)
                    plate_stats.nozzle_diameters = nozzle_opt->values;
            }

            plate_stats.plate_count = static_cast<int>(m_ctx.plate_data.size());

            const ConfigOptionStrings* ftypes =
                m_ctx.config.has("filament_type") ? m_ctx.config.option<ConfigOptionStrings>("filament_type") : nullptr;
            const ConfigOptionStrings* fcolors =
                m_ctx.config.has("filament_colour") ? m_ctx.config.option<ConfigOptionStrings>("filament_colour") : nullptr;

            for (const auto& [extruder_id, used_g] : plate_stats.filament_used_g) {
                SliceOutputStats::FilamentDetail detail;
                detail.id = extruder_id;
                detail.used_g = used_g;
                detail.used_m = plate_stats.filament_used_m.count(extruder_id)
                                     ? plate_stats.filament_used_m.at(extruder_id)
                                     : 0.0;

                const size_t ext_sz = static_cast<size_t>(extruder_id);
                if (ftypes && ext_sz < ftypes->values.size())
                    detail.type = ftypes->values[extruder_id];
                else
                    detail.type = "Unknown";

                if (fcolors && ext_sz < fcolors->values.size())
                    detail.color = fcolors->values[extruder_id];
                else
                    detail.color = "#000000";

                plate_stats.filament_details.push_back(detail);
            }

            // Look up the thumbnail path from plate data
            for (const auto pd : m_ctx.plate_data) {
                if (pd->plate_index == plate_id && !pd->thumbnail_file.empty()) {
                    plate_stats.model_thumbnail = "Metadata/plate_" + std::to_string(plate_id + 1) + ".png";
                    break;
                }
            }
        } else {
            plate_stats.plate_count = static_cast<int>(m_ctx.plate_data.size());
        }

        m_ctx.stats.plates.push_back(plate_stats);
    }

    // Add placeholder plates for plates with issues but not in plate_results
    std::set<int> plates_with_results;
    for (const auto& p : m_ctx.stats.plates)
        plates_with_results.insert(p.plate_id);

    for (const auto& issue : m_ctx.stats.issues) {
        if (issue.plate_id >= 0 && plates_with_results.find(issue.plate_id) == plates_with_results.end()) {
            bool found = false;
            for (const auto& p : m_ctx.stats.plates) {
                if (p.plate_id == issue.plate_id) { found = true; break; }
            }
            if (!found) {
                SliceOutputStats::PlateStats failed_plate;
                failed_plate.plate_id = issue.plate_id;
                failed_plate.success = false;
                failed_plate.plate_count = static_cast<int>(m_ctx.plate_data.size());
                failed_plate.issues.push_back(issue);
                plates_with_results.insert(issue.plate_id);
                m_ctx.stats.plates.push_back(failed_plate);
            } else {
                for (auto& p : m_ctx.stats.plates) {
                    if (p.plate_id == issue.plate_id) {
                        p.issues.push_back(issue);
                        break;
                    }
                }
            }
        }
    }

    // Sort plates by plate_id
    std::sort(m_ctx.stats.plates.begin(), m_ctx.stats.plates.end(),
        [](const SliceOutputStats::PlateStats& a, const SliceOutputStats::PlateStats& b) {
            return a.plate_id < b.plate_id;
        });

    // Determine global success
    m_ctx.stats.success = !m_ctx.stats.plates.empty();
    for (const auto& p : m_ctx.stats.plates) {
        if (!p.success) {
            m_ctx.stats.success = false;
            break;
        }
    }
    if (!m_ctx.stats.success && m_ctx.stats.error_message.empty()) {
        if (m_ctx.stats.plates.empty())
            m_ctx.stats.error_message = "No plates completed successfully";
        else {
            int failed_count = 0;
            for (const auto& p : m_ctx.stats.plates)
                if (!p.success) ++failed_count;
            if (failed_count == static_cast<int>(m_ctx.stats.plates.size()))
                m_ctx.stats.error_message = "All plates failed with errors";
            else
                m_ctx.stats.error_message = "Some plates failed with errors";
        }
    }
}
