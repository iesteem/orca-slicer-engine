#include "PresetManager.hpp"
#include "SliceEngine.hpp"   // for EngineConfig full definition

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "Types.hpp"
#include "Utils.hpp"
#include "PresetRollback.h"

using namespace Slic3r;

namespace {

// --- Config helpers ---

// Full overwrite: copy all values from src into dst unconditionally.
// Scalar options use set(); vector options use set_at() per index.
// Non-existent keys in dst are skipped.  Used by preset substitution
// to completely replace user values with official preset values.
inline void overwrite_all_keys_from(DynamicPrintConfig& dst, const DynamicPrintConfig& src)
{
    for (auto it = src.cbegin(); it != src.cend(); ++it) {
        const auto& key   = it->first;
        auto       dst_opt = dst.option(key, false);
        if (!dst_opt) continue;

        if (dst_opt->is_scalar()) {
            dst_opt->set(it->second.get());
        } else {
            auto dst_vec = dynamic_cast<ConfigOptionVectorBase*>(dst_opt);
            auto src_vec = dynamic_cast<const ConfigOptionVectorBase*>(it->second.get());
            if (!dst_vec || !src_vec) continue;
            for (size_t i = 0; i < dst_vec->size() && i < src_vec->size(); ++i)
                dst_vec->set_at(src_vec, i, i);
        }
    }
}

// G-code keys that must always be overwritten from the official printer preset.
// Bambu-specific G-code variables are not recognized by Snapmaker's
// PlaceholderParser.  Cloud slicing must always use official G-code.
constexpr const char* GCODE_KEYS[] = {
    "machine_start_gcode", "machine_end_gcode",
    "before_layer_change_gcode", "layer_change_gcode",
    "change_filament_gcode", "machine_pause_gcode",
    "template_custom_gcode", "printing_by_object_gcode",
    "time_lapse_gcode",
};

inline void overwrite_gcode_keys_from(DynamicPrintConfig& dst,
                                      const DynamicPrintConfig& src)
{
    for (const auto& key : GCODE_KEYS) {
        const auto* s = src.option(key);
        if (s && dst.has(key))
            dst.set_key_value(key, s->clone());
    }
}


// --- Official preset file helpers ---

// Directory constant for Snapmaker machine preset files under resources/profiles/
static const char* const SNAPMK_MACHINE_DIR = "/profiles/Snapmaker/machine/";


// Named constants for magic numbers
constexpr double NOZZLE_FORMAT_EPSILON    = 0.05;

constexpr double DEFAULT_PLATE_WIDTH       = 200.0;
constexpr double DEFAULT_PLATE_DEPTH       = 200.0;
constexpr double DEFAULT_PRINTABLE_HEIGHT  = 100.0;

} // namespace

PresetManager::PresetManager(EngineContext& ctx)
    : m_ctx(ctx)
{
}

void PresetManager::load_system_presets()
{
    // Always derive profiles path from resources_dir/profiles/
    const std::string res_dir = resources_dir();
    if (res_dir.empty()) {
        BOOST_LOG_TRIVIAL(info) << "No resources directory set; skipping preset validation";
        return;
    }
    std::string profiles_path = res_dir + "/profiles";

    boost::filesystem::path profiles_dir(profiles_path);
    if (!boost::filesystem::exists(profiles_dir) ||
        !boost::filesystem::is_directory(profiles_dir)) {
        BOOST_LOG_TRIVIAL(warning)
            << "Profiles directory not found: " << profiles_dir.string()
            << "; skipping preset validation";
        return;
    }

    // Collect vendor JSON files
    std::vector<std::string> vendor_names;
    for (auto& entry : boost::filesystem::directory_iterator(profiles_dir)) {
        std::string file = entry.path().string();
        if (!is_json_file(file))
            continue;
        std::string name = entry.path().filename().string();
        name.erase(name.size() - 5); // strip .json
        vendor_names.push_back(name);
    }

    if (vendor_names.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "No vendor JSON files in " << profiles_dir.string()
            << "; skipping preset validation";
        return;
    }

    // Move OrcaFilamentLibrary to the front (cross-vendor filament inheritance)
    for (size_t i = 0; i < vendor_names.size(); ++i) {
        if (vendor_names[i] == PresetBundle::ORCA_FILAMENT_LIBRARY) {
            std::swap(vendor_names[0], vendor_names[i]);
            break;
        }
    }

    try {
        m_preset_bundle = std::make_unique<PresetBundle>();

        const auto rule = ForwardCompatibilitySubstitutionRule::EnableSilent;

        // Only load Snapmaker + OrcaFilamentLibrary
        {
            const std::string vendors[] = { PresetBundle::ORCA_FILAMENT_LIBRARY, "Snapmaker" };
            for (const auto& vendor : vendors) {
                m_preset_bundle->load_vendor_configs_from_json(
                    profiles_dir.string(), vendor,
                    PresetBundle::LoadSystem, rule, nullptr);
            }
        }

        m_presets_available = true;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning)
            << "Failed to load system presets: " << e.what()
            << "; preset validation skipped";
        m_preset_bundle.reset();
        m_presets_available = false;
    }
}

