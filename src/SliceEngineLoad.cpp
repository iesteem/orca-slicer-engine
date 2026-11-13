// Part of the SliceEngine class, split from SliceEngine.cpp on 2026-11-13
// (pure code move, no behavior change). Domain: input validation and 3MF
// load-side stages: file/extension checks, the Model::read_from_file
// invocation, layer-range sanitization, mixed-filament detection, and
// config/printer-model validation.

#include "SliceEngine.hpp"
#include <boost/log/trivial.hpp>
#include <sstream>
// Direct uses below (IWYU): boost::filesystem / to_lower / system / uintmax_t
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

using namespace Slic3r;

bool SliceEngine::validate_input_file()
{
    // Extension check (case-insensitive: .3mf, .3MF, .3Mf are all valid)
    std::string extension = boost::filesystem::path(m_cfg.input_file).extension().string();
    boost::to_lower(extension);
    if (extension != ".3mf")
    {
        record_stage_error(
            EXIT_LOAD_ERROR, "FORMAT_REJECTED",
            "Only .3mf files are supported. Please export your project from Snapmaker Orca Slicer.");
        return false;
    }

    // File size check (configurable via --max-size, default 200MB, 0 = no limit)
    if (m_cfg.max_size_mb <= 0) return true;

    boost::uintmax_t max_file_size = static_cast<boost::uintmax_t>(m_cfg.max_size_mb) * 1024ULL * 1024ULL;
    boost::system::error_code err_code;
    boost::uintmax_t file_size = boost::filesystem::file_size(m_cfg.input_file, err_code);
    if (!err_code && file_size > max_file_size)
    {
        record_stage_error(EXIT_LOAD_ERROR, "FILE_SIZE_EXCEEDED",
                           "File size exceeds the limit (" + std::to_string(m_cfg.max_size_mb) +
                               " MB). Please simplify the model or reduce face count and try again.");
        return false;
    }
    return true;
}

