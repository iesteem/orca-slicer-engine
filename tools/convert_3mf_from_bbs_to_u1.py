#!/usr/bin/env python3
"""
3MF Printer Converter: Convert Bambu/Prusa 3MF files to Snapmaker U1 format.

Uses OrcaSlicer's built-in Snapmaker U1 profiles as the reference (source of truth)
for printer identity, valid schema keys, speed/accel ceilings, and G-code blocks.
Profile inheritance chains are resolved at startup so all values come from the
canonical JSON profiles — no hardcoded numbers.

Bundle 3MF files (MakerWorld collections wrapping raw STL / nested .3mf) are
auto-detected and unpacked: nested standard 3MFs are recursively converted,
STL files are extracted.
"""

import argparse
import io
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
import zipfile
from copy import deepcopy
from pathlib import Path

try:
    import yaml
except ImportError:
    yaml = None  # rules engine is optional


# ═══════════════════════════════════════════════════════════════════════════
#  Helpers
# ═══════════════════════════════════════════════════════════════════════════

def _12x(v):
    return [v] * 12

def _6x(v):
    return [v] * 6

def _norm(v):
    """Normalize Bambu-style nested arrays to scalar or simple list."""
    if isinstance(v, list) and len(v) == 1 and isinstance(v[0], list):
        return v[0]
    return v

def _to_float(value):
    """Parse an Orca setting string-or-number into float; None if unparseable."""
    if value is None or isinstance(value, bool):
        return None
    try:
        return float(value)
    except (ValueError, TypeError):
        return None


# ═══════════════════════════════════════════════════════════════════════════
#  Profile Loader — resolves JSON inheritance chains from OrcaSlicer profiles
# ═══════════════════════════════════════════════════════════════════════════

def _resolve_profile(profiles_dir, profile_type, name):
    """Load a JSON profile and resolve its full ``inherits`` chain.

    Returns a merged dict:  root → … → parent → child  (child wins).
    ``profiles_dir`` is the vendor root, e.g. resources/profiles/Snapmaker.
    ``profile_type`` is the subdirectory: "machine" or "process".
    """
    chain = []
    seen = set()
    current = name
    while current:
        if current in seen:
            break
        seen.add(current)
        path = os.path.join(profiles_dir, profile_type, f"{current}.json")
        if not os.path.exists(path):
            break
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        chain.append(data)
        current = data.get("inherits")
    # Merge root-first, later entries override
    merged = {}
    for d in reversed(chain):
        merged.update(d)
    return merged


def _load_reference(profiles_dir, machine_name, process_name):
    """Load resolved machine + process profiles as the U1 reference.

    Returns ``(machine_cfg, process_cfg)`` — plain dicts with all
    ``inherits`` chains already merged.
    """
    if not os.path.isdir(profiles_dir):
        raise SystemExit(f"Profiles directory not found: {profiles_dir}")
    machine = _resolve_profile(profiles_dir, "machine", machine_name)
    process = _resolve_profile(profiles_dir, "process", process_name)
    if not machine:
        raise SystemExit(f"Failed to resolve machine profile: {machine_name}")
    if not process:
        raise SystemExit(f"Failed to resolve process profile: {process_name}")
    return machine, process


# ═══════════════════════════════════════════════════════════════════════════
#  Identity keys — must come from the U1 reference profile, never from source
# ═══════════════════════════════════════════════════════════════════════════

_IDENTITY_KEYS = frozenset({
    "printer_model", "printer_settings_id", "printer_variant",
    "printable_area", "printable_height", "bed_exclude_area",
    "nozzle_diameter", "nozzle_type", "nozzle_volume",
    "extruder_count", "compatible_printers", "compatible_printers_condition",
    "version", "print_settings_id",
})

# G-code keys — replaced wholesale from reference
_GCODE_KEYS = (
    "machine_start_gcode", "machine_end_gcode",
    "before_layer_change_gcode", "layer_change_gcode",
    "change_filament_gcode", "machine_pause_gcode",
    "template_custom_gcode", "printing_by_object_gcode",
    "time_lapse_gcode",
)

# Keys eligible for numeric ceiling clamping (from process profile)
_CLAMP_KEYS = frozenset({
    "outer_wall_speed", "inner_wall_speed", "sparse_infill_speed",
    "internal_solid_infill_speed", "top_surface_speed", "gap_infill_speed",
    "travel_speed", "bridge_speed", "support_speed", "support_interface_speed",
    "initial_layer_speed", "initial_layer_infill_speed", "skirt_speed",
    "ironing_speed", "overhang_1_4_speed", "overhang_2_4_speed",
    "overhang_3_4_speed", "overhang_4_4_speed",
    "default_acceleration", "outer_wall_acceleration", "inner_wall_acceleration",
    "sparse_infill_acceleration", "top_surface_acceleration",
    "travel_acceleration", "initial_layer_acceleration", "bridge_acceleration",
    "internal_solid_infill_acceleration",
})

# Enum remaps — Bambu values Orca doesn't recognise
_VALUE_REMAP = {
    "ensure_vertical_shell_thickness": {"disabled": "none"},
    "dont_filter_internal_bridges": {"disabled": "none"},
    "enable_extra_bridge_layer": {"disabled": "none"},
}


# ═══════════════════════════════════════════════════════════════════════════
#  YAML Filament Rules Engine
# ═══════════════════════════════════════════════════════════════════════════

def _load_rules(rules_dir):
    """Load all .yaml/.yml rule files from *rules_dir*.

    Returns a list of rule dicts sorted by ascending priority.
    """
    if not yaml or not os.path.isdir(rules_dir):
        return []
    rules = []
    for fn in sorted(os.listdir(rules_dir)):
        if not fn.endswith((".yaml", ".yml")):
            continue
        path = os.path.join(rules_dir, fn)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = yaml.safe_load(fh)
            if isinstance(data, dict) and data.get("enabled", True):
                data.setdefault("priority", 0)
                data["_source"] = fn
                rules.append(data)
        except Exception:
            pass
    rules.sort(key=lambda r: r.get("priority", 0))
    return rules