void PresetManager::validate_presets()
{
    if (!m_presets_available || !m_preset_bundle) {
        BOOST_LOG_TRIVIAL(info) << "Preset validation skipped (system presets not available)";
        return;
    }

    PresetBundle& bundle = *m_preset_bundle;

    // B2: Load project embedded presets
    if (!m_ctx.project_presets.empty()) {
        try {
            PresetsConfigSubstitutions preset_subs =
                bundle.load_project_embedded_presets(
                    m_ctx.project_presets,
                    ForwardCompatibilitySubstitutionRule::Enable);

            for (const auto& ps : preset_subs) {
                for (const auto& sub : ps.substitutions) {
                    const char* key = sub.opt_def ? sub.opt_def->opt_key.c_str() : "?";
                    m_ctx.stats.issues.push_back(make_warning(-1, "PRESET_SUBSTITUTION",
                        std::string("Embedded preset '") + ps.preset_name
                        + "' key '" + key + "' was substituted"));
                }
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning)
                << "Failed to load project embedded presets: " << e.what();
        }
    }

    // B3: Validate presets against system profiles
    try {
        std::set<std::string> modified_gcodes;
        int validated = bundle.validate_presets(
            m_ctx.cfg.input_file, m_ctx.config, modified_gcodes);

        switch (validated) {
        case VALIDATE_PRESETS_SUCCESS:
            BOOST_LOG_TRIVIAL(info) << "Preset validation passed";
            break;

        case VALIDATE_PRESETS_PRINTER_NOT_FOUND: {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            std::string msg = "Custom printer preset not found in system presets";
            if (!details.empty())
                msg += ": " + details;
            m_ctx.stats.issues.push_back(make_warning(-1, "PRESET_PRINTER_NOT_FOUND", msg));
            break;
        }

        case VALIDATE_PRESETS_FILAMENTS_NOT_FOUND: {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            std::string msg = "Custom filament preset not found in system presets";
            if (!details.empty())
                msg += ": " + details;
            m_ctx.stats.issues.push_back(make_warning(-1, "PRESET_FILAMENT_NOT_FOUND", msg));
            break;
        }

        case VALIDATE_PRESETS_MODIFIED_GCODES: {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            std::string msg = "Custom G-code detected in presets (" + details
                + ") — will be replaced with official G-code for cloud safety";
            m_ctx.stats.issues.push_back(make_warning(-1, "PRESET_MODIFIED_GCODES", msg));
            break;
        }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning)
            << "Preset validation error: " << e.what();
    }
}


bool PresetManager::has_inline_filament_config(int ext_idx)
{
    auto is_non_nil = [&](const char* key) -> bool {
        if (!m_ctx.config.has(key)) return false;
        auto opt = m_ctx.config.option<ConfigOptionFloats>(key, true);
        if (!opt) return false;
        if (static_cast<int>(opt->values.size()) <= ext_idx) return false;
        return !opt->is_nil(ext_idx) && opt->values[ext_idx] > 0;
    };

    if (is_non_nil("nozzle_temperature")) return true;
    if (is_non_nil("filament_diameter")) return true;

    if (m_ctx.config.has("filament_type")) {
        auto ft = m_ctx.config.option<ConfigOptionStrings>("filament_type", true);
        if (ft && ext_idx < static_cast<int>(ft->values.size())
              && !ft->values[ext_idx].empty())
            return true;
    }

    return false;
}