bool SliceEngine::read_3mf_model()
{
    // Reset substitution state from any prior run
    m_config_substitutions = ConfigSubstitutionContext(ForwardCompatibilitySubstitutionRule::Enable);
    // Reset synthesized-plate state from any prior run (mirrors the
    // m_config_substitutions reset; see the member declaration in the header)
    m_synthesized_single_plate = false;
    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                            LoadStrategy::LoadAuxiliary;

    try
    {
        m_model =
            Model::read_from_file(m_cfg.input_file, &m_config, &m_config_substitutions, strategy, &m_plate_data,
                                  &m_project_presets, &m_is_bbl_3mf, &m_file_version, nullptr, nullptr, nullptr, 0);
    }
    catch (const std::exception& e)
    {
        // Detect gcode.3mf output files (no geometry, only pre-sliced G-code).
        // Model::read_from_file() throws "The supplied file couldn't be read
        // because it's empty" when the 3MF has valid XML metadata but zero
        // model objects — typical of a .gcode.3mf slicing result being
        // mistakenly re-submitted as input.
        // FIXME(libslic3r-upstream): substring match on exception message is
        // brittle — a libslic3r error-text change will silently regress the
        // gcode.3mf detection. Consider an explicit signal from read_from_file.
        const std::string what = e.what();
        BOOST_LOG_TRIVIAL(error) << "Failed to load 3MF file: " << what;
        if (what.find("empty") != std::string::npos)
        {
            record_stage_error(
                EXIT_LOAD_ERROR, "LOAD_3MF_ERROR",
                "This 3MF file contains no 3D model objects. "
                "It appears to be a gcode.3mf slicing output file, not a project file. "
                "Please upload the original .3mf project file instead.");
        }
        else
        {
            record_stage_error(EXIT_LOAD_ERROR, "LOAD_3MF_ERROR",
                               "Failed to load 3MF file. The file may be corrupted or in an unsupported format.");
        }

        // Model::read_from_file() may have partially populated output parameters
        // (plate data, project presets) before throwing.  Release them to prevent
        // downstream code from accessing invalid state.
        release_PlateData_list(m_plate_data);
        m_project_presets.clear();
        return false;
    }

    if (m_model.objects.empty())
    {
        record_stage_error(EXIT_LOAD_ERROR, "MODEL_EMPTY", "3MF file contains no sliceable model objects");
        return false;
    }

    // Desktop Orca never requires plate metadata: a 3MF without <plate>
    // records (third-party exporter output) keeps the GUI's default single
    // plate with geometric instance assignment — equivalent to "plate 1 =
    // every printable instance". Model that here by synthesizing the single
    // plate and enrolling every printable instance, so all six
    // loaded_id / obj_inst_map matching sites (mark_printable_instances,
    // the three run_build_volume_check loops, check_spiral_lift, and the
    // populate_plate_data_for_export rebuild) work without further branches.
    //
    // loaded_id = inst->id().id: globally unique, always > 0, and identical
    // to what the gcode.3mf exporter writes as identify_id when SaveStrategy
    // lacks UseLoadedId — every libslic3r fallback site
    // ((loaded_id > 0) ? loaded_id : id().id) therefore computes the same
    // value it did before. Instances with printable="0" stay unenrolled:
    // desktop excludes them from slicing.
    if (m_plate_data.empty())
    {
        PlateData* pd = new PlateData(); // owned by m_plate_data / ~SliceEngine
        pd->plate_index                = 0;
        int enrolled = 0, excluded = 0, key = 0;
        for (const ModelObject* obj : m_model.objects)
        {
            for (size_t j = 0; j < obj->instances.size(); ++j)
            {
                ModelInstance* inst = obj->instances[j];
                if (!inst->printable)
                {
                    ++excluded;
                    continue;
                }
                // map key only needs uniqueness (engine consumers read
                // value.second exclusively); a running counter also stays
                // collision-free should a BBS-flagged file without plate
                // records ever carry multi-instance objects.
                inst->loaded_id = inst->id().id;
                pd->obj_inst_map.emplace(++key,
                                         std::make_pair(static_cast<int>(j),
                                                        static_cast<int>(inst->id().id)));
                ++enrolled;
            }
        }
        m_plate_data.push_back(pd);
        m_synthesized_single_plate = true;
        BOOST_LOG_TRIVIAL(info) << "3MF has no plate metadata: synthesized single plate ("
                                << enrolled << " printable instance(s) enrolled, " << excluded
                                << " excluded as printable=0)";
        m_stats.issues.push_back(make_tip(-1, "PLATE_DATA_SYNTHESIZED",
                                           "The 3MF file contains no plate metadata; slicing it as a single plate "
                                           "containing all printable objects (desktop Orca behaviour)."));
    }

    m_stats.mixed_filament_used = detect_mixed_filament_usage();
    sanitize_layer_config_ranges();
    return true;
}