def _rule_matches_filament(rule, settings):
    """Check if *rule* matches the filament identity in *settings*.

    All specified match fields must be true (AND logic).
    """
    match = rule.get("match", rule)  # support both "match" key and top-level
    if not match:
        return False, {}

    # --- filament_settings_id_contains (case-insensitive substring) ---
    substr = match.get("filament_settings_id_contains", "")
    if substr:
        fids = settings.get("filament_settings_id", [])
        if isinstance(fids, str):
            fids = [fids]
        found = False
        for fid in fids:
            if substr.lower() in str(fid).lower():
                found = True
                break
        if not found:
            return False, {}

    # --- filament_vendor (exact) ---
    vendor = match.get("filament_vendor", "")
    if vendor:
        fv = settings.get("filament_vendor", [])
        if isinstance(fv, str):
            fv = [fv]
        if vendor not in fv:
            return False, {}

    # --- filament_type (exact) ---
    ftype = match.get("filament_type", "")
    if ftype:
        ft = settings.get("filament_type", [])
        if isinstance(ft, str):
            ft = [ft]
        if ftype not in ft:
            return False, {}

    # --- base_profile_contains (substring in print_settings_id) ---
    base = match.get("base_profile_contains", "")
    if base:
        psid = settings.get("print_settings_id", "")
        if base.lower() not in str(psid).lower():
            return False, {}

    return True, {"matched": {k: v for k, v in match.items() if v}}