bool PresetManager::validate_filament_official(bool enforce)
{
    if (!m_presets_available || !m_preset_bundle)
        return true;

    if (!m_ctx.config.has("filament_settings_id"))
        return true;

    auto filament_ids = m_ctx.config.option<ConfigOptionStrings>("filament_settings_id", true);
    if (!filament_ids || filament_ids->values.empty())
        return true;

    int num_filaments = static_cast<int>(filament_ids->values.size());
    bool any_error = false;

    auto report = [&](bool is_official_violation, const std::string& code, const std::string& msg) {
        if (!enforce && is_official_violation) {
            BOOST_LOG_TRIVIAL(warning) << msg;
            m_ctx.stats.issues.push_back(make_warning(-1, code, msg));
        } else {
            BOOST_LOG_TRIVIAL(error) << msg;
            m_ctx.stats.issues.push_back(make_error(-1, code, msg));
            any_error = true;
        }
    };

    auto is_official_preset = [](const Preset& p) -> bool {
        if (p.vendor && p.vendor->name == PresetBundle::SM_BUNDLE)
            return true;
        if (p.m_from_orca_filament_lib)
            return true;
        return false;
    };

    auto find_in_system = [this](const std::string& name) -> Preset* {
        auto p = m_preset_bundle->filaments.find_preset(name, false);
        if (p && p->name == name) return p;
        for (auto& preset : m_preset_bundle->filaments) {
            if (preset.name == name) return &preset;
        }
        return nullptr;
    };

    auto find_in_project = [this](const std::string& name) -> Preset* {
        for (auto pp : m_ctx.project_presets) {
            if (pp && pp->name == name && pp->type == Preset::TYPE_FILAMENT)
                return pp;
        }
        return nullptr;
    };

    for (int i = 0; i < num_filaments; ++i) {
        // Trim whitespace — upstream 3MF files sometimes carry trailing
        // spaces in preset names (e.g. "Generic PLA " → "Generic PLA").
        std::string name = filament_ids->values[i];
        boost::trim(name);
        filament_ids->values[i] = name;  // write back trimmed value

        // Already an official preset in the system bundle — nothing to do.
        Preset* sys = find_in_system(name);
        if (sys && is_official_preset(*sys)) {
            continue;
        }

        // Locate the preset (system or project-embedded).
        Preset* current = sys ? sys : find_in_project(name);
        if (!current) {
            // Non-enforce: accept inline filament config if present.
            if (!enforce && has_inline_filament_config(i)) {
                std::string msg = "Filament \"" + name
                    + "\" is a custom filament without a preset definition"
                    + " — accepted in allow-custom mode";
                BOOST_LOG_TRIVIAL(warning) << msg;
                m_ctx.stats.issues.push_back(make_warning(-1, "FILAMENT_CUSTOM_INLINE", msg));
                continue;
            }

            // Enforce: preset not found in system or project — try PresetRollback
            // to locate a base-category filament by filament_type from m_config.
            if (PresetRollback::rollback(m_ctx.config, m_preset_bundle.get(), i))
                continue;

            // PresetRollback also failed — terminal.
            std::string msg = "Filament \"" + name
                + "\" is not a recognized preset and no base-category filament found";
            BOOST_LOG_TRIVIAL(error) << msg;
            m_ctx.stats.issues.push_back(make_error(-1, "FILAMENT_NO_OFFICIAL_ANCESTOR", msg));
            m_ctx.any_error = true;
            if (EXIT_PREPROCESS_ERROR > m_ctx.error_type)
                m_ctx.error_type = EXIT_PREPROCESS_ERROR;
            return false;
        }

        // Non-enforce mode: walk the inherits chain purely for validation,
        // warn but don't substitute.
        if (!enforce) {
            std::set<std::string> visited;
            Preset* walk = current;
            bool ok = true;
            while (walk) {
                std::string ih = walk->inherits();
                if (ih.empty()) break;
                if (!visited.insert(ih).second) {
                    m_ctx.stats.issues.push_back(make_error(-1, "FILAMENT_CIRCULAR_INHERITS",
                        "Circular inheritance detected in filament \"" + name + "\""));
                    any_error = true;
                    ok = false;
                    break;
                }
                Preset* next = find_in_system(ih);
                if (!next) next = find_in_project(ih);
                if (!next) {
                    BOOST_LOG_TRIVIAL(warning)
                        << "Filament \"" << name << "\" inherits from unknown preset \"" << ih << "\"";
                    m_ctx.stats.issues.push_back(make_warning(-1, "FILAMENT_UNKNOWN_ANCESTOR",
                        "Filament \"" + name + "\" inherits from unknown preset \"" + ih + "\""));
                    ok = false;
                    break;
                }
                walk = next;
            }
            if (ok) {
                std::string msg = "Filament \"" + name + "\" is a custom preset (not official)";
                BOOST_LOG_TRIVIAL(warning) << msg;
                m_ctx.stats.issues.push_back(make_warning(-1, "FILAMENT_CUSTOM", msg));
            }
            continue;
        }

        // ---- Enforce mode: two-path substitution ----

        // Helper to look up an ancestor by name (system first, then project-embedded).
        auto find_ancestor = [&](const std::string& ih) -> Preset* {
            Preset* p = find_in_system(ih);
            return p ? p : find_in_project(ih);
        };

        // Path 1 — walk the inherits chain looking for an official ancestor.
        bool resolved = false;
        std::set<std::string> visited;
        while (current && !resolved) {
            std::string inherits_name = current->inherits();

            // inherits chain exhausted → not found via Path 1
            if (inherits_name.empty()) break;

            // Circular inheritance.
            if (!visited.insert(inherits_name).second) {
                std::string msg = "Circular inheritance detected in filament \"" + name + "\"";
                m_ctx.stats.issues.push_back(make_error(-1, "FILAMENT_CIRCULAR_INHERITS", msg));
                any_error = true;
                resolved = true;  // stop the loop, error already set
                break;
            }

            Preset* parent = find_ancestor(inherits_name);
            // Unknown ancestor → not found via Path 1; fall through to Path 2.
            if (!parent) break;

            // Found an official ancestor — substitute and mark resolved.
            if (is_official_preset(*parent)) {
                substitute_filament_params(filament_ids, i, *parent, name);
                resolved = true;
            }
            // Non-filament type in the chain → error.
            else if (parent->type != Preset::TYPE_FILAMENT) {
                std::string msg = "Filament \"" + name
                    + "\" inherits from non-filament preset \"" + inherits_name + "\"";
                m_ctx.stats.issues.push_back(make_error(-1, "FILAMENT_UNKNOWN_ANCESTOR", msg));
                any_error = true;
                resolved = true;
            }
            // Intermediate filament preset — continue walking up.
            else {
                current = parent;
            }
        }

        // Path 2 — inherits chain did not yield an official ancestor.
        // Fall back to PresetRollback: find the base-category filament by type/vendor.
        if (!resolved) {
            if (PresetRollback::rollback(m_ctx.config, m_preset_bundle.get(), i)) {
                resolved = true;
            } else {
                // Neither inherits nor PresetRollback found a valid filament —
                // this is a terminal failure: the 3MF cannot be sliced.
                std::string msg = "Filament \"" + name
                    + "\": no official ancestor found via inherits, "
                    + "and no base-category filament found for its filament_type";
                BOOST_LOG_TRIVIAL(error) << msg;
                m_ctx.stats.issues.push_back(
                    make_error(-1, "FILAMENT_NO_OFFICIAL_ANCESTOR", msg));
                m_ctx.any_error = true;
                if (EXIT_PREPROCESS_ERROR > m_ctx.error_type)
                    m_ctx.error_type = EXIT_PREPROCESS_ERROR;
                return false;
            }
        }
    }

    if (any_error) {
        m_ctx.any_error = true;
        if (EXIT_PREPROCESS_ERROR > m_ctx.error_type) m_ctx.error_type = EXIT_PREPROCESS_ERROR;
    }

    return !any_error;
}

