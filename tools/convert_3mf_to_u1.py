#!/usr/bin/env python3
"""
3MF Printer Converter: Convert Bambu/Prusa 3MF files to Snapmaker U1 format.

Based on analysis of u1convert.com conversion logic.
Only modifies project_settings.config and slice_info.config — does NOT add
machine/process/filament settings (they are inherited from the engine's built-in U1 profile).
"""

import argparse
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
import zipfile
from copy import deepcopy
from pathlib import Path


# ── Helper Functions ──────────────────────────────────────────────────

def _12x(v):
    return [v] * 12

def _6x(v):
    return [v] * 6


# ── U1 Printer Constants ──────────────────────────────────────────────

U1_PRINTABLE_AREA = ["0.5x1", "270.5x1", "270.5x271", "0.5x271"]
U1_PRINTABLE_HEIGHT = "270.05"
U1_NOZZLE_TYPE = "hardened_steel"
U1_NOZZLE_VOLUME = "143"
U1_PRINTER_MODEL = "Snapmaker U1"
U1_VERSION = "2.3.1"

# Speed caps (Bambu printers are faster — we cap to U1 safe limits)
SPEED_CAPS = {
    "travel_speed": 500,
    "outer_wall_speed": 150,
    "inner_wall_speed": 250,
    "sparse_infill_speed": 250,
    "top_surface_speed": 100,
    "gap_infill_speed": 100,
    "internal_solid_infill_speed": 200,
    "default_acceleration": 5000,
    "outer_wall_acceleration": 3000,
    "bridge_speed": 50,
}

# Keys to remove (Bambu/Prusa specific)
KEYS_TO_REMOVE = {
    "chamber_temperatures",
    "extruder_clearance_dist_to_rod",
    "extruder_clearance_max_radius",
    "extruder_type",
    "filament_scarf_gap",
    "filament_scarf_height",
    "filament_scarf_length",
    "filament_scarf_seam_type",
    "initial_layer_flow_ratio",
    "ironing_direction",
    "overhang_threshold_participating_cooling",
    "overhang_totally_speed",
    "process_notes",
    "role_base_wipe_speed",
    "smooth_coefficient",
    "smooth_speed_discontinuity_area",
    "sparse_infill_anchor",
    "sparse_infill_anchor_max",
    "thumbnail_size",
    "top_area_threshold",
    "top_one_wall_type",
    # Additional Bambu-specific keys found in some files
    "chamber_temperature_comment",
    "cooling_tube_retraction_comment",
    "enable_support_comment",
    "extruder_clearance_height_to_lid",
    "extruder_clearance_height_to_rod",
    "gcode_flavor",
    "host_type",
    "inherits",
    "inherits_group",
    "machine_load_filament_time",
    "machine_unload_filament_time",
    "nozzle_type_comment",
    "physical_printer_settings_id",
    "print_host",
    "printer_notes",
    "printer_structure",
    "printer_variant",
    "printhost_apikey",
    "printhost_authorization_type",
    "printhost_port",
    "printhost_ssl",
    "printhost_user",
    "remaining_times",
    "scan_first_layer",
    "silent_mode",
    "support_chamber_temp_control",
    "template_custom_gcode",
    "thumbnails_custom_color",
    "use_relative_e_distances",
    "wiping_volumes_matrix",
    # More Bambu-specific keys found in some files
    "extruder_clearance_height_to_lid",
    "extruder_clearance_height_to_rod",
    "gcode_flavor",
    "host_type",
    "infill_shift_step",
    "inherits_group",
    "machine_load_filament_time",
    "machine_unload_filament_time",
    "printer_notes",
    "printer_structure",
    "printer_variant",
}