// Sanitize hand-crafted / corrupt height-range data before it reaches
// libslic3r. The 3MF importer validates nothing inside
// layer_config_ranges.xml (_extract_layer_config_ranges_from_archive), and a
// range lacking "layer_height" null-derefs inside
// layer_height_profile_from_ranges (Slicing.cpp "option(...)->getFloat()"),
// killing the whole engine with SIGSEGV — verified live with a crafted
// fixture (rc 139, crash in Print::validate). Such ranges are fixed here by
// falling back to the global default layer height. Out-of-range filament ids
// are NOT rewritten: libslic3r already clamps them to extruder 1 at extruder
// collection (Print.cpp), and re-clamping here would only fork that logic —
// but the silent remap is surfaced as a warning so the output change stays
// attributable.
void SliceEngine::sanitize_layer_config_ranges()
{
    const auto* colours = m_config.option<ConfigOptionStrings>("filament_colour");
    const size_t num_physical = colours ? colours->values.size() : 0;
    // P-29 (external MR !3 review F1, 2026-10-16): m_config at this point
    // holds only keys the 3MF block actually wrote — Model::read_from_file
    // fills it directly and the defaults backfill in normalize_loaded_config
    // runs later, back in run(). A hand-crafted block can omit the global
    // layer_height entirely, and the const opt_float dereferences option()
    // without a null check (fork Config.hpp) — the sanitizer then died with
    // SIGSEGV on the very input class it exists to guard. Fall back to the
    // same FullPrintConfig default the backfill installs moments later.
    // The fallback arm itself needs a null check too: option<T>() returns
    // nullptr for an absent key AND for a type mismatch, and a stale-header
    // build once made this defaults() lookup return null — the bare ->value
    // deref then compiled to a ud2 trap (87fa2fa third-party 3MF, 2026-10-21,
    // load-stage SIGSEGV). Last resort is the PrintConfig default (0.2),
    // surfaced as a warning so the substituted height stays attributable.
    const auto* global_lh = m_config.option<ConfigOptionFloat>("layer_height");
    double default_layer_height = 0.2;
    if (global_lh)
    {
        default_layer_height = global_lh->value;
    }
    else
    {
        const auto* fallback = FullPrintConfig::defaults().option<ConfigOptionFloat>("layer_height");
        if (fallback)
        {
            default_layer_height = fallback->value;
        }
        else
        {
            BOOST_LOG_TRIVIAL(error) << "layer_height resolvable from neither the 3MF config nor "
                                        "FullPrintConfig::defaults(); height-range sanitizer falls "
                                        "back to 0.2";
            m_stats.issues.push_back(make_warning(
                -1, "CONFIG_DEFAULT_UNAVAILABLE",
                "Default layer height could not be resolved from engine defaults; height ranges "
                "missing layer_height are set to 0.2mm"));
        }
    }

    for (ModelObject* obj : m_model.objects)
    {
        if (obj->layer_config_ranges.empty())
            continue;

        std::vector<std::string> problems;
        for (auto& [z_range, cfg] : obj->layer_config_ranges)
        {
            std::ostringstream where;
            where << "[" << z_range.first << "-" << z_range.second << "]";

            if (!cfg.has("layer_height"))
            {
                cfg.set("layer_height", default_layer_height);
                problems.push_back(where.str() + " missing layer_height, set to default " +
                                   std::to_string(default_layer_height));
            }
            for (const char* key : {"wall_filament", "sparse_infill_filament", "solid_infill_filament"})
                if (cfg.has(key))
                {
                    const int id = cfg.opt_int(key);
                    if (id < 0 || id > static_cast<int>(num_physical))
                        problems.push_back(where.str() + " " + key + "=" + std::to_string(id) +
                                           " is outside the physical filament range 1.." +
                                           std::to_string(num_physical) + "; it prints with extruder 1");
                }
        }

        if (!problems.empty())
        {
            std::string joined;
            for (size_t i = 0; i < problems.size(); ++i)
            {
                if (i > 0)
                    joined += "; ";
                joined += problems[i];
            }
            m_stats.issues.push_back(make_warning(
                -1, "CONFIG_INVALID_LAYER_RANGE",
                "Object \"" + obj->name + "\" height-range settings carry invalid values: " + joined));
        }
    }
}

// Does the project USE a mixed filament (as opposed to merely defining one)?
// MixedFilamentManager numbers the virtual mixed slots from num_physical+1,
// so any reference above the PHYSICAL filament count (filament_colour vector
// length) is a mixed-slot reference. Two reference paths are checked:
//   1. object / part level "extruder"
//   2. painted facet states (mmu_segmentation used_states, 1-based extruder)
// Height-range colour overrides (layer_config_ranges) are deliberately NOT
// checked: the GUI supports physical filaments only there, so a virtual id
// in that block is invalid hand-crafted data, not a real mixed-filament use.
// The 3MF importer already clamps dangling references at load
// (max_supported_filament_id_from_project_config), so any id that survives
// loading is backed by an enabled mixed_filament_definitions row — no
// re-validation against the definitions string is needed here.
bool SliceEngine::detect_mixed_filament_usage() const
{
    const auto* colours = m_config.option<ConfigOptionStrings>("filament_colour");
    const size_t num_physical = colours ? colours->values.size() : 0;
    if (num_physical == 0)
        return false;

    auto extruder_exceeds = [num_physical](const ModelConfig& cfg) {
        return cfg.has("extruder") && cfg.opt_int("extruder") > static_cast<int>(num_physical);
    };

    for (const ModelObject* obj : m_model.objects)
    {
        if (extruder_exceeds(obj->config))
            return true;
        for (const ModelVolume* vol : obj->volumes)
        {
            if (extruder_exceeds(vol->config))
                return true;
            // Painted facets: used_states[s] marks 1-based extruder state s in
            // use on this volume's mesh (persisted in the 3MF).
            const auto& used_states = vol->mmu_segmentation_facets.get_data().used_states;
            for (size_t s = static_cast<size_t>(EnforcerBlockerType::Extruder1);
                 s < used_states.size() && s <= static_cast<size_t>(EnforcerBlockerType::ExtruderMax); ++s)
                if (used_states[s] && s > num_physical)
                    return true;
        }
    }
    return false;
}