void PresetManager::substitute_filament_params(ConfigOptionStrings* filament_ids, int ext_idx,
                                                const Preset& official_parent,
                                                const std::string& original_name)
{
    filament_ids->values[ext_idx] = official_parent.name;

    const size_t dst_idx = static_cast<size_t>(ext_idx);

    for (auto it = official_parent.config.cbegin(); it != official_parent.config.cend(); ++it) {
        const auto& key     = it->first;
        const auto& src_opt = it->second;

        auto dst_opt = m_ctx.config.option(key, true);
        if (!dst_opt) continue;

        auto dst_vec = dynamic_cast<ConfigOptionVectorBase*>(dst_opt);
        if (!dst_vec) continue;
        if (dst_vec->size() <= dst_idx) continue;

        // Full overwrite: all filament values come from the official preset.
        auto src_vec = dynamic_cast<const ConfigOptionVectorBase*>(src_opt.get());
        if (src_vec && src_vec->size() > 0)
            dst_vec->set_at(src_vec, dst_idx, 0);
    }

    const std::string msg = original_name == official_parent.name
        ? std::string("Filament \"") + original_name
            + "\" config values updated from official preset"
        : std::string("Custom filament \"") + original_name
            + "\" replaced with official preset \""
            + official_parent.name + "\" for cloud safety";
    m_ctx.stats.issues.push_back(make_warning(-1, "FILAMENT_SUBSTITUTED", msg));
}