# Keys to always add with Snapmaker U1 defaults
KEYS_TO_ADD = {
    "activate_chamber_temp_control": _12x("0"),
    "adaptive_bed_mesh_margin": "0",
    "adaptive_pressure_advance": _12x("0"),
    "adaptive_pressure_advance_bridges": _12x("0"),
    "adaptive_pressure_advance_model": _12x("0,0,0\\n0,0,0"),
    "adaptive_pressure_advance_overhangs": _12x("0"),
    "align_infill_direction_to_model": "0",
    "alternate_extra_wall": "0",
    "bbl_calib_mark_logo": "1",
    "bbl_use_printhost": "0",
    "bed_mesh_max": "99999,99999",
    "bed_mesh_min": "-99999,-99999",
    "bed_mesh_probe_distance": "50,50",
    "bottom_solid_infill_flow_ratio": "1",
    "bottom_surface_density": "100%",
    "bridge_acceleration": "50%",
    "bridge_density": "80%",
    "brim_ears_detection_length": "1",
    "brim_ears_max_angle": "125",
    "calib_flowrate_topinfill_special_order": "0",
    "chamber_temperature": _12x("0"),
    "change_extrusion_role_gcode": "",
    "cooling_tube_length": "5",
    "cooling_tube_retraction": "91.5",
    "counterbore_hole_bridging": "none",
    "default_bed_type": "Textured PEI Plate",
    "default_junction_deviation": "0",
    "delta_temperature": "0",
    "disable_m73": "0",
    "dont_filter_internal_bridges": "disabled",
    "dont_slow_down_outer_wall": _12x("0"),
    "elefant_foot_compensation_layers": "1",
    "emit_machine_limits_to_gcode": "1",
    "enable_change_pressure_when_wiping": "1",
    "enable_extra_bridge_layer": "disabled",
    "enable_filament_ramming": "0",
    "extra_loading_move": "-2",
    "extra_perimeters_on_overhangs": "0",
    "extra_solid_infills": "",
    "extruder_clearance_radius": "72.5",
    "extrusion_rate_smoothing_external_perimeter_only": "0",
    "fan_kickstart": "0",
    "fan_speedup_overhangs": "1",
    "fan_speedup_time": "0",
    "filament_cooling_final_speed": _6x("3.4"),
    "filament_cooling_initial_speed": _6x("2.2"),
    "filament_cooling_moves": _6x("4"),
    "filament_loading_speed": _6x("28"),
    "filament_loading_speed_start": _6x("3"),
    "filament_multitool_ramming": _6x("1"),
    "filament_multitool_ramming_flow": ["10", "15", "10", "10", "10", "15"],
    "filament_multitool_ramming_volume": ["0.5", "0.1", "0.5", "0.5", "10", "0.5"],
    "filament_ramming_parameters": _6x(
        "120 100 6.6 6.8 7.2 7.6 7.9 8.2 8.7 9.4 9.9 10.0"
        "| 0.05 6.6 0.45 6.8 0.95 7.8 1.45 8.3 1.95 9.7 2.45 10 2.95 7.6 3.45 7.6 3.95 7.6 4.45 7.6 4.95 7.6"
    ),
    "filament_retract_length_toolchange": _6x("10"),
    "filament_retract_lift_above": _6x("nil"),
    "filament_retract_lift_below": _6x("nil"),
    "filament_retract_lift_enforce": _6x("nil"),
    "filament_retract_restart_extra_toolchange": _6x("nil"),
    "filament_shrinkage_compensation_z": _6x("100%"),
    "filament_stamping_distance": _6x("0"),
    "filament_stamping_loading_speed": _6x("0"),
    "filament_toolchange_delay": _6x("0"),
    "filament_unloading_speed": _6x("90"),
    "filament_unloading_speed_start": _6x("100"),
    "fill_multiline": "1",
    "fuzzy_skin_first_layer": "0",
    "fuzzy_skin_mode": "displacement",
    "fuzzy_skin_noise_type": "classic",
    "fuzzy_skin_octaves": "4",
    "fuzzy_skin_persistence": "0.5",
    "fuzzy_skin_scale": "1",
    "gap_fill_target": "topbottom",
    "gcode_comments": "0",
    "gcode_label_objects": "1",
    "graphic_effect_plate_temp": _12x("65"),
    "graphic_effect_plate_temp_initial_layer": _12x("65"),
    "high_current_on_filament_swap": "0",
    "hole_to_polyhole": "0",
    "hole_to_polyhole_threshold": "0.01",
    "hole_to_polyhole_twisted": "1",
    "idle_temperature": _12x("0"),
    "infill_anchor": "400%",
    "infill_anchor_max": "20",
    "infill_combination_max_layer_height": "100%",
    "infill_lock_depth": "1",
    "infill_overhang_angle": "60",
    "initial_layer_min_bead_width": "85%",
    "initial_layer_travel_speed": "100%",
    "internal_bridge_angle": "0",
    "internal_bridge_density": "100%",
    "internal_bridge_fan_speed": _12x("-1"),
    "internal_bridge_flow": "1",
    "internal_bridge_speed": "150%",
    "internal_solid_infill_acceleration": "100%",
    "ironing_angle": "-1",
    "ironing_fan_speed": _12x("-1"),
    "interlocking_beam_width": "1",
    "interlocking_boundary_avoidance": "2",
    "interlocking_depth": "1.5",
    "interlocking_beam": "0",
    "interlocking_beam_layer_count": "1",
    "interlocking_orientation": "0",
    "lateral_lattice_angle_1": "-45",
    "lateral_lattice_angle_2": "45",
    "machine_max_junction_deviation": ["0", "0"],
    "machine_tool_change_time": "5",
    "make_overhang_printable": "0",
    "make_overhang_printable_angle": "55",
    "make_overhang_printable_hole_size": "0",
    "manual_filament_change": "0",
    "max_resonance_avoidance_speed": "120",
    "max_volumetric_extrusion_rate_slope": "0",
    "max_volumetric_extrusion_rate_slope_segment_length": "3",
    "min_length_factor": "0.5",
    "min_resonance_avoidance_speed": "70",
    "min_skirt_length": "0",
    "min_width_top_surface": "200%",
    "notes": "",
    "nozzle_hrc": "0",
    "only_one_wall_top": "1",
    "overhang_reverse": "0",
    "overhang_reverse_internal_only": "0",
    "overhang_reverse_threshold": "50%",
    "parking_pos_retraction": "92",
    "pellet_flow_coefficient": _12x("0.4157"),
    "pellet_modded_printer": "0",
    "precise_outer_wall": "0",
    "preferred_orientation": "0",
    "preheat_steps": "1",
    "preheat_time": "30",
    "prime_tower_brim_chamfer": "1",
    "prime_tower_brim_chamfer_max_width": "4",
    "print_order": "default",
    "purge_in_prime_tower": "0",
    "ramming_line_width_ratio": "2",
    "ramming_pressure_advance_value": "0.02",
    "resonance_avoidance": "0",
    "retract_lift_enforce": ["All Surfaces"] * 4,
    "role_based_wipe_speed": "1",
    "scarf_joint_flow_ratio": "1",
    "scarf_joint_speed": "100%",
    "scarf_overhang_threshold": "40%",
    "seam_slope_min_length": "20",
    "seam_slope_start_height": "0",
    "seam_slope_type": "none",
    "single_extruder_multi_material_priming": "0",
    "single_loop_draft_shield": "0",
    "skeleton_infill_density": "25%",
    "skeleton_infill_line_width": "100%",
    "skin_infill_density": "25%",
    "skin_infill_depth": "2",
    "skin_infill_line_width": "100%",
    "skirt_speed": "50",
    "skirt_start_angle": "-135",
    "skirt_type": "combined",
    "slow_down_layers": "0",
    "slowdown_for_curled_perimeters": "0",
    "small_area_infill_flow_compensation": "0",
    "small_area_infill_flow_compensation_model": [],
    "solid_infill_direction": "45",
    "solid_infill_rotate_template": "",
    "sparse_infill_rotate_template": "",
    "spiral_finishing_flow_ratio": "0",
    "spiral_starting_flow_ratio": "0",
    "staggered_inner_seams": "0",
    "support_ironing": "0",
    "support_ironing_flow": "10%",
    "support_ironing_pattern": "rectilinear",
    "support_ironing_spacing": "0.1",
    "support_material_interface_fan_speed": _12x("-1"),
    "support_multi_bed_types": "0",
    "support_threshold_overlap": "50%",
    "textured_cool_plate_temp": _12x("40"),
    "textured_cool_plate_temp_initial_layer": _12x("40"),
    "thick_internal_bridges": "1",
    "thumbnails": "48x48/PNG, 300x300/PNG",
    "thumbnails_format": "PNG",
    "time_cost": "0",
    "tool_change_temprature_wait": "0",
    "top_bottom_infill_wall_overlap": "25%",
    "top_surface_density": "100%",
    "travel_acceleration": "100%",
    "travel_slope": ["3"] * 4,
    "tree_support_adaptive_layer_height": "1",
    "tree_support_angle_slow": "25",
    "tree_support_auto_brim": "1",
    "tree_support_branch_angle_organic": "40",
    "tree_support_branch_diameter_organic": "2",
    "tree_support_branch_distance_organic": "1",
    "tree_support_brim_width": "3",
    "tree_support_tip_diameter": "0.8",
    "tree_support_top_rate": "30%",
    "wall_direction": "auto",
    "wipe_before_external_loop": "0",
    "wipe_on_loops": "0",
    "wipe_tower_bridging": "10",
    "wipe_tower_cone_angle": "15",
    "wipe_tower_extra_flow": "100%",
    "wipe_tower_extra_rib_length": "8",
    "wipe_tower_extra_spacing": "120%",
    "wipe_tower_filament": "1",
    "wipe_tower_fillet_wall": "1",
    "wipe_tower_max_purge_speed": "90",
    "wipe_tower_rib_width": "8",
    "wipe_tower_wall_type": "rectangle",
    "wiping_volumes_extruders": ["70"] * 10,
    "z_hop_when_prime": ["0"] * 4,
    "z_offset": "0",
}

