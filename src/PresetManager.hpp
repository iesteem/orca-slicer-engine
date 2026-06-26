#pragma once

#include <memory>
#include <string>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "EngineContext.hpp"

namespace Slic3r {
    class DynamicPrintConfig;
}

class PresetManager {
public:
    explicit PresetManager(EngineContext& ctx);

    void load_system_presets();
    void validate_presets();
    bool validate_printer_model();
    void apply_printer_official_preset();
    void apply_printer_preset_config();
    bool validate_filament_official(bool enforce = true);
    bool has_inline_filament_config(int ext_idx);
    void substitute_filament_params(Slic3r::ConfigOptionStrings* filament_ids, int ext_idx,
                                     const Slic3r::Preset& official_parent,
                                     const std::string& original_name);
    void substitute_printer_params(const std::string& original_name,
                                    const std::string& parent_name);
    Slic3r::DynamicPrintConfig build_full_print_config();

    bool presets_available() const { return m_presets_available; }
    Slic3r::PresetBundle* preset_bundle() { return m_preset_bundle.get(); }

private:
    EngineContext& m_ctx;
    std::unique_ptr<Slic3r::PresetBundle> m_preset_bundle;
    bool m_presets_available = false;
    Slic3r::ConfigSubstitutionContext m_config_substitutions{
        Slic3r::ForwardCompatibilitySubstitutionRule::Enable};
};
