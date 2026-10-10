#include "JsonReport.hpp"
#include "Utils.hpp"

#include <cmath>
#include <fstream>
#include <iostream>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "Types.hpp"
#include "libslic3r/libslic3r.h"

using ordered_json = nlohmann::ordered_json;

// Round to 2 decimal places — matches the original std::setprecision(2) output
static double round2(double v)
{
    if (!std::isfinite(v))
        return 0.0;
    return std::round(v * 100.0) / 100.0;
}

static const char* issue_level_name(IssueLevel level)
{
    switch (level)
    {
    case IssueLevel::error:
        return "error";
    case IssueLevel::serious_warning:
        return "serious_warning";
    case IssueLevel::warning:
        return "warning";
    case IssueLevel::tip:
        return "tip";
    }
    return "unknown";
}

static ordered_json issue_to_json(const Issue& issue)
{
    ordered_json j;
    j["level"] = issue_level_name(issue.level);
    j["plate_id"] = issue.plate_id >= 0 ? issue.plate_id + 1 : issue.plate_id;
    j["object_name"] = issue.object_name;
    j["z_height"] = issue.z_height;
    if (issue.layer >= 0)
        j["layer"] = issue.layer;
    j["code"] = issue.code;
    j["message"] = issue.message;
    if (!issue.suggestion.empty())
        j["suggestion"] = issue.suggestion;
    return j;
}