# G-code templates (static only — engine does NOT support {variable} placeholders)
U1_MACHINE_START_GCODE = (
    " ;===== date: 20260128 =====================\n"
    "\n"
    "PRINT_START\n"
    "DEFECT_DETECTION_START\n"
)

U1_MACHINE_END_GCODE = "  PRINT_END\nTIMELAPSE_STOP\n"

U1_BEFORE_LAYER_CHANGE_GCODE = (
    ";BEFORE_LAYER_CHANGE\n"
    ";[layer_z]\n"
    "G92 E0\n"
    "TIMELAPSE_TAKE_FRAME\n"
    "DEFECT_DETECTION_DETECT"
)

U1_LAYER_CHANGE_GCODE = (
    ";AFTER_LAYER_CHANGE\n"
    ";[layer_z]"
)

U1_MACHINE_PAUSE_GCODE = "M600"

U1_CHANGE_FILAMENT_GCODE = (
    ";===== date: 20251213=====================\n"
    "T[next_extruder]\n"
    "M109 S[temperature]\n"
)


def _norm(v):
    """Normalize Bambu-style nested arrays to scalar or simple list."""
    if isinstance(v, list) and len(v) == 1 and isinstance(v[0], list):
        return v[0]
    return v


def count_filaments(proj):
    """Get number of filaments from project settings."""
    n = 1
    for key, val in proj.items():
        if isinstance(val, list) and len(val) > n:
            # Only count arrays that look like per-filament settings
            if all(isinstance(x, str) for x in val):
                n = max(n, len(val))
    return min(n, 12)  # cap at 12