// ============================================================================
// Stage 1.1: Printer model validation (fail-fast, before preset loading)
// ============================================================================

bool SliceEngine::validate_printer_model()
{
    auto fail_model = [&](const std::string& code, const std::string& msg)
    {
        record_stage_error(EXIT_PREPROCESS_ERROR, code, msg);
        return false;
    };

    if (!m_config.has("printer_model"))
    {
        return fail_model("PRINTER_MODEL_MISSING", "Printer model is missing. Only the Snapmaker U1 is supported.");
    }

    std::string printer_model = m_config.opt_string("printer_model");
    if (printer_model != DEFAULT_PRINTER_MODEL)
    {
        return fail_model("PRINTER_MODEL_UNSUPPORTED",
                          "Unsupported printer model: \"" + printer_model + "\". Only the Snapmaker U1 is supported.");
    }

    return true;
}

// ============================================================================
// Stage 1.2: Config warning collection
// ============================================================================

void SliceEngine::collect_config_warnings()
{
    // Validate config values (layer_height, nozzle_diameter, etc.).
    // Use under_cli=false to match desktop GUI behavior — invalid config
    // values produce warnings but do NOT block slicing.
    std::map<std::string, std::string> invalid = m_config.validate(false);
    for (const auto& [key, msg] : invalid)
        m_stats.issues.push_back(make_warning(-1, "CONFIG_INVALID_" + key, msg));

    // Check config substitutions (unknown keys, forward-compat changes)
    if (!m_config_substitutions.empty())
    {
        for (const auto& substitution : m_config_substitutions.substitutions)
        {
            const char* key = substitution.opt_def ? substitution.opt_def->opt_key.c_str() : "?";
            m_stats.issues.push_back(make_warning(-1, "CONFIG_SUBSTITUTION",
                                                  std::string("Config key '") + key +
                                                      "' value was substituted (old: " + substitution.old_value + ")"));
        }

        for (const auto& key : m_config_substitutions.unrecogized_keys)
        {
            m_stats.issues.push_back(make_warning(-1, "CONFIG_UNRECOGNIZED",
                                                  std::string("Unrecognized config key '") + key +
                                                      "' — may be from a newer slicer version"));
        }
    }
}

// ============================================================================
// Stage 1.3: Load presets (system from vendor JSON, project-embedded from .3mf)
// ============================================================================

bool SliceEngine::validate_input()
{
    // Invariant: read_3mf_model() guarantees m_plate_data is non-empty —
    // either the 3MF carried <plate> records or a single plate was
    // synthesized (desktop parity for metadata-less files). An empty list
    // here is an internal regression; fail loudly rather than silently
    // looping over zero plates, which would exit 0 with no output.
    if (m_plate_data.empty())
    {
        BOOST_LOG_TRIVIAL(error) << "Internal invariant violated: no plate data after model load";
        m_any_error            = true;
        set_error_type(EXIT_PREPROCESS_ERROR);
        m_stats.error_message = "No plate data available after model load (internal error)";
        m_stats.issues.push_back(make_error(-1, "PLATE_DATA_MISSING", m_stats.error_message));
        return false;
    }

    return true;
}

// ============================================================================
// Stage 3: Global preprocessing (ensure on bed)
// ============================================================================

