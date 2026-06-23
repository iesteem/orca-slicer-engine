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

# Speed caps for U1 (Bambu printers are faster — we cap to U1 safe limits)
SPEED_CAPS_NUMERIC = [
    ("travel_speed", 500),
    ("outer_wall_speed", 200),
    ("inner_wall_speed", 300),
    ("sparse_infill_speed", 270),
    ("top_surface_speed", 200),
    ("gap_infill_speed", 250),
    ("internal_solid_infill_speed", 250),
    ("bridge_speed", 50),
    ("support_speed", 150),
    ("support_interface_speed", 80),
    ("initial_layer_speed", 50),
    ("initial_layer_infill_speed", 105),
    ("skirt_speed", 50),
    ("wipe_speed", 75),
    ("ironing_speed", 30),
]

ACCEL_CAPS_NUMERIC = [
    ("default_acceleration", 10000),
    ("outer_wall_acceleration", 5000),
    ("inner_wall_acceleration", 10000),
    ("initial_layer_acceleration", 500),
    ("top_surface_acceleration", 2000),
    ("travel_acceleration", 10000),
]

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
    "timelapse_type",
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

# G-code templates (engine expands {variable} placeholders at runtime)
U1_MACHINE_START_GCODE = (
    ";===== date: 20260128 =====================\n"
    "PRINT_START\n"
    "DEFECT_DETECTION_START\n"
    "SET_PRINT_STATS_INFO TOTAL_LAYER={total_layer_count} CURRENT_LAYER=0\n"
    "TIMELAPSE_START\n"
    "M140 S{bed_temperature_initial_layer_single}\n"
    "M104 T{initial_extruder} S140\n"
    "M204 S10000\n"
    "G28 X Y\n"
    "T{initial_extruder}\n"
    "G90\n"
    "DEFECT_DETECTION_DETECT_BED\n"
    "SM_PRINT_CHECK_SWITCH_EXTRUDER\n"
    "M106 S255\n"
    "M106 P2 S0\n"
    "MOVE_TO_DISCARD_FILAMENT_POSITION\n"
    "M109 T{initial_extruder} S{nozzle_temperature[initial_extruder] - 90}\n"
    "ROUGHLY_CLEAN_NOZZLE_WITH_DISCARD\n"
    "MOVE_TO_XY_IDLE_POSITION_EXTRUDER\n"
    "G28 Z I140 J140\n"
    "DETECT_BED_PLATE\n"
    "G90\n"
    "G0 Z5 F10000\n"
    "MOVE_TO_DISCARD_FILAMENT_POSITION\n"
    "M109 S{nozzle_temperature[initial_extruder] - 50}\n"
    "ROUGHLY_CLEAN_NOZZLE\n"
    "MOVE_TO_XY_IDLE_POSITION_EXTRUDER\n"
    "FINELY_CLEAN_NOZZLE_STAGE_1\n"
    "M104 S{nozzle_temperature[initial_extruder] - 90}\n"
    "G0 Z5 F10000\n"
    "MOVE_TO_DISCARD_FILAMENT_POSITION\n"
    "ROUGHLY_CLEAN_NOZZLE\n"
    "MOVE_TO_XY_IDLE_POSITION_EXTRUDER\n"
    "FINELY_CLEAN_NOZZLE_STAGE_2\n"
    "M106 S255\n"
    "M109 S{nozzle_temperature[initial_extruder] - 90}\n"
    "M190 S{bed_temperature_initial_layer_single}\n"
    "M107 P2\n"
    "G90\n"
    "G0 Z5 F10000\n"
    "G28 Z\n"
    "BED_MESH_CALIBRATE PROBE_COUNT=11,11\n"
    "G90\n"
    "G1 Z1.5\n"
    "G0 X85 Y1 Z2 F18000\n"
    "M109 S{nozzle_temperature_initial_layer[initial_extruder]}\n"
    "G1 Z0.2\n"
    "M83\n"
    "G1 X185 E15 F360\n"
    "G1 Z1.5\n"
    "G90\n"
    "M106 S0\n"
)

U1_MACHINE_END_GCODE = "PRINT_END\nTIMELAPSE_STOP\n"

U1_BEFORE_LAYER_CHANGE_GCODE = (
    ";BEFORE_LAYER_CHANGE\n"
    ";[layer_z]\n"
    "G92 E0\n"
    "TIMELAPSE_TAKE_FRAME\n"
    "DEFECT_DETECTION_DETECT"
)