def build_gcode_overrides(proj):
    """Replace Bambu-specific G-code with Snapmaker U1 equivalents."""
    overrides = {
        "machine_start_gcode": U1_MACHINE_START_GCODE,
        "machine_end_gcode": U1_MACHINE_END_GCODE,
        "machine_pause_gcode": U1_MACHINE_PAUSE_GCODE,
        "before_layer_change_gcode": U1_BEFORE_LAYER_CHANGE_GCODE,
        "layer_change_gcode": U1_LAYER_CHANGE_GCODE,
        "change_filament_gcode": U1_CHANGE_FILAMENT_GCODE,
    }
    result = {}
    for key, val in overrides.items():
        if key not in proj or _is_bambu_gcode(proj[key], key):
            result[key] = val
    return result


def _is_bambu_gcode(value, key):
    """Check if a G-code string looks like it's Bambu-specific."""
    if not isinstance(value, str) or not value.strip():
        return True  # empty gcode -> replace
    s = value.lower()
    bambu_indicators = [
        "bbl", "bambu", "m1007", "g392", "m620", "m622",
        "m9833", "m73 p", "a1", "p1s", "x1c",
        "m400 u1", "m991",
    ]
    return any(ind in s for ind in bambu_indicators)


def _normalize_printer_id(raw):
    """Convert Bambu printer ID to Snapmaker U1."""
    if not isinstance(raw, str):
        return f"{U1_PRINTER_MODEL} (0.4 nozzle)"
    # Extract nozzle size if present
    m = re.search(r"(\d+\.?\d*)\s*mm\s*nozzle", raw, re.IGNORECASE)
    nozzle = m.group(1) if m else "0.4"
    return f"{U1_PRINTER_MODEL} ({nozzle} nozzle)"