bool PresetManager::validate_printer_model()
{
    const std::string ALLOWED_PRINTER_MODEL = "Snapmaker U1";

    if (!m_ctx.config.has("printer_model")) {
        std::string msg = "Printer model is missing. Only Snapmaker U1 is supported.";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_ctx.any_error = true;
        if (EXIT_PREPROCESS_ERROR > m_ctx.error_type) m_ctx.error_type = EXIT_PREPROCESS_ERROR;
        m_ctx.stats.error_message = msg;
        m_ctx.stats.issues.push_back(make_error(-1, "PRINTER_MODEL_MISSING", msg));
        return false;
    }

    std::string printer_model = m_ctx.config.opt_string("printer_model");
    if (printer_model != ALLOWED_PRINTER_MODEL) {
        std::string msg = "Unsupported printer model: \"" + printer_model
                        + "\". Only Snapmaker U1 is supported.";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_ctx.any_error = true;
        if (EXIT_PREPROCESS_ERROR > m_ctx.error_type) m_ctx.error_type = EXIT_PREPROCESS_ERROR;
        m_ctx.stats.error_message = msg;
        m_ctx.stats.issues.push_back(make_error(-1, "PRINTER_MODEL_UNSUPPORTED", msg));
        return false;
    }

    return true;
}


void PresetManager::apply_printer_official_preset()
{
    double nozzle = 0.4;
    auto nd_opt = m_ctx.config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nd_opt && !nd_opt->values.empty() && nd_opt->values[0] > 0)
        nozzle = nd_opt->values[0];

    auto fmt_nozzle = [](double d) {
        std::array<char, 8> buf;
        const int precision = (std::abs(d - std::round(d)) < NOZZLE_FORMAT_EPSILON) ? 0 : 1;
        snprintf(buf.data(), buf.size(), "%.*f", precision, d);
        return std::string(buf.data());
    };

    std::string preset_name = "Snapmaker U1 (" + fmt_nozzle(nozzle) + " nozzle)";
    std::string preset_path = resources_dir()
        + SNAPMK_MACHINE_DIR + preset_name + ".json";

    if (!boost::filesystem::exists(preset_path)) {
        static const char* known[] = {"0.2", "0.4", "0.6", "0.8", nullptr};
        for (int i = 0; known[i]; ++i) {
            std::string candidate = std::string(resources_dir())
                + SNAPMK_MACHINE_DIR + "Snapmaker U1 (" + known[i] + " nozzle).json";
            if (boost::filesystem::exists(candidate)) {
                preset_path = candidate;
                preset_name = "Snapmaker U1 (" + std::string(known[i]) + " nozzle)";
                break;
            }
        }
    }

    try {
        DynamicPrintConfig official_cfg;
        std::map<std::string, std::string> key_values;
        std::string reason;
        official_cfg.load_from_json(preset_path,
            ForwardCompatibilitySubstitutionRule::EnableSilent,
            key_values, reason);

        overwrite_all_keys_from(m_ctx.config, official_cfg);

        overwrite_gcode_keys_from(m_ctx.config, official_cfg);

        m_ctx.config.set_key_value("printer_settings_id",
            new ConfigOptionString(preset_name));
        auto pm = official_cfg.option<ConfigOptionString>("printer_model");
        if (pm && m_ctx.config.has("printer_model"))
            m_ctx.config.set_key_value("printer_model",
                new ConfigOptionString(pm->value));

        m_ctx.stats.issues.push_back(make_warning(-1, "PRINTER_SUBSTITUTED",
            "Printer preset substituted with official preset \""
            + preset_name + "\" for cloud safety"));

    } catch (const std::exception& e) {
        std::string msg = "Failed to load official printer preset \""
            + preset_name + "\": " + e.what();
        BOOST_LOG_TRIVIAL(error) << msg;
        m_ctx.any_error = true;
        if (EXIT_PREPROCESS_ERROR > m_ctx.error_type) m_ctx.error_type = EXIT_PREPROCESS_ERROR;
        m_ctx.stats.error_message = msg;
        m_ctx.stats.issues.push_back(make_error(-1, "PRINTER_PRESET_LOAD_ERROR", msg));
        return;
    }

    {
        auto pa = m_ctx.config.option<ConfigOptionPoints>("printable_area");
        if (!pa || pa->values.size() != 4) {
            std::string msg = "Printer configuration incomplete: printable_area missing";
            BOOST_LOG_TRIVIAL(error) << msg;
            m_ctx.any_error = true;
            if (EXIT_PREPROCESS_ERROR > m_ctx.error_type) m_ctx.error_type = EXIT_PREPROCESS_ERROR;
            m_ctx.stats.error_message = msg;
            m_ctx.stats.issues.push_back(make_error(-1, "PRINTER_PRESET_NOT_APPLIED", msg));
            return;
        }
        bool is_default =
            (pa->values[0].x() == 0.0 && pa->values[0].y() == 0.0) &&
            (pa->values[1].x() == DEFAULT_PLATE_WIDTH &&
             pa->values[1].y() == 0.0) &&
            (pa->values[2].x() == DEFAULT_PLATE_WIDTH &&
             pa->values[2].y() == DEFAULT_PLATE_DEPTH) &&
            (pa->values[3].x() == 0.0 &&
             pa->values[3].y() == DEFAULT_PLATE_DEPTH);
        if (is_default) {
            std::string msg = "Printer configuration incomplete: "
                "printable_area is still the default. "
                "The U1 printer preset was not applied correctly.";
            BOOST_LOG_TRIVIAL(error) << msg;
            m_ctx.any_error = true;
            if (EXIT_PREPROCESS_ERROR > m_ctx.error_type) m_ctx.error_type = EXIT_PREPROCESS_ERROR;
            m_ctx.stats.error_message = msg;
            m_ctx.stats.issues.push_back(make_error(-1, "PRINTER_PRESET_NOT_APPLIED", msg));
            return;
        }
        auto ph = m_ctx.config.option<ConfigOptionFloat>("printable_height");
        if (!ph || ph->value == DEFAULT_PRINTABLE_HEIGHT) {
            std::string msg = "Printer configuration incomplete: "
                "printable_height is still the default. "
                "The U1 printer preset was not applied correctly.";
            BOOST_LOG_TRIVIAL(error) << msg;
            m_ctx.any_error = true;
            if (EXIT_PREPROCESS_ERROR > m_ctx.error_type) m_ctx.error_type = EXIT_PREPROCESS_ERROR;
            m_ctx.stats.error_message = msg;
            m_ctx.stats.issues.push_back(make_error(-1, "PRINTER_PRESET_NOT_APPLIED", msg));
            return;
        }
    }
}