U1_LAYER_CHANGE_GCODE = (
    ";AFTER_LAYER_CHANGE\n"
    ";[layer_z]\n"
    "SET_PRINT_STATS_INFO TOTAL_LAYER={total_layer_count} CURRENT_LAYER={layer_num+1}"
)

U1_MACHINE_PAUSE_GCODE = "M600"

U1_CHANGE_FILAMENT_GCODE = (
    ";===== date: 20251213=====================\n"
    "; Change Tool[previous_extruder] -> Tool[next_extruder] (layer [layer_num])\n"
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


def _normalize_printer_id(raw, nozzle_diameters=None):
    """Convert Bambu printer ID to Snapmaker U1, deriving nozzle from config."""
    # Prefer nozzle_diameter from config (the actual hardware spec)
    if nozzle_diameters:
        sizes = []
        for x in nozzle_diameters:
            try:
                sizes.append(float(x))
            except (ValueError, TypeError):
                pass
        if sizes:
            nozzle = str(max(sizes))
            if nozzle.endswith(".0"):
                nozzle = nozzle[:-2]
            return f"{U1_PRINTER_MODEL} ({nozzle} nozzle)"
    # Fallback: extract from printer_settings_id string
    if isinstance(raw, str):
        m = re.search(r"(\d+\.?\d*)\s*mm\s*nozzle", raw, re.IGNORECASE)
        if m:
            return f"{U1_PRINTER_MODEL} ({m.group(1)} nozzle)"
    return f"{U1_PRINTER_MODEL} (0.4 nozzle)"


def _normalize_print_settings(raw):
    """Append (converted) to the print settings name."""
    if isinstance(raw, str) and not raw.endswith("(converted)"):
        return raw + " (converted)"
    return str(raw)


# ── Snapmaker U1 Official Filament Preset Names ─────────────────────────
# These are the exact preset names on disk under:
#   resources/profiles/Snapmaker/filament/Snapmaker XXX @U1.json
# The engine validates filament_settings_id against these names.
_SNAPMK_U1_FILAMENTS_BY_TYPE = {
    # PLA variants
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
    # PETG variants
    ("petg", "cf"):           "Snapmaker PETG-CF @U1",
    ("petg", "carbon fiber"): "Snapmaker PETG-CF @U1",
    ("petg", None):           "Snapmaker PETG @U1",
    ("pet", None):            "Snapmaker PET @U1",
    # ABS/ASA
    ("abs", None):            "Snapmaker ABS @U1",
    ("asa", None):            "Snapmaker ASA @U1",
    # TPU/TPE
    ("tpu", "95a"):           "Snapmaker TPU 95A @U1",
    ("tpu", "90a"):           "Snapmaker TPU 90A @U1",
    ("tpu", "hf"):            "Snapmaker TPU High-Flow @U1",
    ("tpu", "high flow"):     "Snapmaker TPU High-Flow @U1",
    ("tpu", None):            "Snapmaker TPU @U1",
    ("tpe", None):            "Snapmaker TPE @U1",
    # PA / Nylon
    ("pa", "cf"):             "Snapmaker PA-CF @U1",
    ("pa", "carbon fiber"):   "Snapmaker PA-CF @U1",
    ("pa", None):             "Generic PA @System",
    # PC
    ("pc", None):             "Generic PC @System",
    # PVA / Support
    ("pva", None):            "Snapmaker PVA @U1",
    ("support", "pla"):       "Snapmaker Breakaway Support For PLA @U1",
    ("support", None):        "Snapmaker Breakaway Support For PLA @U1",
}

# Fallback mapping from keyword-only detection
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

# Recognized sub-type keywords for detection
_SUBTYPE_KEYWORDS = [
    "basic", "matte", "silk", "eco", "wood", "metal", "marble", "glow",
    "snapspeed", "high speed", "cf", "carbon fiber", "full spectrum",
    "95a", "90a", "hf", "high flow",
]


def _remap_filament_name(name):
    """Map Bambu/Prusa filament names to Snapmaker U1 official preset names.

    The engine's validate_filament_official() checks filament_settings_id
    against system presets.  Mapping to exact Snapmaker @U1 preset names
    ensures the engine finds a direct match (fast path #1).
    """
    if not isinstance(name, str) or not name.strip():
        return name

    # ---- Phase 1: strip Bambu annotations ----
    # Remove filename annotations:  "Bambu PLA Basic @BBL X1C(all+multicolored.3mf)"
    name = re.sub(r"\([^)]*\.3mf\)$", "", name).strip()
    # Remove printer suffixes: @BBL A1, @BBL X1C, @BBL A1M, @BBL P1S
    name = re.sub(r"\s*@BBL\s+\S+$", "", name).strip()
    # Remove Prusa printer suffixes
    name = re.sub(r"\s*@Prusa\s+\S+$", "", name).strip()

    # ---- Phase 2: detect material type and sub-type ----
    upper = name.upper()
    material = None
    for key in ("PLA", "PETG", "PET", "ABS", "ASA", "TPU", "TPE",
                 "PA", "PC", "PVA", "NYLON", "SUPPORT"):
        if key in upper:
            material = key
            break

    if material is None:
        return name

    # Resolve synonyms
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

    # Handle "Bambu PLA-CF" style combined names
    if material == "PLA" and "PLA-CF" in upper:
        material, subtype = "PLA", "cf"
    if material == "PETG" and "PETG-CF" in upper:
        material, subtype = "PETG", "cf"

    # ---- Phase 3: lookup exact match in mapping table ----
    key = (material.lower(), subtype)
    if key in _SNAPMK_U1_FILAMENTS_BY_TYPE:
        return _SNAPMK_U1_FILAMENTS_BY_TYPE[key]
    # Try material-only fallback
    key = (material.lower(), None)
    if key in _SNAPMK_U1_FILAMENTS_BY_TYPE:
        return _SNAPMK_U1_FILAMENTS_BY_TYPE[key]

    # ---- Phase 4: keyword-based fallback ----
    if material in _KEYWORD_TO_SNAPMK_FILAMENT:
        return _KEYWORD_TO_SNAPMK_FILAMENT[material]

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
    # Get raw nozzle diameters before they are expanded, for printer ID
    raw_nozzle = _norm(proj.get("nozzle_diameter", ["0.4"]))
    if isinstance(raw_nozzle, str):
        raw_nozzle = [raw_nozzle]
    result["printer_settings_id"] = _normalize_printer_id(
        result.get("printer_settings_id", ""), raw_nozzle)
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

    # Expand nozzle_diameter array to match filament count,
    # clamping each value to U1 supported sizes [0.2, 0.4, 0.6, 0.8]
    def _clamp_nozzle(value):
        try:
            v = float(value)
        except (ValueError, TypeError):
            return "0.4"
        allowed = [0.2, 0.4, 0.6, 0.8]
        return str(min(allowed, key=lambda x: abs(x - v)))

    nozzle = _norm(proj.get("nozzle_diameter", ["0.4"]))
    if isinstance(nozzle, str):
        nozzle = [nozzle]
    nozzle = [_clamp_nozzle(v) for v in nozzle]
    if len(nozzle) < n_fil:
        result["nozzle_diameter"] = (nozzle * n_fil)[:n_fil]
    else:
        result["nozzle_diameter"] = nozzle[:n_fil]

    # 5. Cap speeds (skip percentage values, handle 0-means-default)
    def _cap_numeric(result, key, cap):
        if key not in result:
            return
        val = _norm(result[key])
        if isinstance(val, str) and "%" in val:
            return  # percentage-based acceleration, skip
        was_str = isinstance(val, str)
        try:
            val = int(val) if was_str else val
            if isinstance(val, (int, float)):
                if val == 0 and key in ("inner_wall_acceleration", "default_acceleration"):
                    result[key] = str(cap) if was_str else cap
                elif cap > 0:
                    capped = min(val, cap)
                    result[key] = str(capped) if was_str else capped
        except (ValueError, TypeError):
            pass

    for key, cap in SPEED_CAPS_NUMERIC:
        _cap_numeric(result, key, cap)
    for key, cap in ACCEL_CAPS_NUMERIC:
        _cap_numeric(result, key, cap)

    # 6. Support filament indices (0-based -> 1-based for U1)
    result["support_filament"] = "1"
    result["support_interface_filament"] = "1"

    # 7. Replace G-code
    gcode = build_gcode_overrides(proj)
    for key, val in gcode.items():
        result[key] = val

    # 8. Clear time_lapse_gcode (U1 handles timelapse natively)
    result["time_lapse_gcode"] = ""

    # 9. Keep original "from" field — do NOT force to "project".
    # When from="project" the engine looks for filament presets inside
    # the 3MF's filament_settings files, but we skip generating those
    # for official Snapmaker names (to avoid circular inherits).  The
    # original value lets the engine find presets in the system bundle.

    # 10. Clean up Bambu multi-material flush/prime params.
    # Only zero out for single-filament; multi-filament needs valid
    # flush volumes & prime tower — zeroing them causes rc=6.
    if "flush_volumes_matrix" in result:
        if n_fil <= 1:
            result["flush_volumes_matrix"] = ["0"]
    result["flush_volumes_vector"] = ["140"] * n_fil
    result["wiping_volumes_extruders"] = ["70"] * 10
    if n_fil <= 1:
        result["enable_prime_tower"] = "0"
        result["prime_volume"] = "0"
        result["flush_into_infill"] = "0"
        result["flush_into_objects"] = "0"

    # 11. Remap filament names to Snapmaker U1 official equivalents
    for key in ("filament_type", "filament_settings_id", "default_filament_colour",
                "filament_colour", "support_filament", "support_interface_filament"):
        if key in result:
            val = result[key]
            if isinstance(val, str):
                result[key] = _remap_filament_name(val)
            elif isinstance(val, list):
                result[key] = [_remap_filament_name(v) for v in val]

    # 12. Add Snapmaker U1 defaults
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


# ── Filament Settings Generation ────────────────────────────────────────

# Keys that belong in a filament_settings file (per-extruder values)
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

# Known official Snapmaker U1 filament preset names (for validation)
_SNAPMK_OFFICIAL_FILAMENT_NAMES = set(
    _SNAPMK_U1_FILAMENTS_BY_TYPE.values()
) | {"Generic PLA @System", "Generic PETG @System", "Generic ABS @System",
     "Generic ASA @System", "Generic PA @System", "Generic PC @System",
     "Generic TPU @System", "Generic PVA @System", "Generic Support @System"}


def generate_filament_settings(proj, n_fil, official_names):
    """Generate filament_settings config dicts for each extruder with overrides.

    Args:
        proj: Modified project_settings dict (after convert_project_settings).
        n_fil: Number of filaments/extruders.
        official_names: List of official Snapmaker filament name strings used
                       as filament_settings_id (and inherits fallback).

    Returns:
        List of dicts, each a filament_settings config for JSON serialization.
        Empty list if no extruder has filament-level overrides.
    """
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

        # Only generate a filament_settings file when the filament name is
        # NOT already an official Snapmaker preset.  When it IS official the
        # engine finds it directly via find_in_system() — wrapping it would
        # create a self-referencing inherits loop (name == inherits).
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
            # Extract per-extruder value
            if isinstance(val, list) and len(val) > i:
                elem = val[i]
            elif isinstance(val, list) and len(val) == 1:
                elem = val[0]
            elif isinstance(val, str):
                elem = val
            else:
                continue

            # Skip nil/empty sentinel values
            if elem == "nil" or elem is None:
                continue
            # Skip empty strings that aren't meaningful
            if isinstance(elem, str) and elem == "" and key not in (
                "filament_end_gcode", "filament_start_gcode", "filament_notes"):
                continue

            fs[key] = [str(elem)]
            has_overrides = True

        if has_overrides:
            filament_settings_list.append(fs)

    return filament_settings_list


# ── Post-Conversion Validation ──────────────────────────────────────────

def validate_converted_3mf(zf_or_path):
    """Validate a converted 3MF against Snapmaker U1 requirements.

    Args:
        zf_or_path: A zipfile.ZipFile instance or path string to the 3MF.

    Returns:
        (bool, list_of_str): (is_valid, list_of_warnings).
        True = passes all critical checks.
    """
    import contextlib

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

        # Check project_settings exists
        if "Metadata/project_settings.config" not in names:
            warnings.append("CRITICAL: Missing project_settings.config")
            is_valid = False
            return is_valid, warnings

        proj = json.loads(zf.read("Metadata/project_settings.config"))

        # 1. Printer model
        model = proj.get("printer_model", "")
        if model != U1_PRINTER_MODEL:
            warnings.append(f"CRITICAL: printer_model is '{model}', expected '{U1_PRINTER_MODEL}'")
            is_valid = False

        # 2. Nozzle type
        nozzle = proj.get("nozzle_type", "")
        if nozzle != U1_NOZZLE_TYPE:
            warnings.append(f"WARNING: nozzle_type is '{nozzle}', expected '{U1_NOZZLE_TYPE}'")

        # 3. Bed dimensions
        area = proj.get("printable_area", [])
        if area != U1_PRINTABLE_AREA:
            warnings.append(f"CRITICAL: printable_area mismatch, expected {U1_PRINTABLE_AREA}")
            is_valid = False
        height = proj.get("printable_height", "")
        if height != U1_PRINTABLE_HEIGHT:
            warnings.append(f"CRITICAL: printable_height is '{height}', expected '{U1_PRINTABLE_HEIGHT}'")
            is_valid = False

        # 4. Filament check
        filament_ids = proj.get("filament_settings_id", [])
        if isinstance(filament_ids, str):
            filament_ids = [filament_ids]
        for idx, fid in enumerate(filament_ids):
            if not fid or not fid.strip():
                warnings.append(f"WARNING: extruder {idx+1} filament_settings_id is empty")
            elif fid not in _SNAPMK_OFFICIAL_FILAMENT_NAMES:
                warnings.append(f"WARNING: extruder {idx+1} filament '{fid}' is not a recognized Snapmaker U1 preset")

        # 5. Filament settings files inherits
        for name in names:
            if name.startswith("Metadata/filament_settings_") and name.endswith(".config"):
                try:
                    fs = json.loads(zf.read(name))
                    inh = fs.get("inherits", "")
                    if isinstance(inh, str) and (not inh or not inh.strip()):
                        warnings.append(f"CRITICAL: {name} has empty inherits — engine will reject")
                        is_valid = False
                except (json.JSONDecodeError, UnicodeDecodeError):
                    warnings.append(f"WARNING: {name} could not be parsed")

        # 6. Check for leftover Bambu-specific keys
        _bambu_indicators = ["host_type", "gcode_flavor", "printhost_apikey",
                            "printhost_ssl", "printhost_port", "printhost_user",
                            "scan_first_layer", "silent_mode", "remaining_times",
                            "bbl_use_printhost"]
        for key in _bambu_indicators:
            if key in proj:
                warnings.append(f"WARNING: Bambu-specific key '{key}' still present in project_settings")

        # 7. Printer settings ID check
        ps_id = proj.get("printer_settings_id", "")
        if "Snapmaker U1" not in str(ps_id):
            warnings.append(f"WARNING: printer_settings_id '{ps_id}' does not contain 'Snapmaker U1'")

    return is_valid, warnings


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

        # Determine filament count and official names for filament_settings generation
        n_fil = count_filaments(patched) if patched else 1
        official_names = patched.get("filament_settings_id", []) if patched else []
        if isinstance(official_names, str):
            official_names = [official_names]

        # Check if source already has filament_settings files
        has_fil_settings = any(
            n.startswith("Metadata/filament_settings_") and n.endswith(".config")
            for n in names
        )

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

                # Patch existing filament_settings files — remove them if
                # they map to official U1 presets (system handles those).
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

                        # Fix inherits: remap, fill from filament_settings_id if empty
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

                        # If filament name maps to official preset, drop the
                        # file — system preset handles it, avoids circular inherits.
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

            # --- Generate missing filament_settings files ---
            if not has_fil_settings and n_fil > 0:
                generated = generate_filament_settings(patched, n_fil, official_names)
                for i, fs_dict in enumerate(generated):
                    fs_name = f"Metadata/filament_settings_{i+1}.config"
                    zf_out.writestr(
                        fs_name,
                        json.dumps(fs_dict, indent=2, ensure_ascii=False),
                    )
                if generated:
                    info["changes"].append(
                        f"created {len(generated)} filament_settings file(s) with inherits chain"
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
    parser.add_argument("--validate", action="store_true",
                        help="Validate converted 3MF against Snapmaker U1 requirements")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Print detailed conversion steps")
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

            if args.validate:
                print("\n--- Validation ---")
                is_valid, warnings = validate_converted_3mf(out)
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