def _apply_rules(settings, rules):
    """Apply matching filament rules to *settings*.

    Rules are applied in priority order; later (higher-priority) rules
    win on conflicting keys.  Numeric override values are coerced to
    strings because Orca settings are always JSON strings.
    """
    if not rules:
        return
    applied = []
    for rule in rules:
        ok, evidence = _rule_matches_filament(rule, settings)
        if not ok:
            continue
        overrides = rule.get("overrides", {})
        for key, val in overrides.items():
            if isinstance(val, (int, float)) and not isinstance(val, bool):
                val = str(val)
            if isinstance(val, list):
                val = [str(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else v for v in val]
            settings[key] = deepcopy(val)
        applied.append({"rule": rule.get("name", rule.get("_source", "?")),
                        "priority": rule.get("priority", 0),
                        "overrides": list(overrides.keys())})
    return applied


# ═══════════════════════════════════════════════════════════════════════════
#  Core Conversion
# ═══════════════════════════════════════════════════════════════════════════

def _filter_to_schema(source, schema_keys, *, keep=frozenset()):
    """Return *source* with keys not in *schema_keys* removed.

    Identity keys and *keep* keys are always preserved.
    """
    allowed = set(schema_keys) | set(_IDENTITY_KEYS) | set(keep)
    result = {}
    dropped = []
    for k, v in source.items():
        if k in allowed:
            result[k] = v
        else:
            dropped.append(k)
    return result, dropped


def _clamp_numeric_ceilings(settings, reference):
    """Clamp speed / acceleration values to U1 reference ceilings.

    If a value is 0 or -1 (Bambu sentinel for "inherit"), replace with
    the reference value.  Otherwise enforce the ceiling.
    """
    events = []
    for key in _CLAMP_KEYS:
        if key not in settings or key not in reference:
            continue
        cur = settings[key]
        ref = reference[key]

        if isinstance(cur, list) and isinstance(ref, list):
            out = []
            for i, cv in enumerate(cur):
                rv = ref[min(i, len(ref) - 1)]
                cf = _to_float(cv)
                rf = _to_float(rv)
                if cf is not None and rf is not None and rf > 0:
                    if cf <= 0:
                        out.append(rv)
                        events.append(f"  {key}[{i}]: {cv} -> {rv} (inherit → reference)")
                    elif cf > rf:
                        out.append(rv)
                        events.append(f"  {key}[{i}]: {cv} -> {rv} (capped)")
                    else:
                        out.append(cv)
                else:
                    out.append(cv)
            settings[key] = out
        else:
            cf = _to_float(cur)
            rf = _to_float(ref)
            if cf is not None and rf is not None and rf > 0:
                if cf <= 0:
                    settings[key] = ref
                    events.append(f"  {key}: {cur} -> {ref} (inherit → reference)")
                elif cf > rf:
                    settings[key] = ref
                    events.append(f"  {key}: {cur} -> {ref} (capped)")

    return events


def _overlay_reference_defaults(settings, reference):
    """For keys in *reference* that are missing from *settings*, add defaults."""
    added = []
    for k, v in reference.items():
        if k not in settings and not k.startswith("_"):
            # Skip identity keys — they are handled separately
            if k in _IDENTITY_KEYS:
                continue
            settings[k] = deepcopy(v)
            added.append(k)
    return added


# ── Snapmaker U1 Official Filament Preset Names ─────────────────────────

_SNAPMK_U1_FILAMENTS_BY_TYPE = {
    ("pla", "basic"):         "Snapmaker PLA Basic @U1",
    ("pla", "matte"):         "Snapmaker PLA Matte @U1",
    ("pla", "silk"):          "Snapmaker PLA Silk @U1",
    ("pla", "eco"):           "Snapmaker PLA Eco @U1",
    ("pla", "wood"):          "Snapmaker PLA Wood @U1",
    ("pla", "metal"):         "Snapmaker PLA Metal @U1",
    ("pla", "marble"):        "Snapmaker PLA Marble @U1",
    ("pla", "glow"):          "Snapmaker PLA Glow @U1",
    ("pla", "snapspeed"):     "Snapmaker PLA SnapSpeed @U1",
    ("pla", "high speed"):    "Snapmaker PLA SnapSpeed @U1",
    ("pla", "cf"):            "Snapmaker PLA-CF @U1",
    ("pla", "carbon fiber"):  "Snapmaker PLA-CF @U1",
    ("pla", "full spectrum"): "Snapmaker PLA Full Spectrum @U1",
    ("pla", None):            "Snapmaker PLA @U1",
    ("petg", "cf"):           "Snapmaker PETG-CF @U1",
    ("petg", "carbon fiber"): "Snapmaker PETG-CF @U1",
    ("petg", None):           "Snapmaker PETG @U1",
    ("pet", None):            "Snapmaker PET @U1",
    ("abs", None):            "Snapmaker ABS @U1",
    ("asa", None):            "Snapmaker ASA @U1",
    ("tpu", "95a"):           "Snapmaker TPU 95A @U1",
    ("tpu", "90a"):           "Snapmaker TPU 90A @U1",
    ("tpu", "hf"):            "Snapmaker TPU High-Flow @U1",
    ("tpu", "high flow"):     "Snapmaker TPU High-Flow @U1",
    ("tpu", None):            "Snapmaker TPU @U1",
    ("tpe", None):            "Snapmaker TPE @U1",
    ("pa", "cf"):             "Snapmaker PA-CF @U1",
    ("pa", "carbon fiber"):   "Snapmaker PA-CF @U1",
    ("pa", None):             "Generic PA @System",
    ("pc", None):             "Generic PC @System",
    ("pva", None):            "Snapmaker PVA @U1",
    ("support", "pla"):       "Snapmaker Breakaway Support For PLA @U1",
    ("support", None):        "Snapmaker Breakaway Support For PLA @U1",
}

_KEYWORD_TO_SNAPMK_FILAMENT = {
    "PLA":       "Snapmaker PLA @U1",
    "PETG":      "Snapmaker PETG @U1",
    "PET":       "Snapmaker PET @U1",
    "ABS":       "Snapmaker ABS @U1",
    "ASA":       "Snapmaker ASA @U1",
    "TPU":       "Snapmaker TPU @U1",
    "TPE":       "Snapmaker TPE @U1",
    "PA":        "Generic PA @System",
    "NYLON":     "Generic PA @System",
    "PC":        "Generic PC @System",
    "PVA":       "Snapmaker PVA @U1",
    "SUPPORT":   "Snapmaker Breakaway Support For PLA @U1",
}

_SUBTYPE_KEYWORDS = [
    "basic", "matte", "silk", "eco", "wood", "metal", "marble", "glow",
    "snapspeed", "high speed", "cf", "carbon fiber", "full spectrum",
    "95a", "90a", "hf", "high flow",
]

_SNAPMK_OFFICIAL_FILAMENT_NAMES = set(
    _SNAPMK_U1_FILAMENTS_BY_TYPE.values()
) | {"Generic PLA @System", "Generic PETG @System", "Generic ABS @System",
     "Generic ASA @System", "Generic PA @System", "Generic PC @System",
     "Generic TPU @System", "Generic PVA @System", "Generic Support @System"}


def _get_nozzle_suffix(proj):
    """Extract nozzle size suffix from project settings.

    Returns a string like '0.4 nozzle' if the 3MF has nozzle_diameter set.
    Only uses the first nozzle diameter value (the most common case).
    """
    nd = proj.get("nozzle_diameter")
    if isinstance(nd, list) and len(nd) > 0:
        nozzle_val = str(nd[0]).strip()
        # Strip trailing 'mm' if present, then re-add space + nozzle
        nozzle_val = re.sub(r'\s*mm$', '', nozzle_val, flags=re.IGNORECASE)
        if nozzle_val:
            return f"{nozzle_val} nozzle"
    return None


def _append_nozzle_suffix(name, suffix):
    """Append nozzle suffix to a filament preset name if not already present."""
    if not isinstance(name, str) or not name.strip():
        return name
    if suffix in name:
        return name
    return f"{name} {suffix}"


def _append_nozzle_suffix_if_exists(name, suffix, profiles_dir=None):
    """Append nozzle suffix only if the resulting preset file exists."""
    if not isinstance(name, str) or not name.strip():
        return name
    if suffix in name:
        return name
    candidate = f"{name} {suffix}"
    if profiles_dir and os.path.isdir(profiles_dir):
        candidate_file = os.path.join(profiles_dir, f"{candidate}.json")
        if os.path.exists(candidate_file):
            return candidate
    # Check default Snapmaker filament dir relative to this script
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_filament_dir = os.path.join(
        script_dir, "..", "package", "resources", "profiles", "Snapmaker", "filament")
    if os.path.isdir(default_filament_dir):
        if os.path.exists(os.path.join(default_filament_dir, f"{candidate}.json")):
            return candidate
    # No per-nozzle variant exists — keep the base name
    return name


def _remap_filament_name(name):
    if not isinstance(name, str) or not name.strip():
        return name
    # Strip Bambu annotations
    name = re.sub(r"\([^)]*\.3mf\)$", "", name).strip()
    name = re.sub(r"\s*@BBL\s+\S+$", "", name).strip()
    name = re.sub(r"\s*@Prusa\s+\S+$", "", name).strip()
    upper = name.upper()
    material = None
    for key in ("PLA", "PETG", "PET", "ABS", "ASA", "TPU", "TPE",
                 "PA", "PC", "PVA", "NYLON", "SUPPORT"):
        if key in upper:
            material = key
            break
    if material is None:
        return name
    if material == "NYLON":
        material = "PA"
    if material == "PET":
        material = "PETG"
    subtype = None
    lower = name.lower()
    for kw in _SUBTYPE_KEYWORDS:
        if kw in lower:
            subtype = kw
            break
    if material == "PLA" and "PLA-CF" in upper:
        material, subtype = "PLA", "cf"
    if material == "PETG" and "PETG-CF" in upper:
        material, subtype = "PETG", "cf"
    key = (material.lower(), subtype)
    if key in _SNAPMK_U1_FILAMENTS_BY_TYPE:
        return _SNAPMK_U1_FILAMENTS_BY_TYPE[key]
    key = (material.lower(), None)
    if key in _SNAPMK_U1_FILAMENTS_BY_TYPE:
        return _SNAPMK_U1_FILAMENTS_BY_TYPE[key]
    if material in _KEYWORD_TO_SNAPMK_FILAMENT:
        return _KEYWORD_TO_SNAPMK_FILAMENT[material]
    return name


def count_filaments(proj):
    n = 1
    for key, val in proj.items():
        if isinstance(val, list) and len(val) > n:
            if all(isinstance(x, str) for x in val):
                n = max(n, len(val))
    return min(n, 12)


def convert_project_settings(proj, machine_cfg, process_cfg, rules=None):
    """Convert Bambu/Prusa project settings to Snapmaker U1.

    Uses the resolved *machine_cfg* and *process_cfg* as the U1 reference
    (source of truth).  Steps:

    1. Replace identity keys from machine_cfg
    2. Swap G-code blocks from machine_cfg
    3. Filter out keys not in U1 schema (process_cfg + machine_cfg)
    4. Clamp speed / accel to process_cfg ceilings
    5. Overlay U1 defaults from process_cfg
    6. Apply enum value remaps
    7. Apply filament rules (if any)
    8. Remap filament names to Snapmaker official presets
    """
    result = deepcopy(proj)
    n_fil = count_filaments(proj)

    # Build the U1 schema (all valid keys)
    schema_keys = set(process_cfg.keys()) | set(machine_cfg.keys())

    # --- Step 1: Identity swap ---
    for key in _IDENTITY_KEYS:
        if key in machine_cfg:
            result[key] = deepcopy(machine_cfg[key])
    # Use the machine profile name as the printer_settings_id (more reliable
    # than the raw "printer_settings_id" field which may contain dev values).
    # Append " (converted)" so Orca doesn't match an existing library profile.
    machine_name = machine_cfg.get("name", "")
    if machine_name:
        result["printer_settings_id"] = machine_name + " (converted)"

    # --- Step 2: G-code swap ---
    for key in _GCODE_KEYS:
        if key in machine_cfg:
            result[key] = machine_cfg[key]

    # --- Step 3: Filter out non-U1-schema keys ---
    # Keys that should always be preserved (filament identity keys live in
    # project_settings but come from filament profiles, not process/machine).
    _always_keep = {
        "custom_gcode_per_layer", "internal_bridge_support_thickness",
        # filament identity (needed for rules matching + name remapping)
        "filament_type", "filament_vendor", "filament_settings_id",
        "filament_cost", "filament_density", "filament_diameter",
        "filament_is_support", "filament_soluble",
        # filament temperature (needed for rules + general use)
        "filament_flow_ratio", "filament_max_volumetric_speed",
        "filament_minimal_purge_on_wipe_tower",
        "nozzle_temperature", "nozzle_temperature_initial_layer",
        "nozzle_temperature_range_low", "nozzle_temperature_range_high",
        "hot_plate_temp", "hot_plate_temp_initial_layer",
        "cool_plate_temp", "cool_plate_temp_initial_layer",
        "eng_plate_temp", "eng_plate_temp_initial_layer",
        "textured_plate_temp", "textured_plate_temp_initial_layer",
        "supertack_plate_temp", "supertack_plate_temp_initial_layer",
        "graphic_effect_plate_temp", "graphic_effect_plate_temp_initial_layer",
        "textured_cool_plate_temp", "textured_cool_plate_temp_initial_layer",
        "chamber_temperature", "temperature_vitrification",
        # filament retraction
        "filament_retraction_length", "filament_retraction_speed",
        "filament_retraction_minimum_travel", "filament_z_hop",
        "filament_z_hop_types", "filament_retract_when_changing_layer",
        "filament_retract_before_wipe", "filament_wipe", "filament_wipe_distance",
        "filament_retract_restart_extra", "filament_deretraction_speed",
        "filament_retract_lift_above", "filament_retract_lift_below",
        "filament_retract_lift_enforce", "filament_retraction_distances_when_cut",
        "filament_long_retractions_when_cut",
        # filament cooling
        "filament_cooling_initial_speed", "filament_cooling_final_speed",
        "filament_cooling_moves",
        "fan_max_speed", "fan_min_speed", "fan_cooling_layer_time",
        "slow_down_for_layer_cooling", "slow_down_layer_time", "slow_down_min_speed",
        "close_fan_the_first_x_layers", "full_fan_speed_layer",
        "enable_overhang_bridge_fan", "overhang_fan_speed", "overhang_fan_threshold",
        "reduce_fan_stop_start_freq",
        # filament start/end gcode
        "filament_start_gcode", "filament_end_gcode", "filament_notes",
        # filament advance/pressure
        "pressure_advance", "enable_pressure_advance",
        # filament loading/unloading
        "filament_loading_speed", "filament_loading_speed_start",
        "filament_unloading_speed", "filament_unloading_speed_start",
        "filament_multitool_ramming", "filament_multitool_ramming_flow",
        "filament_multitool_ramming_volume", "filament_ramming_parameters",
        "filament_toolchange_delay", "filament_stamping_distance",
        "filament_stamping_loading_speed",
        # filament other
        "filament_shrink", "filament_shrinkage_compensation_z",
        "filament_colour", "default_filament_colour",
        "idle_temperature", "required_nozzle_HRC",
        "support_filament", "support_interface_filament",
        "activate_air_filtration", "activate_chamber_temp_control",
        "during_print_exhaust_fan_speed", "complete_print_exhaust_fan_speed",
        "additional_cooling_fan_speed",
        "pellet_flow_coefficient", "dont_slow_down_outer_wall",
        "internal_bridge_fan_speed", "ironing_fan_speed",
        "support_material_interface_fan_speed",
    }
    result, dropped = _filter_to_schema(result, schema_keys, keep=frozenset(_always_keep))

    # --- Step 4: Overlay reference defaults (fill gaps) ---
    added = _overlay_reference_defaults(result, process_cfg)
    # Also overlay machine_cfg defaults that aren't identity or gcode
    for k, v in machine_cfg.items():
        if k not in result and k not in _IDENTITY_KEYS and k not in _GCODE_KEYS and not k.startswith("_"):
            result[k] = deepcopy(v)

    # --- Step 5: Enum value remaps ---
    for key, remap in _VALUE_REMAP.items():
        if key in result and result[key] in remap:
            result[key] = remap[result[key]]

    # --- Step 6: Speed / accel clamping ---
    clamp_events = _clamp_numeric_ceilings(result, process_cfg)

    # --- Step 7: Force U1-specific values ---
    result["wipe_tower_wall_type"] = "rectangle"
    result.pop("inherits", None)
    result.pop("inherits_group", None)
    if n_fil <= 1:
        result["support_filament"] = "1"
        result["support_interface_filament"] = "1"
        if "enable_prime_tower" in result:
            result["enable_prime_tower"] = "0"
        if "prime_volume" in result:
            result["prime_volume"] = "0"

    # --- Step 8: Apply filament rules (BEFORE name remapping, since
    # rules match on original Bambu filament_type/vendor values) ---
    rule_applied = []
    if rules:
        rule_applied = _apply_rules(result, rules)

    # --- Step 9: Remap filament names to Snapmaker official presets ---
    for key in ("filament_type", "filament_settings_id", "default_filament_colour",
                "filament_colour", "support_filament", "support_interface_filament"):
        if key in result:
            val = result[key]
            if isinstance(val, str):
                result[key] = _remap_filament_name(val)
            elif isinstance(val, list):
                result[key] = [_remap_filament_name(v) for v in val]

    # --- Step 9b: Append nozzle suffix to filament_settings_id ---
    # Only add nozzle suffix if the system preset with that exact name
    # actually exists.  Some filament families (e.g. "Snapmaker PLA @U1")
    # use nozzles inherited from base presets and do NOT have per-nozzle
    # variant files.
    nozzle_suffix = _get_nozzle_suffix(result)
    if nozzle_suffix and "filament_settings_id" in result:
        fids = result["filament_settings_id"]
        if isinstance(fids, list):
            result["filament_settings_id"] = [
                _append_nozzle_suffix_if_exists(v, nozzle_suffix)
                for v in fids
            ]

    return result, dropped, clamp_events, rule_applied, added


def convert_slice_info(xml_str, machine_cfg):
    """Update slice_info.config XML for Snapmaker U1."""
    printer_model = machine_cfg.get("printer_model", "Snapmaker U1")
    try:
        root = ET.fromstring(xml_str)
        for plate in root.findall(".//plate"):
            for meta in plate.findall("metadata"):
                if meta.get("key") == "printer_model_id":
                    meta.set("value", printer_model)
        for warning in root.findall(".//warning"):
            parent = warning.find("..")
            if parent is not None:
                parent.remove(warning)
        return ET.tostring(root, encoding="unicode")
    except ET.ParseError:
        return xml_str


# ═══════════════════════════════════════════════════════════════════════════
#  Filament Settings Generation
# ═══════════════════════════════════════════════════════════════════════════

_FILAMENT_PER_EXTRUDER_KEYS = [
    "activate_air_filtration", "activate_chamber_temp_control",
    "additional_cooling_fan_speed", "chamber_temperature",
    "close_fan_the_first_x_layers", "complete_print_exhaust_fan_speed",
    "cool_plate_temp", "cool_plate_temp_initial_layer",
    "default_filament_colour", "dont_slow_down_outer_wall",
    "during_print_exhaust_fan_speed", "enable_overhang_bridge_fan",
    "enable_pressure_advance", "eng_plate_temp", "eng_plate_temp_initial_layer",
    "fan_cooling_layer_time", "fan_max_speed", "fan_min_speed",
    "filament_cooling_final_speed", "filament_cooling_initial_speed",
    "filament_cooling_moves", "filament_cost", "filament_density",
    "filament_deretraction_speed", "filament_diameter",
    "filament_end_gcode", "filament_flow_ratio", "filament_is_support",
    "filament_loading_speed", "filament_loading_speed_start",
    "filament_long_retractions_when_cut", "filament_max_volumetric_speed",
    "filament_minimal_purge_on_wipe_tower", "filament_multitool_ramming",
    "filament_multitool_ramming_flow", "filament_multitool_ramming_volume",
    "filament_notes", "filament_ramming_parameters",
    "filament_retract_before_wipe", "filament_retract_length_toolchange",
    "filament_retract_lift_above", "filament_retract_lift_below",
    "filament_retract_lift_enforce", "filament_retract_restart_extra",
    "filament_retract_restart_extra_toolchange",
    "filament_retract_when_changing_layer",
    "filament_retraction_distances_when_cut", "filament_retraction_length",
    "filament_retraction_minimum_travel", "filament_retraction_speed",
    "filament_settings_id", "filament_shrink",
    "filament_shrinkage_compensation_z", "filament_soluble",
    "filament_stamping_distance", "filament_stamping_loading_speed",
    "filament_start_gcode", "filament_toolchange_delay", "filament_type",
    "filament_unloading_speed", "filament_unloading_speed_start",
    "filament_vendor", "filament_wipe", "filament_wipe_distance",
    "filament_z_hop", "filament_z_hop_types", "full_fan_speed_layer",
    "hot_plate_temp", "hot_plate_temp_initial_layer", "idle_temperature",
    "internal_bridge_fan_speed", "ironing_fan_speed",
    "nozzle_temperature", "nozzle_temperature_initial_layer",
    "nozzle_temperature_range_high", "nozzle_temperature_range_low",
    "overhang_fan_speed", "overhang_fan_threshold",
    "pellet_flow_coefficient", "pressure_advance",
    "reduce_fan_stop_start_freq", "required_nozzle_HRC",
    "slow_down_for_layer_cooling", "slow_down_layer_time",
    "slow_down_min_speed", "supertack_plate_temp",
    "supertack_plate_temp_initial_layer",
    "support_material_interface_fan_speed", "temperature_vitrification",
    "textured_cool_plate_temp", "textured_cool_plate_temp_initial_layer",
    "textured_plate_temp", "textured_plate_temp_initial_layer",
]


def generate_filament_settings(proj, n_fil, official_names):
    filament_settings_list = []
    for i in range(n_fil):
        filament_name = ""
        if i < len(official_names):
            filament_name = official_names[i]
        else:
            filament_name = official_names[-1] if official_names else ""
        if isinstance(filament_name, list):
            filament_name = filament_name[0] if filament_name else ""
        if isinstance(filament_name, str):
            filament_name = filament_name.strip()
        if not filament_name:
            continue
        if filament_name in _SNAPMK_OFFICIAL_FILAMENT_NAMES:
            continue
        fs = {
            "name": filament_name,
            "from": "project",
            "inherits": filament_name,
            "filament_settings_id": [filament_name],
            "filament_type": ["PLA"],
            "filament_vendor": ["Snapmaker"],
            "version": "02.03.01.00",
        }
        has_overrides = False
        for key in _FILAMENT_PER_EXTRUDER_KEYS:
            if key not in proj:
                continue
            val = proj[key]
            if isinstance(val, list) and len(val) > i:
                elem = val[i]
            elif isinstance(val, list) and len(val) == 1:
                elem = val[0]
            elif isinstance(val, str):
                elem = val
            else:
                continue
            if elem == "nil" or elem is None:
                continue
            if isinstance(elem, str) and elem == "" and key not in (
                "filament_end_gcode", "filament_start_gcode", "filament_notes"):
                continue
            fs[key] = [str(elem)]
            has_overrides = True
        if has_overrides:
            filament_settings_list.append(fs)
    return filament_settings_list


# ═══════════════════════════════════════════════════════════════════════════
#  Validation
# ═══════════════════════════════════════════════════════════════════════════

def validate_converted_3mf(zf_or_path, machine_cfg):
    import contextlib
    printer_model = machine_cfg.get("printer_model", "Snapmaker U1")
    warnings = []
    is_valid = True

    @contextlib.contextmanager
    def _open_zf():
        if isinstance(zf_or_path, zipfile.ZipFile):
            yield zf_or_path
        else:
            with zipfile.ZipFile(zf_or_path, "r") as zf:
                yield zf

    with _open_zf() as zf:
        names = zf.namelist()
        if "Metadata/project_settings.config" not in names:
            warnings.append("CRITICAL: Missing project_settings.config")
            return False, warnings

        proj = json.loads(zf.read("Metadata/project_settings.config"))
        model = proj.get("printer_model", "")
        if model != printer_model:
            warnings.append(f"CRITICAL: printer_model is '{model}', expected '{printer_model}'")
            is_valid = False
        area = proj.get("printable_area", [])
        ref_area = machine_cfg.get("printable_area", [])
        if ref_area and area != ref_area:
            warnings.append(f"CRITICAL: printable_area mismatch, expected {ref_area}")
            is_valid = False
        height = proj.get("printable_height", "")
        ref_height = machine_cfg.get("printable_height", "")
        if ref_height and height != ref_height:
            warnings.append(f"CRITICAL: printable_height is '{height}', expected '{ref_height}'")
            is_valid = False

        filament_ids = proj.get("filament_settings_id", [])
        if isinstance(filament_ids, str):
            filament_ids = [filament_ids]
        for idx, fid in enumerate(filament_ids):
            if not fid or not fid.strip():
                warnings.append(f"WARNING: extruder {idx+1} filament_settings_id is empty")
            elif fid not in _SNAPMK_OFFICIAL_FILAMENT_NAMES:
                warnings.append(f"WARNING: extruder {idx+1} filament '{fid}' is not a recognized Snapmaker U1 preset")

        for name in names:
            if name.startswith("Metadata/filament_settings_") and name.endswith(".config"):
                try:
                    fs = json.loads(zf.read(name))
                    inh = fs.get("inherits", "")
                    if isinstance(inh, str) and (not inh or not inh.strip()):
                        warnings.append(f"CRITICAL: {name} has empty inherits")
                        is_valid = False
                except (json.JSONDecodeError, UnicodeDecodeError):
                    warnings.append(f"WARNING: {name} could not be parsed")

    return is_valid, warnings


# ═══════════════════════════════════════════════════════════════════════════
#  Bundle 3MF handling — MakerWorld collections wrapping raw files
# ═══════════════════════════════════════════════════════════════════════════

def is_bundle_3mf(input_path):
    with zipfile.ZipFile(input_path, "r") as zf:
        for n in zf.namelist():
            if ".rels" in n:
                return False
    return True


def unpack_bundle(input_path, output_dir, machine_cfg, process_cfg, rules):
    """Unpack a bundle 3MF and convert nested standard 3MFs, extract STLs."""
    results = []
    bundle_stem = Path(input_path).stem
    os.makedirs(output_dir, exist_ok=True)
    nested_3mf_found = False
    stl_found = False

    with zipfile.ZipFile(input_path, "r") as zf:
        names = zf.namelist()

        # Phase 1: nested standard 3MFs → recursively convert
        for name in names:
            if not name.endswith(".3mf"):
                continue
            data = zf.read(name)
            try:
                inner = zipfile.ZipFile(io.BytesIO(data), "r")
                has_rels = any(".rels" in n for n in inner.namelist())
                inner.close()
                if has_rels:
                    nested_3mf_found = True
                    nested_stem = Path(name).stem
                    safe_stem = re.sub(r'[<>:"/\\|?*]', '_', nested_stem)
                    out_name = f"{safe_stem}-U1.3mf"
                    out_path = os.path.join(output_dir, out_name)
                    tmp_path = os.path.join(output_dir, f"_tmp_{safe_stem}.3mf")
                    with open(tmp_path, "wb") as f:
                        f.write(data)
                    try:
                        info = convert_3mf(tmp_path, out_path, machine_cfg, process_cfg, rules)
                        info["file"] = out_name
                        results.append(info)
                    except Exception as e:
                        results.append({"file": os.path.basename(name), "error": str(e)})
                    finally:
                        if os.path.exists(tmp_path):
                            os.remove(tmp_path)
            except Exception:
                pass

        # Phase 2: unsupported (STL extraction disabled — only 3MF is needed)
        if not nested_3mf_found:
            other_exts = set()
            for name in names:
                if "." in name and not name.endswith("/"):
                    ext = os.path.splitext(name)[1].lower()
                    if ext not in (".3mf", ".stl"):
                        other_exts.add(ext)
            other_str = ", ".join(sorted(other_exts)) if other_exts else "no geometry files"
            results.append({"file": os.path.basename(input_path),
                            "error": f"bundle contains {other_str} — cannot import"})

    return results


# ═══════════════════════════════════════════════════════════════════════════
#  Single-file conversion
# ═══════════════════════════════════════════════════════════════════════════

def convert_3mf(input_path, output_path, machine_cfg, process_cfg, rules=None):
    """Convert a single standard 3MF file to Snapmaker U1 format."""
    info = {"file": os.path.basename(input_path), "changes": []}

    with zipfile.ZipFile(input_path, "r") as zf:
        names = zf.namelist()
        has_proj = "Metadata/project_settings.config" in names
        if has_proj:
            proj = json.loads(zf.read("Metadata/project_settings.config"))
            old_printer = proj.get("printer_model", "<<none>>")
            info["original_printer"] = old_printer
            patched, dropped, clamp_evts, rule_applied, added = \
                convert_project_settings(proj, machine_cfg, process_cfg, rules)
            info["changes"].append(f"printer_model: {old_printer} -> {machine_cfg.get('printer_model', 'Snapmaker U1')}")
            if dropped:
                info["changes"].append(f"dropped {len(dropped)} Bambu-specific keys")
            if clamp_evts:
                info["changes"].append(f"clamped {len(clamp_evts)} speed/accel values")
            if rule_applied:
                for ra in rule_applied:
                    info["changes"].append(f"applied rule '{ra['rule']}' ({', '.join(ra['overrides'])})")
        else:
            patched = None
            info["original_printer"] = "<<missing>>"
            info["changes"].append("no project_settings.config in source")

        has_si = "Metadata/slice_info.config" in names
        if has_si:
            si_xml = zf.read("Metadata/slice_info.config").decode("utf-8")
            new_si = convert_slice_info(si_xml, machine_cfg)
            if new_si != si_xml:
                info["changes"].append("updated slice_info.config")

        n_fil = count_filaments(patched) if patched else 1
        official_names = patched.get("filament_settings_id", []) if patched else []
        if isinstance(official_names, str):
            official_names = [official_names]

        has_fil_settings = any(
            n.startswith("Metadata/filament_settings_") and n.endswith(".config")
            for n in names
        )

        with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as zf_out:
            skip = set()
            if has_proj:
                skip.add("Metadata/project_settings.config")
            if has_si:
                skip.add("Metadata/slice_info.config")
            skip.add("Metadata/cut_information.xml")

            for name in names:
                if name in skip:
                    continue
                info_obj = zf.getinfo(name)
                data = zf.read(name)

                if name.startswith("Metadata/filament_settings_") and name.endswith(".config"):
                    should_skip = False
                    try:
                        fs = json.loads(data.decode("utf-8"))
                        modified = False
                        for fkey in ("filament_settings_id", "filament_type", "filament_vendor"):
                            if fkey in fs and isinstance(fs[fkey], str):
                                new_val = _remap_filament_name(fs[fkey])
                                if new_val != fs[fkey]:
                                    fs[fkey] = new_val
                                    modified = True
                            elif fkey in fs and isinstance(fs[fkey], list):
                                new_vals = [_remap_filament_name(v) for v in fs[fkey]]
                                if new_vals != fs[fkey]:
                                    fs[fkey] = new_vals
                                    modified = True
                        if "inherits" in fs:
                            if isinstance(fs["inherits"], str):
                                new_inherits = _remap_filament_name(fs["inherits"])
                                if not new_inherits or not new_inherits.strip():
                                    fsid = fs.get("filament_settings_id", "")
                                    if isinstance(fsid, list):
                                        fsid = fsid[0] if fsid else ""
                                    new_inherits = fsid
                                if new_inherits and new_inherits != fs.get("inherits", ""):
                                    fs["inherits"] = new_inherits
                                    modified = True
                        fs_name = fs.get("name", "")
                        if fs_name in _SNAPMK_OFFICIAL_FILAMENT_NAMES:
                            should_skip = True
                        elif isinstance(fs.get("filament_settings_id"), list):
                            if any(n in _SNAPMK_OFFICIAL_FILAMENT_NAMES for n in fs["filament_settings_id"]):
                                should_skip = True
                        if should_skip:
                            info["changes"].append(f"removed {os.path.basename(name)} (now system preset)")
                            continue
                        if "from" in fs and fs["from"] != "project":
                            fs["from"] = "project"
                            modified = True
                        if "version" in fs:
                            fs["version"] = "02.03.01.00"
                            modified = True
                        if modified:
                            data = json.dumps(fs, indent=2, ensure_ascii=False).encode("utf-8")
                            info["changes"].append(f"remapped names in {os.path.basename(name)}")
                    except (json.JSONDecodeError, UnicodeDecodeError):
                        pass

                zf_out.writestr(info_obj, data)

            if has_proj and patched is not None:
                zf_out.writestr("Metadata/project_settings.config",
                                json.dumps(patched, indent=2, ensure_ascii=False))
            if has_si:
                zf_out.writestr("Metadata/slice_info.config", new_si.encode("utf-8"))

            if not has_fil_settings and n_fil > 0 and patched is not None:
                generated = generate_filament_settings(patched, n_fil, official_names)
                for i, fs_dict in enumerate(generated):
                    fs_name = f"Metadata/filament_settings_{i+1}.config"
                    zf_out.writestr(fs_name, json.dumps(fs_dict, indent=2, ensure_ascii=False))
                if generated:
                    info["changes"].append(f"created {len(generated)} filament_settings file(s)")

            if "Metadata/plate_1.json" not in names:
                plate_info = {"bbox_all": [0, 0, 270, 270], "bbox_objects": []}
                zf_out.writestr("Metadata/plate_1.json", json.dumps(plate_info, indent=2))
                info["changes"].append("added plate_1.json")

    return info


# ═══════════════════════════════════════════════════════════════════════════
#  Batch conversion
# ═══════════════════════════════════════════════════════════════════════════

def batch_convert(input_dir, output_dir, machine_cfg, process_cfg, rules, dry_run=False):
    """Convert all 3MF files in a directory."""
    os.makedirs(output_dir, exist_ok=True)
    files = sorted(Path(input_dir).glob("*.3mf"))
    printer_model = machine_cfg.get("printer_model", "Snapmaker U1")

    results = {"total": len(files), "converted": 0, "already_u1": 0,
               "bundle_unpacked": 0, "bundle_files": 0, "errors": []}

    for f in files:
        try:
            with zipfile.ZipFile(f) as zf:
                names = zf.namelist()
                if "Metadata/project_settings.config" in names:
                    proj = json.loads(zf.read("Metadata/project_settings.config"))
                    if proj.get("printer_model") == printer_model:
                        results["already_u1"] += 1
                        print(f"  SKIP (already U1): {f.name}")
                        continue

            if dry_run:
                if is_bundle_3mf(str(f)):
                    print(f"  BUNDLE: {f.name} (would unpack)")
                else:
                    print(f"  WOULD CONVERT: {f.name}")
                continue

            if is_bundle_3mf(str(f)):
                bundle_results = unpack_bundle(str(f), output_dir, machine_cfg, process_cfg, rules)
                n_ok = sum(1 for r in bundle_results if "error" not in r)
                n_err = sum(1 for r in bundle_results if "error" in r)
                if n_ok > 0:
                    results["bundle_unpacked"] += 1
                    results["bundle_files"] += n_ok
                    for r in bundle_results:
                        if "error" not in r:
                            print(f"  BUNDLE-OK: {f.name} -> {r['file']}")
                for r in bundle_results:
                    if "error" in r:
                        results["errors"].append((r["file"], r["error"]))
                        print(f"  BUNDLE-ERR: {r['file']} - {r['error']}")
                continue

            out_path = os.path.join(output_dir, f.name.replace(".3mf", "-U1.3mf"))
            info = convert_3mf(str(f), out_path, machine_cfg, process_cfg, rules)
            results["converted"] += 1
            print(f"  CONVERTED: {f.name} ({info.get('original_printer', '?')} -> {printer_model})")
            for c in info.get("changes", []):
                print(f"    + {c}")
        except Exception as e:
            results["errors"].append((f.name, str(e)))
            print(f"  ERROR: {f.name} - {e}")

    print(f"\nDone. {results['total']} files:")
    print(f"  Converted: {results['converted']}")
    print(f"  Already U1: {results['already_u1']}")
    print(f"  Bundle unpacked: {results['bundle_unpacked']} ({results['bundle_files']} files)")
    print(f"  Errors: {len(results['errors'])}")
    return results


# ═══════════════════════════════════════════════════════════════════════════
#  CLI
# ═══════════════════════════════════════════════════════════════════════════

def _default_profiles_dir():
    """Guess the Snapmaker profiles directory relative to this script."""
    script_dir = Path(__file__).resolve().parent
    # Try orca-slicer-engine repo layout
    candidates = [
        script_dir.parent / "package_consumer_windows" / "resources" / "profiles" / "Snapmaker",
        script_dir.parent / "resources" / "profiles" / "Snapmaker",
    ]
    # Also try relative to cwd
    candidates.append(Path("resources/profiles/Snapmaker"))
    for c in candidates:
        if c.is_dir():
            return str(c)
    return str(candidates[0])  # default, will error with clear message if missing


def main():
    parser = argparse.ArgumentParser(
        description="Convert Bambu/Prusa 3MF files to Snapmaker U1 format"
    )
    parser.add_argument("target", nargs="?", help="Target 3MF file or input directory")
    parser.add_argument("--out", "-o", help="Output path or directory")
    parser.add_argument("--dry-run", "-n", action="store_true", help="Preview changes only")
    parser.add_argument("--validate", action="store_true",
                        help="Validate converted 3MF against U1 requirements")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Print detailed conversion steps")
    parser.add_argument("--profiles-dir", default=_default_profiles_dir(),
                        help="Path to Snapmaker profiles directory (default: auto-detect)")
    parser.add_argument("--machine-profile", default="Snapmaker U1 (0.4 nozzle)",
                        help="Machine profile name (default: 'Snapmaker U1 (0.4 nozzle)')")
    parser.add_argument("--process-profile", default="fdm_process_U1_0.20",
                        help="Process profile name for schema + ceilings (default: 'fdm_process_U1_0.20')")
    parser.add_argument("--rules-dir", default=None,
                        help="Directory of YAML filament-tuning rules (optional)")
    args = parser.parse_args()

    if not args.target:
        parser.print_help()
        sys.exit(1)

    # Load reference profiles
    print(f"Loading U1 reference profiles from: {args.profiles_dir}")
    machine_cfg, process_cfg = _load_reference(args.profiles_dir, args.machine_profile, args.process_profile)
    print(f"  Machine: {machine_cfg.get('printer_model', '?')}")
    print(f"  Printable area: {machine_cfg.get('printable_area', '?')}")
    print(f"  Schema keys: {len(process_cfg)} process + {len(machine_cfg)} machine")

    # Load filament rules
    rules = None
    if args.rules_dir:
        rules = _load_rules(args.rules_dir)
        if rules:
            print(f"  Rules: {len(rules)} loaded from {args.rules_dir}")
    if rules is None:
        # Try default rules dir next to script
        default_rules = Path(__file__).resolve().parent / "rules"
        if default_rules.is_dir():
            rules = _load_rules(str(default_rules))
            if rules:
                print(f"  Rules: {len(rules)} loaded from {default_rules}")

    target = Path(args.target)
    if target.is_dir():
        out_dir = args.out or os.path.join(str(target), "converted")
        batch_convert(str(target), out_dir, machine_cfg, process_cfg, rules, dry_run=args.dry_run)
    elif target.is_file():
        out_dir = os.path.dirname(args.out) if args.out else str(target.parent)
        out = args.out or str(target).replace(".3mf", "-U1.3mf")

        if is_bundle_3mf(str(target)):
            if args.dry_run:
                print(f"BUNDLE: {target.name} (would unpack)")
                sys.exit(0)
            print(f"Bundle detected: {target.name}")
            bundle_results = unpack_bundle(str(target), out_dir, machine_cfg, process_cfg, rules)
            for r in bundle_results:
                if "error" in r:
                    print(f"  ERROR: {r['file']} - {r['error']}")
                else:
                    print(f"  EXTRACTED: {r['file']}")
                    for c in r.get("changes", []):
                        print(f"    + {c}")
            if not bundle_results:
                print("  No geometry files found in bundle.")
            sys.exit(0)

        if args.dry_run:
            with zipfile.ZipFile(str(target)) as zf:
                if "Metadata/project_settings.config" in zf.namelist():
                    proj = json.loads(zf.read("Metadata/project_settings.config"))
                    patched, dropped, clamp_evts, rule_applied, added = \
                        convert_project_settings(proj, machine_cfg, process_cfg, rules)
                    print(f"Would convert: {target.name}")
                    print(f"  printer_model: {proj.get('printer_model')} -> {machine_cfg.get('printer_model', 'Snapmaker U1')}")
                    print(f"  Would drop {len(dropped)} Bambu-specific keys")
                    if clamp_evts:
                        print(f"  Would clamp {len(clamp_evts)} speed/accel values")
                else:
                    print(f"No project_settings.config in {target.name}")
        else:
            info = convert_3mf(str(target), out, machine_cfg, process_cfg, rules)
            print(f"Converted: {info['file']}")
            print(f"  original: {info['original_printer']}")
            in_size = os.path.getsize(str(target))
            out_size = os.path.getsize(out)
            print(f"  size: {in_size:,} -> {out_size:,} bytes")
            for c in info["changes"]:
                print(f"  + {c}")

            if args.validate:
                print("\n--- Validation ---")
                is_valid, warnings = validate_converted_3mf(out, machine_cfg)
                for w in warnings:
                    print(f"  {'[FAIL]' if w.startswith('CRITICAL') else '[WARN]'} {w}")
                if is_valid:
                    print("  Validation: PASSED")
                else:
                    print("  Validation: FAILED (critical issues found)")
                    sys.exit(1)
    else:
        print(f"Error: {args.target} not found")
        sys.exit(1)


if __name__ == "__main__":
    main()