def _normalize_print_settings(raw):
    """Append (converted) to the print settings name."""
    if isinstance(raw, str) and not raw.endswith("(converted)"):
        return raw + " (converted)"
    return str(raw)


def _remap_filament_name(name):
    """Map Bambu/Prusa filament names to Generic equivalents that U1 engine recognizes."""
    if not isinstance(name, str) or not name.strip():
        return name

    # Remove filename annotations appended by Bambu Studio
    # e.g. "Bambu PLA Basic @BBL X1C(all+multicolored.3mf)" -> "Bambu PLA Basic @BBL X1C"
    name = re.sub(r"\([^)]*\.3mf\)$", "", name).strip()

    # Remove printer suffixes: @BBL A1, @BBL X1C, @BBL A1M, @BBL P1S, etc.
    name = re.sub(r"\s*@BBL\s+\S+$", "", name).strip()

    # Map brand prefixes to Generic
    replacements = [
        (r"^Bambu\s+PLA\s+.*", "Generic PLA"),
        (r"^Bambu\s+PETG\s+.*", "Generic PETG"),
        (r"^Bambu\s+ABS\s+.*", "Generic ABS"),
        (r"^Bambu\s+ASA\s+.*", "Generic ASA"),
        (r"^Bambu\s+TPU\s+.*", "Generic TPU"),
        (r"^Bambu\s+PA\s*.*", "Generic PA"),
        (r"^Bambu\s+PC\s+.*", "Generic PC"),
        (r"^Bambu\s+Support\s+.*", "Generic Support"),
        (r"^Generic\s+PLA\s*$", "Generic PLA"),
        (r"^Generic\s+PETG\s*$", "Generic PETG"),
        (r"^Generic\s+ABS\s*$", "Generic ABS"),
        (r"^Generic\s+ASA\s*$", "Generic ASA"),
        (r"^Generic\s+TPU\s*$", "Generic TPU"),
        (r"^Generic\s+PA\s*$", "Generic PA"),
        (r"^Generic\s+PC\s*$", "Generic PC"),
        (r"^Generic\s+PVA\s*$", "Generic PVA"),
        (r"^eSUN\s+.*", "Generic PLA"),
        (r"^PolyLite\s+.*", "Generic PLA"),
        (r"^PolyTerra\s+.*", "Generic PLA"),
        (r"^Overture\s+.*", "Generic PLA"),
        (r"^SUNLU\s+.*", "Generic PLA"),
    ]
    for pattern, replacement in replacements:
        if re.match(pattern, name, re.IGNORECASE):
            return replacement

    # Map to nearest Generic
    upper = name.upper()
    if "PLA" in upper:
        return "Generic PLA"
    if "PETG" in upper or "PET" in upper:
        return "Generic PETG"
    if "ABS" in upper:
        return "Generic ABS"
    if "ASA" in upper:
        return "Generic ASA"
    if "TPU" in upper or "TPE" in upper:
        return "Generic TPU"
    if "PA" in upper or "NYLON" in upper:
        return "Generic PA"
    if "PC" in upper:
        return "Generic PC"
    if "PVA" in upper:
        return "Generic PVA"
    if "SUPPORT" in upper:
        return "Generic Support"

    return name