std::string build_statistics_json(const SliceOutputStats& stats, const std::string& output_file_path)
{
    ordered_json root;

    // ---- Aggregate totals for print_info_total ----
    double total_print_time = 0;
    double total_weight = 0;
    double total_length = 0;
    std::vector<ordered_json> total_filaments;
    // G-code actual support flag: per-plate support extrusion volume
    // (GCodeProcessor support_volumes_per_extruder, roles
    // erSupportMaterial/Interface/Transition — normal, tree and raft support)
    // is non-zero. NOT the config-level enable_support flag: a self-supporting
    // model with support enabled extrudes zero support and stays "free".
    // Aggregated from successful plates only; failed plates keep the false
    // default. Internal polarity mirrors the source (true = support extruded);
    // it is inverted to support_free only at the JSON emission sites below.
    bool any_support_generated = false;
    {
        // Deduplicate by (type, color), summing used_g across plates
        struct AggFilament
        {
            std::string type;
            std::string color;
            double used_g = 0;
            double used_m = 0;
        };
        std::vector<AggFilament> agg;
        for (const auto& plate : stats.plates)
        {
            if (plate.success)
            {
                total_print_time += plate.print_time;
                total_weight += plate.total_filament_g;
                total_length += plate.total_filament_m;
                any_support_generated = any_support_generated || plate.support_generated;
                for (const auto& detail : plate.filament_details)
                {
                    auto it = std::find_if(agg.begin(), agg.end(),
                                           [&](const AggFilament& a)
                                           {
                                               return a.type == detail.type && a.color == detail.color;
                                           });
                    if (it != agg.end())
                    {
                        it->used_g += detail.used_g;
                        it->used_m += detail.used_m;
                    }
                    else
                        agg.push_back({detail.type, detail.color, detail.used_g, detail.used_m});
                }
            }
        }
        for (const auto& a : agg)
        {
            ordered_json fj;
            fj["type"] = a.type;
            fj["color"] = a.color;
            fj["used_g"] = round2(a.used_g);
            fj["used_mm"] = round2(a.used_m * 1000.0);
            total_filaments.push_back(std::move(fj));
        }
    }

    root["success"] = stats.success;
    root["engine_version"] = ENGINE_VERSION;
    // Resources (vendor profile) version read verbatim from
    // <resources>/profiles/Snapmaker.json at report time — no Semver
    // normalisation. Absent when the file could not be read; consumers must
    // treat MISSING as "unknown", never as a specific version.
    if (!stats.resources_version.empty())
        root["resources_version"] = stats.resources_version;

    ordered_json print_info;
    print_info["output_file"] = stats.success ? output_file_path : "";
    print_info["print_time_seconds"] = round2(total_print_time);
    print_info["print_time_formatted"] = format_time_hhmmss(static_cast<float>(total_print_time));
    print_info["total_weight_g"] = round2(total_weight);
    print_info["total_length_mm"] = round2(total_length);
    print_info["plate_count"] = static_cast<int>(stats.plates.size());
    print_info["filaments"] = total_filaments;
    // support_free = !any_support_generated (gcode actual layer). Emitted only
    // on overall success (all plates sliced): a failed plate's support state
    // was never determined, so aggregating it would be a guess. Consumers must
    // treat a MISSING support_free as "unknown", never as "support-free".
    // true = no support extrusion in any plate's gcode (config-enabled but
    // self-supporting geometry counts as free); false = support structures
    // (incl. raft) were actually extruded on some plate.
    if (stats.success)
        print_info["support_free"] = !any_support_generated;
    // Input-file property determined at load time (any virtual mixed-filament
    // slot reference in any plate), so unlike support_free it survives a
    // partially failed run. Emitted whenever at least one plate was processed;
    // absent on load-stage failures where the model was never parsed.
    if (!stats.plates.empty())
        print_info["mixed_filament_used"] = stats.mixed_filament_used;
    root["print_info_total"] = std::move(print_info);

    // Global issues
    ordered_json global_issues = ordered_json::array();
    for (const auto& issue : stats.issues)
        global_issues.push_back(issue_to_json(issue));
    root["issues"] = std::move(global_issues);

    // Plates
    ordered_json plates_json = ordered_json::array();
    for (const auto& plate : stats.plates)
    {
        ordered_json pj;
        pj["plate_id"] = plate.plate_id + 1;
        pj["success"] = plate.success;

        if (!plate.issues.empty())
        {
            ordered_json pi = ordered_json::array();
            for (const auto& iss : plate.issues)
                pi.push_back(issue_to_json(iss));
            pj["issues"] = std::move(pi);
        }

        if (plate.success)
        {
            pj["print_time_seconds"] = round2(plate.print_time);
            pj["print_time_formatted"] = format_time_hhmmss(plate.print_time);
            pj["total_weight_g"] = round2(plate.total_filament_g);

            ordered_json fils = ordered_json::array();
            for (const auto& detail : plate.filament_details)
            {
                ordered_json fdj;
                fdj["id"] = detail.id + 1;
                fdj["type"] = detail.type;
                fdj["color"] = detail.color;
                fdj["used_g"] = round2(detail.used_g);
                fdj["used_mm"] = round2(detail.used_m * 1000.0);
                fils.push_back(std::move(fdj));
            }
            pj["filaments"] = std::move(fils);
            pj["model_thumbnail"] = plate.model_thumbnail;
            pj["long_retraction_when_cut"] = plate.long_retraction_when_cut;
            // G-code actual layer, inverted at emission: source field is the
            // per-plate support extrusion volume > 0 (GCodeProcessor
            // support_volumes_per_extruder), not the config enable flag.
            pj["support_free"] = !plate.support_generated;
        }

        plates_json.push_back(std::move(pj));
    }
    root["plates"] = std::move(plates_json);

    return root.dump(2);
}

void output_slice_statistics(const SliceOutputStats& stats, const std::string& json_output_path,
                             const std::string& output_file_path)
{
    std::string json_str = build_statistics_json(stats, output_file_path);

    std::cout << "\n=== SLICE STATISTICS (JSON) ===" << std::endl;
    std::cout << json_str << std::endl;
    std::cout << "=== END STATISTICS ===" << std::endl;

    if (!json_output_path.empty())
    {
        std::ofstream ofs(json_output_path);
        if (ofs.is_open())
        {
            ofs << json_str;
        }
        else
        {
            BOOST_LOG_TRIVIAL(warning) << "Failed to write statistics to: " << json_output_path;
        }
    }
}