def convert_project_settings(proj):
    """Apply all U1 conversion rules to project settings dict."""
    result = deepcopy(proj)
    n_fil = count_filaments(proj)

    # 1. Remove Bambu-specific keys
    for key in KEYS_TO_REMOVE:
        result.pop(key, None)

    # 2. Set printer identity
    result["printer_model"] = U1_PRINTER_MODEL
    if "printer_settings_id" in result:
        result["printer_settings_id"] = _normalize_printer_id(result["printer_settings_id"])
    if "print_settings_id" in result:
        result["print_settings_id"] = _normalize_print_settings(result["print_settings_id"])

    # 3. Set U1 bed dimensions
    result["printable_area"] = U1_PRINTABLE_AREA
    result["printable_height"] = U1_PRINTABLE_HEIGHT
    result["bed_exclude_area"] = ["0x0"]

    # 4. Nozzle settings
    result["nozzle_type"] = U1_NOZZLE_TYPE
    result["nozzle_volume"] = U1_NOZZLE_VOLUME
    result["version"] = U1_VERSION

    # Expand nozzle_diameter array to match filament count
    nozzle = _norm(proj.get("nozzle_diameter", ["0.4"]))
    if isinstance(nozzle, str):
        nozzle = [nozzle]
    if isinstance(nozzle, list) and len(nozzle) < n_fil:
        result["nozzle_diameter"] = nozzle * n_fil

    # 5. Cap speeds
    for key, cap in SPEED_CAPS.items():
        if key in result:
            val = _norm(result[key])
            try:
                val = int(val) if isinstance(val, str) else val
                if isinstance(val, (int, float)):
                    if key == "inner_wall_acceleration" and val == 0:
                        result[key] = cap  # 0 means "use default", set explicitly
                    else:
                        result[key] = min(val, cap)
            except (ValueError, TypeError):
                pass

    # 6. Support filament indices (0-based -> 1-based for U1)
    result["support_filament"] = "1"
    result["support_interface_filament"] = "1"

    # 7. Replace G-code
    gcode = build_gcode_overrides(proj)
    for key, val in gcode.items():
        result[key] = val

    # 8. Clear time_lapse_gcode (U1 handles timelapse natively)
    result["time_lapse_gcode"] = ""

    # 9. Remap filament names to Generic equivalents
    for key in ("filament_type", "filament_settings_id", "default_filament_colour",
                "filament_colour", "support_filament", "support_interface_filament"):
        if key in result:
            val = result[key]
            if isinstance(val, str):
                result[key] = _remap_filament_name(val)
            elif isinstance(val, list):
                result[key] = [_remap_filament_name(v) for v in val]

    # Also fix filament-specific keys in filament_settings files
    # (handled at the 3MF level — see convert_3mf)

    # 10. Add Snapmaker U1 defaults
    for key, val in KEYS_TO_ADD.items():
        result[key] = deepcopy(val)

    return result


def convert_slice_info(xml_str):
    """Update slice_info.config XML for Snapmaker U1."""
    try:
        root = ET.fromstring(xml_str)
        for plate in root.findall(".//plate"):
            for meta in plate.findall("metadata"):
                if meta.get("key") == "printer_model_id":
                    meta.set("value", U1_PRINTER_MODEL)
        # Remove warning elements (may reference Bambu-specific things)
        for warning in root.findall(".//warning"):
            plate = warning.getparent() if hasattr(warning, 'getparent') else None
            if plate is not None:
                plate.remove(warning)
        return ET.tostring(root, encoding="unicode")
    except ET.ParseError:
        return xml_str


def convert_3mf(input_path, output_path):
    """
    Convert a single 3MF file to Snapmaker U1 format.

    Returns a dict with conversion info.
    """
    info = {"file": os.path.basename(input_path), "changes": []}

    with zipfile.ZipFile(input_path, "r") as zf:
        names = zf.namelist()

        # Read original project settings
        has_proj = "Metadata/project_settings.config" in names
        if has_proj:
            proj = json.loads(zf.read("Metadata/project_settings.config"))
            old_printer = proj.get("printer_model", "<<none>>")
            info["original_printer"] = old_printer
            patched = convert_project_settings(proj)
            info["changes"].append(f"printer_model: {old_printer} -> Snapmaker U1")
        else:
            patched = None
            info["original_printer"] = "<<missing>>"
            info["changes"].append("created project_settings.config")

        # Read slice_info
        has_si = "Metadata/slice_info.config" in names
        if has_si:
            si_xml = zf.read("Metadata/slice_info.config").decode("utf-8")
            new_si = convert_slice_info(si_xml)
            if new_si != si_xml:
                info["changes"].append("updated slice_info.config (printer_model_id -> Snapmaker U1)")

        # Read model_settings
        has_ms = "Metadata/model_settings.config" in names

        # Build output archive
        with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as zf_out:
            skip = set()
            if has_proj:
                skip.add("Metadata/project_settings.config")
            if has_si:
                skip.add("Metadata/slice_info.config")
            # Remove Bambu-specific metadata
            skip.add("Metadata/cut_information.xml")

            for name in names:
                if name in skip:
                    continue
                info_obj = zf.getinfo(name)
                data = zf.read(name)

                # Patch filament_settings files: remap filament names
                if name.startswith("Metadata/filament_settings_") and name.endswith(".config"):
                    try:
                        fs = json.loads(data.decode("utf-8"))
                        for fkey in ("filament_settings_id", "filament_type", "filament_vendor"):
                            if fkey in fs and isinstance(fs[fkey], str):
                                fs[fkey] = _remap_filament_name(fs[fkey])
                        # Fix inherits chain
                        if "inherits" in fs and isinstance(fs["inherits"], str):
                            fs["inherits"] = _remap_filament_name(fs["inherits"])
                        data = json.dumps(fs, indent=2, ensure_ascii=False).encode("utf-8")
                        info["changes"].append("remapped filament names")
                    except (json.JSONDecodeError, UnicodeDecodeError):
                        pass

                zf_out.writestr(info_obj, data)

            # Write patched/new config files with compression
            if has_proj:
                zf_out.writestr(
                    "Metadata/project_settings.config",
                    json.dumps(patched, indent=2, ensure_ascii=False),
                )
            if has_si:
                zf_out.writestr(
                    "Metadata/slice_info.config",
                    new_si.encode("utf-8"),
                )

            # Add plate_1.json if missing
            if "Metadata/plate_1.json" not in names:
                plate_info = {"bbox_all": [0, 0, 270, 270], "bbox_objects": []}
                zf_out.writestr(
                    "Metadata/plate_1.json",
                    json.dumps(plate_info, indent=2),
                )
                info["changes"].append("added plate_1.json")

    return info


def batch_convert(input_dir, output_dir, dry_run=False):
    """Convert all 3MF files in a directory."""
    os.makedirs(output_dir, exist_ok=True)
    files = sorted(Path(input_dir).glob("*.3mf"))

    results = {"total": len(files), "converted": 0, "already_u1": 0, "errors": []}

    for f in files:
        out_path = os.path.join(output_dir, f.name.replace(".3mf", "-U1.3mf"))
        try:
            # Quick check if already U1
            with zipfile.ZipFile(f) as zf:
                if "Metadata/project_settings.config" in zf.namelist():
                    proj = json.loads(zf.read("Metadata/project_settings.config"))
                    if proj.get("printer_model") == U1_PRINTER_MODEL:
                        results["already_u1"] += 1
                        print(f"  SKIP (already U1): {f.name}")
                        continue

            if dry_run:
                info = convert_project_settings(
                    json.loads(zipfile.ZipFile(f).read("Metadata/project_settings.config"))
                )
                print(f"  WOULD CONVERT: {f.name}")
            else:
                info = convert_3mf(str(f), out_path)
                results["converted"] += 1
                print(f"  CONVERTED: {f.name} ({info['original_printer']} -> Snapmaker U1)")
        except Exception as e:
            results["errors"].append((f.name, str(e)))
            print(f"  ERROR: {f.name} - {e}")

    print(f"\nDone. {results['total']} files:")
    print(f"  Converted: {results['converted']}")
    print(f"  Already U1: {results['already_u1']}")
    print(f"  Errors: {len(results['errors'])}")
    return results


def main():
    parser = argparse.ArgumentParser(
        description="Convert Bambu/Prusa 3MF files to Snapmaker U1 format"
    )
    parser.add_argument("target", nargs="?", help="Target 3MF file or input directory")
    parser.add_argument("--out", "-o", help="Output path or directory")
    parser.add_argument("--dry-run", "-n", action="store_true", help="Preview changes only")
    args = parser.parse_args()

    if not args.target:
        parser.print_help()
        sys.exit(1)

    target = Path(args.target)
    if target.is_dir():
        out_dir = args.out or os.path.join(str(target), "converted")
        batch_convert(str(target), out_dir, dry_run=args.dry_run)
    elif target.is_file():
        out = args.out or str(target).replace(".3mf", "-U1.3mf")
        if args.dry_run:
            with zipfile.ZipFile(str(target)) as zf:
                proj = json.loads(zf.read("Metadata/project_settings.config"))
                info = convert_project_settings(proj)
                print(f"Would convert: {target.name}")
                print(f"  printer_model: {proj.get('printer_model')} -> Snapmaker U1")
        else:
            info = convert_3mf(str(target), out)
            print(f"Converted: {info['file']}")
            print(f"  original: {info['original_printer']}")
            in_size = os.path.getsize(str(target))
            out_size = os.path.getsize(out)
            print(f"  size: {in_size:,} -> {out_size:,} bytes")
            for c in info["changes"]:
                print(f"  + {c}")
    else:
        print(f"Error: {args.target} not found")
        sys.exit(1)


if __name__ == "__main__":
    main()
