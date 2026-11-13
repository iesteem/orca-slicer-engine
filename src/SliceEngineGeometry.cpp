// Part of the SliceEngine class, split from SliceEngine.cpp on 2026-11-13
// (pure code move, no behavior change). Domain: run-once global preprocessing
// stages: instance-Z baking into meshes, arrange order, extruder params,
// plate-thumbnail decoding, geometric plate-membership rebuild (P-24), and
// output-path resolution.

#include "SliceEngine.hpp"
#include "PlateGrid.hpp"
#include "Utils.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
// Direct uses below (IWYU): boost::filesystem / system / uintmax_t; Slic3r::png
#include <boost/filesystem.hpp>

using namespace Slic3r;

// ----------------------------------------------------------------------------
// File-local free-function forward declarations.
//
// These helpers do not touch any SliceEngine member state (pure input→output),
// so they are kept as file-local `static` functions rather than members. They
// are defined further down but declared here because call sites in member
// functions appear before their definitions.
// ----------------------------------------------------------------------------
static void decode_one_plate_thumbnail(Slic3r::PlateData& pd);

void SliceEngine::bake_instance_z_into_mesh()
{
    // Relocate each object's Z so it survives libslic3r's Print::apply, which
    // zeroes the Z component of every instance transform ("Z offset is
    // discarded to ensure first layer starts at Z=0", PrintApply.cpp:155).
    // An object stored with instance.z != 0 would have that Z silently
    // dropped, clipping whatever the author placed above/below the bed.
    //
    // Strategy: move all Z information out of instance space into mesh-vertex
    // space, where Print::apply cannot touch it. Two phases per object:
    //
    //   Step 1 -- bake the existing instance.z offset into the mesh, then
    //             zero every instance.z. After this, instance.z is 0 for all
    //             instances of this object, and the mesh has been translated
    //             by the equivalent local-space Z.
    //
    //   Step 2 -- apply the desktop "intentional sinking" rule
    //             (allow_negative_z=true semantics from ModelObject::
    //              ensure_on_bed) to decide whether the object also needs
    //             raising, and if so bake that raise into the mesh too.
    //
    // Single-part sinking rule (mirrors Model.cpp:1829-1835):
    //   - min_z >= SINKING_Z_THRESHOLD (-0.001)  -> already on bed, no raise
    //   - max_z < 0                              -> fully buried, raise to bed
    //   - min_z < 0 AND max_z >= 0              -> straddling: author
    //                                              intentionally sank it,
    //                                              leave in place
    // Multi-part rule (mirrors Model.cpp:1836-1840): raise only when the
    // whole object's max_z < SINKING_MIN_Z_THRESHOLD (0.05). Multi-part
    // objects do not get the "intentional straddling" carve-out -- only
    // their max_z matters.
    //
    // Note (assumption): baking Z into the mesh assumes every instance of
    // this ModelObject shares the same rotation. libslic3r's ModelObject
    // uses one mesh shared across instances, and our world->local Z
    // conversion uses instances.front()'s rotation matrix. Per-instance
    // rotation with non-zero Z is not supported by this path. Current
    // callers (3MF import + arrange) never produce that combination; if
    // they ever do, the bake will be correct only for the first instance.
    //
    // Side effect on the world position of the object: ideally zero. We
    // compensate every instance-space mutation with an equal and opposite
    // mesh-space mutation, so the final world-space Z equals what
    // ensure_on_bed(true) would have produced -- but lives entirely in
    // mesh-vertex space, safe from Print::apply.
    for (ModelObject* obj : m_model.objects)
    {
        if (obj->instances.empty())
            continue;
        // Force fresh bounding-box computation. The cache may be stale from
        // 3MF import before instance transforms were finalized.
        obj->invalidate_bounding_box();

        // --- Step 0: sanitize non-finite instance offsets (P-37) ---
        // MSVC-based producers serialize NaN instance translations as the
        // literal "-nan(ind)" (observed: Snapmaker_Orca-2.4.1 project export,
        // fe47c91147985294.3mf item 8), which glibc strtod parses back to
        // NaN. Both Step 1's magnitude guard below and the bounding-box
        // merge are NaN-blind (NaN compares false; std::min/max return the
        // original operand), so an unsanitized NaN lets the object's z range
        // collapse to the defined-empty box and Step 2 "raises" the mesh by
        // +DBL_MAX (z_shift=1.79769e+308), flinging it outside the build
        // volume and failing the whole plate. The desktop survives the same
        // file because Print::apply unconditionally zeroes instance Z before
        // geometry is built (PrintApply.cpp:155) — sanitize the same way.
        bool offset_reset = false;
        for (ModelInstance* inst : obj->instances)
        {
            const Vec3d off = inst->get_offset();
            if (std::isfinite(off.x()) && std::isfinite(off.y()) && std::isfinite(off.z()))
                continue;
            inst->set_offset(Vec3d(std::isfinite(off.x()) ? off.x() : 0.0,
                                   std::isfinite(off.y()) ? off.y() : 0.0,
                                   std::isfinite(off.z()) ? off.z() : 0.0));
            offset_reset = true;
        }
        if (offset_reset)
        {
            BOOST_LOG_TRIVIAL(warning)
                << "bake_instance_z_into_mesh: object \"" << obj->name
                << "\" had a non-finite instance translation; reset to 0 (desktop "
                   "Print::apply discards instance Z as well)";
            m_stats.issues.push_back(make_warning(
                -1, "INSTANCE_OFFSET_NON_FINITE",
                "Object \"" + obj->name + "\": instance translation contained non-finite "
                "values (NaN/inf, e.g. a producer-written \"-nan(ind)\"); they were "
                "reset to 0. Desktop discards instance Z the same way when slicing.",
                obj->name,
                "Re-save the project in a current slicer version to fix the stored transform."));
        }

        // Local helper: convert a world-space (0,0,dz) shift to local mesh
        // space using the front instance's rotation (no offset component).
        // translate() writes mesh vertices in local space; if the instance
        // is rotated, the world-Z axis is not aligned with local-Z, so the
        // shift must be un-rotated first.
        auto world_z_to_local = [obj](double dz) {
            auto rot_no_off = obj->instances.front()->get_transformation().get_matrix_no_offset();
            Vec3d world_shift(0, 0, dz);
            return rot_no_off.inverse() * world_shift;
        };

        // --- Step 1: bake existing instance Z offset into mesh vertices ---
        double inst_z = 0.0;
        for (ModelInstance* inst : obj->instances)
        {
            inst_z = inst->get_offset().z();
            if (std::abs(inst_z) > 1e-4)
                break;
        }

        if (std::abs(inst_z) > 1e-4)
        {
            Vec3d local_shift = world_z_to_local(inst_z);
            obj->translate(local_shift.x(), local_shift.y(), local_shift.z());
            // Zero all instance Z offsets.
            for (ModelInstance* inst : obj->instances)
            {
                Vec3d off = inst->get_offset();
                inst->set_offset(Vec3d(off.x(), off.y(), 0.0));
            }
        }

        // --- Step 2: apply the desktop sinking rule, baking any raise into the mesh ---
        //
        // We do NOT call ModelObject::ensure_on_bed here. ensure_on_bed's only
        // side effect is to mutate instance offset (translate_instances ->
        // set_offset, Model.cpp:1845-1862); it never touches mesh vertices.
        // Using it would force the same "call then undo the side effect"
        // pattern the previous implementation relied on -- readable only if
        // the reader remembers that detail, and brittle across libslic3r
        // versions if that contract ever changes.
        //
        // Instead we replicate the allow_negative_z=true decision (Model.cpp:
        // 1829-1841) locally to compute z_shift, then bake z_shift into the
        // mesh ourselves. Same final world-space Z as ensure_on_bed(true),
        // but the raise lives in mesh-vertex space where Print::apply cannot
        // discard it.
        constexpr double kSinkingZThreshold     = -0.001;  // Model.hpp: SINKING_Z_THRESHOLD
        constexpr double kSinkingMinZThreshold   = 0.05;   // Model.hpp: SINKING_MIN_Z_THRESHOLD
        constexpr double kEpsilon                 = 1e-4;
        // Log gate deliberately tighter than kEpsilon: a z_shift below
        // kEpsilon changes nothing geometrically, but shifts as small as
        // 1e-6 are still worth logging for the bake audit trail.
        constexpr double kLogGateEps              = 1e-6;

        {
            obj->invalidate_bounding_box();
            const double min_z_before = obj->min_z();
            const double max_z_before = obj->max_z();  // for diagnostics; geometry never mutates here
            const size_t  parts       = obj->parts_count();

            // P-37: an invalid or non-finite z range makes the sinking rule's
            // comparisons meaningless. The defined-empty box (NaN vertices
            // merging into it, or an object whose volumes were all dropped at
            // load) reports max_z=-DBL_MAX; the multi-part arm below would
            // then "raise" the mesh by +DBL_MAX and teleport it outside the
            // build volume. Treat an unusable range as "leave the mesh alone"
            // — an object with no measurable extent cannot be sunk on purpose.
            const bool z_range_usable = std::isfinite(min_z_before)
                                     && std::isfinite(max_z_before)
                                     && min_z_before <= max_z_before;
            if (!z_range_usable)
            {
                BOOST_LOG_TRIVIAL(warning)
                    << "bake_instance_z_into_mesh: object \"" << obj->name
                    << "\" has an unusable z range [" << min_z_before << ", "
                    << max_z_before << "] (non-finite or inverted); sinking rule skipped";
                m_stats.issues.push_back(make_warning(
                    -1, "OBJECT_Z_RANGE_INVALID",
                    "Object \"" + obj->name + "\": bounding-box z range is non-finite or "
                    "inverted ([" + std::to_string(min_z_before) + ", "
                    + std::to_string(max_z_before) + "]); the on-bed placement rule was "
                    "skipped for it. This usually accompanies degenerate or discarded "
                    "geometry, or a corrupted instance transform.",
                    obj->name,
                    "Inspect the object in a desktop slicer; repair or remove it."));
            }

            // Replicate ensure_on_bed(allow_negative_z=true):
            //   single-part: raise only if (min_z >= -0.001) or (max_z < 0).
            //                Straddling (min_z < 0 AND max_z >= 0) is left alone.
            //   multi-part:  raise only if max_z < 0.05.
            // Straddling single-part objects are the author's intentional
            // sinking and are preserved exactly; the multi-part rule has no
            // such carve-out, only an absolute max_z cutoff.
            double z_shift = 0.0;
            if (z_range_usable && parts == 1)
            {
                if (min_z_before >= kSinkingZThreshold || max_z_before < 0.0)
                    z_shift = -min_z_before;
            }
            else if (z_range_usable)
            {
                if (max_z_before < kSinkingMinZThreshold)
                    z_shift = kSinkingMinZThreshold - max_z_before;
            }

            if (std::abs(z_shift) > kEpsilon)
            {
                Vec3d local_shift = world_z_to_local(z_shift);
                obj->translate(local_shift.x(), local_shift.y(), local_shift.z());
                // No instance offset change: the raise is entirely in the mesh.
            }

            obj->invalidate_bounding_box();
            if (std::abs(z_shift) > kLogGateEps || std::abs(min_z_before) > kLogGateEps)
            {
                BOOST_LOG_TRIVIAL(info) << "bake_instance_z_into_mesh: object \"" << obj->name
                                        << "\" inst_z=" << inst_z
                                        << " parts=" << parts
                                        << " z_shift=" << z_shift;
            }

            // Diagnostic warning when the object's stored Z sat below the bed.
            // Two distinct cases, kept separate because their semantics are
            // different and the previous combined text conflated them:
            //
            //   (a) z_shift != 0  -> The sinking rule fired. The object was
            //      either fully buried below the bed or its bottom was
            //      essentially on the bed and got snapped up. Either way we
            //      raised it; surface that so the silent correction is
            //      visible.
            //
            //   (b) z_shift == 0 AND min_z_before < 0  -> Only reachable for
            //      single-part objects: straddling (min_z<0 AND max_z>=0).
            //      This is a genuine author-intended sinking (e.g. a base
            //      flattened against the bed) and is preserved as stored.
            //      Multi-part objects with max_z >= 0.05 never raise, but
            //      they also don't reach here as "intentional" -- they were
            //      simply above the threshold, not sank -- so the
            //      "intentional" wording is only used for the single-part
            //      straddle case.
            //
            // We include the original instance.z in the message so a user
            // debugging can tell apart "stored with instance.z=0 and a
            // negative mesh" from "stored with instance.z=N and a mesh that
            // combined to a negative world Z" -- both produce the same
            // post-Step-1 mesh state, but the upstream project state differs.
            if (min_z_before < -kEpsilon)
            {
                if (std::abs(z_shift) > kEpsilon)
                {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf),
                                  "Object \"%s\" was stored below the print bed "
                                  "(min Z=%.2fmm, original instance Z=%.2fmm, parts=%zu); "
                                  "auto-raised by %.2fmm to sit on the bed.",
                                  obj->name.c_str(), min_z_before, inst_z, parts, z_shift);
                    m_stats.issues.push_back(make_warning(
                        -1, "OBJECT_BELOW_BED_ADJUSTED", buf, obj->name,
                        "If the sinking was intentional, reposition the object's Z in your slicer before exporting."));
                }
                else
                {
                    // Reached when no raise was applied but min_z_before < 0.
                    // Two sub-cases land here:
                    //   - single-part straddling (min_z<0 AND max_z>=0):
                    //     the author intentionally sank the base -- preserved
                    //     as stored, the "intentionally straddles" wording is
                    //     accurate.
                    //   - multi-part with max_z >= 0.05 and min_z < 0:
                    //     the multi-part rule did not fire because only max_z
                    //     matters; the object is partially below the bed but
                    //     was not deliberately sank in the single-part sense.
                    //     The wording below is slightly off for this sub-case
                    //     (it's not "intentional"), but we keep one issue code
                    //     to match the previous behavior. The "parts=N" field
                    //     lets users tell the two apart.
                    char buf[224];
                    std::snprintf(buf, sizeof(buf),
                                  "Object \"%s\" intentionally straddles the print bed "
                                  "(Z %.2f to %.2fmm, original instance Z=%.2fmm, parts=%zu); "
                                  "preserved as-is, the below-bed portion will not be printed.",
                                  obj->name.c_str(), min_z_before, max_z_before, inst_z, parts);
                    m_stats.issues.push_back(make_warning(
                        -1, "OBJECT_INTENTIONALLY_BELOW_BED", buf, obj->name,
                        "Reposition the object in your slicer if the below-bed portion was meant to be printed."));
                }
            }
        }
    }
}

// Global model/config preparation, run once from run() before the per-plate loop
// (after bake_instance_z_into_mesh, before decode_plate_thumbnails).

void SliceEngine::assign_arrange_order()
{
    int order = 1;
    for (ModelObject* obj : m_model.objects)
        for (ModelInstance* inst : obj->instances)
            inst->arrange_order = order++;
}

void SliceEngine::setup_extruder_params()
{
    int num_extruders = 0;
    if (m_config.has("filament_diameter"))
    {
        auto fd = m_config.option<ConfigOptionFloats>("filament_diameter");
        if (fd)
            num_extruders = static_cast<int>(fd->values.size());
    }
    Model::setExtruderParams(m_config, num_extruders);
}

// ============================================================================
// Stage 3: Global preprocessing (decode plate thumbnails)
// ============================================================================

void SliceEngine::decode_plate_thumbnails()
{
    // All allocations (vector<unsigned char>, PNG decode buffers) live inside
    // this try block. std::bad_alloc from a malformed/huge PNG is routed to
    // an early function exit rather than propagating -- the engine's calling
    // convention treats thumbnail decode as best-effort: a missing thumbnail
    // is acceptable, an uncaught exception is not (Snapmaker no-throw policy).
    BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: enter, plate_count=" << m_plate_data.size();
    try
    {
        for (auto& pd : m_plate_data)
            decode_one_plate_thumbnail(*pd);
    }
    catch (const std::exception& e)
    {
        // Most likely std::bad_alloc from a PNG buffer allocation. Log and
        // return normally -- partial progress (already-decoded plates) is
        // retained; remaining plates simply get no thumbnail.
        BOOST_LOG_TRIVIAL(error) << "decode_plate_thumbnails: aborted after exception: " << e.what();
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << "decode_plate_thumbnails: aborted after unknown exception";
    }
}

static void decode_one_plate_thumbnail(PlateData& pd)
{
    // PNG file signature (PNG spec section 5.2, first 8 bytes are fixed).
    static constexpr unsigned char PNG_SIGNATURE[] =
        {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static constexpr std::streamsize PNG_SIGNATURE_LEN = 8;

    // Max bytes accepted for a single plate thumbnail PNG.
    // Derivation: U1 display renders 300x300; measured PNG ~30 KB. 4 MB gives
    // ~130x headroom, covers future 600x600-class screens, still small enough
    // to prevent OOM from a malicious oversized file.
    static constexpr size_t MAX_PNG_SIZE = 4u * 1024u * 1024u;

    // Guard 1: path non-empty and ends in ".png".
    const std::string& raw_path = pd.thumbnail_file;
    if (raw_path.size() < 4 ||
        raw_path.compare(raw_path.size() - 4, 4, ".png") != 0) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", path not .png";
        return;
    }

    // Guard 2: canonicalise and verify path under system temp root.
    // canonical() resolves symlinks and normalises ".." components,
    // preventing path-traversal where the .3mf supplies a thumbnail_file
    // like "backup/../../etc/passwd.png". The system-temp-root check is
    // done against a known temp parent (e.g. "/tmp/") rather than
    // temp_directory_path() because TMPDIR may point to a custom location
    // that differs from libslic3r's own temp dir (backup_path).
    boost::system::error_code ec;
    const boost::filesystem::path resolved = boost::filesystem::canonical(raw_path, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", canonical failed (" << raw_path << ")";
        return;
    }

    // Verify the canonical path is under the system-wide temp root.
    const std::string resolved_str = resolved.string();
    bool under_temp = false;
    for (const char* prefix : {"/tmp/", "/var/tmp/", "/private/tmp/"})
        if (resolved_str.find(prefix) == 0) { under_temp = true; break; }
    if (!under_temp) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", path outside temp dir (" << resolved_str << ")";
        return;
    }

    // Guard 3: file size in [PNG_SIGNATURE_LEN, MAX_PNG_SIZE]. Files shorter
    // than the 8-byte signature must be rejected HERE: Guard 5 reads
    // file_bytes[0..7], so a 1..7 byte file that passes this guard is read
    // out of bounds (heap OOB read).
    const boost::uintmax_t file_sz = boost::filesystem::file_size(resolved, ec);
    if (ec || file_sz < static_cast<boost::uintmax_t>(PNG_SIGNATURE_LEN) || file_sz > MAX_PNG_SIZE) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", bad size " << file_sz;
        return;
    }
    const size_t sz = static_cast<size_t>(file_sz);

    // Guard 4: open and read full file. gcount() validates bytes read
    // (failbit on EOF makes "!ifs" unreliable).
    std::ifstream ifs(resolved.string(), std::ios::binary);
    if (!ifs.is_open()) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", cannot open";
        return;
    }

    std::vector<unsigned char> file_bytes(sz);
    // reinterpret_cast is required: istream::read takes char*, PNG bytes
    // are unsigned char. This is an ifstream API constraint, not type
    // punning; char may alias any type, safe.
    ifs.read(reinterpret_cast<char*>(file_bytes.data()), static_cast<std::streamsize>(sz));
    if (ifs.gcount() != static_cast<std::streamsize>(sz)) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", short read";
        return;
    }
    // ifs closes itself on destruction (RAII).

    // Guard 5: PNG magic bytes.
    bool sig_ok = true;
    for (size_t i = 0; i < static_cast<size_t>(PNG_SIGNATURE_LEN); ++i)
    {
        if (file_bytes[i] != PNG_SIGNATURE[i])
        {
            sig_ok = false;
            break;
        }
    }
    if (!sig_ok) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", bad PNG magic";
        return;
    }

    // Guard 6: PNG decode.
    Slic3r::png::ReadBuf buf{file_bytes.data(), file_bytes.size()};
    Slic3r::png::ImageColorscale img;
    if (!Slic3r::png::decode_colored_png(buf, img)) {
        BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: skip plate " << pd.plate_index
                                << ", PNG decode failed";
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "decode_plate_thumbnails: success plate " << pd.plate_index
                            << " (" << img.cols << "x" << img.rows << ")";

    // Populate plate_thumbnail only after successful decode (no half-state).
    pd.plate_thumbnail.set(static_cast<unsigned int>(img.cols), static_cast<unsigned int>(img.rows));

    const size_t src_bpp = static_cast<size_t>(img.bytes_per_pixel);
    for (size_t y = 0; y < img.rows; ++y)
    {
        for (size_t x = 0; x < img.cols; ++x)
        {
            const size_t src_idx = (y * img.cols + x) * src_bpp;
            const size_t dst_idx = (y * img.cols + x) * 4;
            pd.plate_thumbnail.pixels[dst_idx + 0] = img.buf[src_idx + 0];
            pd.plate_thumbnail.pixels[dst_idx + 1] = img.buf[src_idx + 1];
            pd.plate_thumbnail.pixels[dst_idx + 2] = img.buf[src_idx + 2];
            pd.plate_thumbnail.pixels[dst_idx + 3] = (src_bpp >= 4) ? img.buf[src_idx + 3] : 255;
        }
    }
}

// Stage 3 tail: resolve the export destination before the per-plate loop.
// generate_output_path (Utils) appends a timestamp suffix when the requested
// path is already occupied (multi-process collision protection). The artifact
// then lands elsewhere while rc stays 0 — surface the relocation so callers
// reading the requested path know to check stats().output_path() (JSON:
// print_info_total.output_file) instead of serving the stale leftover. The
// relocation itself is intentional and unchanged.
void SliceEngine::resolve_output_path()
{
    bool output_relocated = false;
    m_output_path = generate_output_path(m_cfg.input_file, m_cfg.output_base, &output_relocated);
    if (output_relocated)
        m_stats.issues.push_back(make_warning(-1, "OUTPUT_PATH_RELOCATED",
                                              "Output path already exists; artifact written to \"" + m_output_path + "\""));
}

// ============================================================================
// Stage 4: Process a single plate
// ============================================================================

void SliceEngine::rebuild_plate_membership()
{
    // P-22 guard: the synthesized single plate IS the membership ("plate 1 =
    // every printable instance", position-independent — see read_3mf_model).
    // A geometric rebuild would drop off-bed instances from its map, silently
    // skipping objects the desktop slices, and an all-off-bed file would
    // wrongly hit the PLATELESS_NO_PRINTABLE_INSTANCES hard-fail.
    if (m_synthesized_single_plate)
        return;

    // Degradation guard, same condition as setup_print_origin above: no
    // printable_area -> no grid geometry -> leave membership untouched. The
    // plates stay empty and the P-19 EMPTY_PLATE warning path applies.
    // (In practice unreachable: apply_printer_official_preset hard-fails
    // earlier when it cannot apply a printable_area.)
    const auto* pa = m_config.option<ConfigOptionPoints>("printable_area");
    if (!pa || pa->values.empty())
        return;
    BoundingBoxf bed_bbox;
    for (const Vec2d& pt : pa->values)
        bed_bbox.merge(pt);
    const double plate_width  = bed_bbox.size().x();
    const double plate_depth  = bed_bbox.size().y();
    const int    total_plates = static_cast<int>(m_plate_data.size());
    // Desktop parity: PartPlate::get_build_volume inflates the plate box by
    // BuildVolume::SceneEpsilon before intersect_instance() tests it.
    const double eps = Slic3r::BuildVolume::SceneEpsilon;

    // P-24 always-rebuild: the desktop GUI never restores plate membership
    // from the file — load_from_3mf_structure keeps the records-commented-out
    // ("we will rebuild later") and ends with reload_all_objects(), a full
    // geometric reassignment for every instance. Production file
    // 2d986d79661ffb62.3mf (third-party exporter, metadata shifted one grid
    // cell from geometry) proved trusting non-empty maps diverges from the
    // desktop, which slices all 8 plates while P-23 Fix A failed 3 of them.
    //
    // Snapshot BEFORE clearing so the tip can report an actual reassignment
    // (desktop-authored files keep geometry == metadata and stay silent).
    // Instance identity is (object index, instance index), NOT the identify_id
    // value: rebuilt loaded_ids use id().id, which never equals the file's
    // identify_ids, so a value-wise diff would always report a change.
    std::map<int, std::pair<int, int>> loaded_to_inst;  // loaded_id -> (obj idx, inst idx)
    for (size_t o = 0; o < m_model.objects.size(); ++o)
        for (size_t j = 0; j < m_model.objects[o]->instances.size(); ++j)
        {
            const int lid = static_cast<int>(m_model.objects[o]->instances[j]->loaded_id);
            if (lid > 0)
                loaded_to_inst.emplace(lid, std::make_pair(static_cast<int>(o),
                                                           static_cast<int>(j)));
        }
    // per plate_index -> printable instance identities resolved from the file's maps;
    // stale records (identify_id matching no instance) are ignored.
    std::map<int, std::set<std::pair<int, int>>> before;
    for (PlateData* pd : m_plate_data)
    {
        auto& ids = before[pd->plate_index];
        for (const auto& [key, inst_info] : pd->obj_inst_map)
        {
            const auto it = loaded_to_inst.find(inst_info.second);
            if (it != loaded_to_inst.end() &&
                m_model.objects[it->second.first]->instances[it->second.second]->printable)
                ids.insert(it->second);
        }
        pd->obj_inst_map.clear();
    }

    // Desktop loop (GUI PartPlateList::reload_all_objects): per printable
    // instance, convex-hull bounding box (world coords), first intersecting
    // plate in plate order wins; an instance hitting no plate lands on the
    // desktop's unprintable plate (here: dropped, with a warning).
    // A degenerate bbox (e.g. modifier-only object with empty convex hull)
    // compares NaN/false everywhere below and is left unenrolled — benign,
    // no UB (NaN FP comparisons are defined false).
    std::vector<orca::PlateBox2D> boxes;  // parallel to m_plate_data
    for (const PlateData* pd : m_plate_data)
        boxes.push_back(orca::compute_plate_box(pd->plate_index, total_plates,
                                                plate_width, plate_depth, eps));
    std::map<int, std::set<std::pair<int, int>>> after;   // plate_index -> identities
    std::map<std::pair<int, int>, int>          after_plate;  // identity -> plate_index
    std::vector<std::pair<std::pair<int, int>, std::string>> dropped;  // identity + object name
    int excluded = 0, key = 0;
    for (size_t o = 0; o < m_model.objects.size(); ++o)
    {
        ModelObject* obj = m_model.objects[o];
        for (size_t j = 0; j < obj->instances.size(); ++j)
        {
            ModelInstance* inst = obj->instances[j];
            if (!inst->printable)
            {
                // Desktop puts printable="0" instances on the unprintable
                // plate; they never join a real plate.
                ++excluded;
                continue;
            }
            const BoundingBoxf3 inst_bbox = obj->instance_convex_hull_bounding_box(j);
            bool assigned = false;
            for (size_t c = 0; c < m_plate_data.size(); ++c)
            {
                const orca::PlateBox2D& b = boxes[c];
                if (inst_bbox.max.x() >= b.xmin && inst_bbox.min.x() <= b.xmax &&
                    inst_bbox.max.y() >= b.ymin && inst_bbox.min.y() <= b.ymax)
                {
                    // P-22 enrollment pattern: key only needs uniqueness
                    // (engine consumers read value.second exclusively) and a
                    // running counter is collision-free on a cleared map.
                    // static_cast size_t->int narrowing is bounded by the
                    // instance count and id().id is globally unique, > 0.
                    inst->loaded_id = inst->id().id;
                    m_plate_data[c]->obj_inst_map.emplace(
                        ++key, std::make_pair(static_cast<int>(j),
                                              static_cast<int>(inst->id().id)));
                    const std::pair<int, int> identity(static_cast<int>(o),
                                                       static_cast<int>(j));
                    after[m_plate_data[c]->plate_index].insert(identity);
                    after_plate[identity] = m_plate_data[c]->plate_index;
                    assigned = true;
                    break;
                }
            }
            if (!assigned)
                dropped.emplace_back(std::make_pair(static_cast<int>(o),
                                                     static_cast<int>(j)), obj->name);
        }
    }

    // Off-all-plates printable instances: warning, non-blocking. Desktop
    // parity: GLCanvas3D gates the slice button on contained_min_one &&
    // !partlyOut — a fully-outside instance goes to the unprintable plate and
    // does NOT block slicing the remaining plates (only partly-outside does,
    // handled by run_build_volume_check's blocking errors).
    for (const auto& [identity, name] : dropped)
        m_stats.issues.push_back(make_warning(
            -1, "INSTANCE_NOT_ON_ANY_PLATE",
            "Object \"" + name + "\" (instance " +
                std::to_string(identity.second) +
                ") does not intersect any build plate and will not be printed",
            name, "Move the object onto a build plate before slicing."));

    // Reassigned = any printable instance whose plate changed (moved, gained,
    // or lost). Identical membership (every desktop-authored file) stays
    // silent; Case-style regression locks rely on this quiet path.
    int moved = 0;
    {
        std::set<std::pair<int, int>> all;
        for (const auto& [pi, ids] : before) all.insert(ids.begin(), ids.end());
        for (const auto& [pi, ids] : after) all.insert(ids.begin(), ids.end());
        for (const auto& identity : all)
        {
            const int pb = [&]() {
                for (const auto& [pi, ids] : before)
                    if (ids.count(identity)) return pi;
                return -1;
            }();
            const int pa2 = after_plate.count(identity) ? after_plate[identity] : -1;
            if (pb != pa2)
                ++moved;
        }
    }
    if (moved == 0 && dropped.empty())
        return;

    BOOST_LOG_TRIVIAL(info) << "Rebuilt plate membership geometrically: " << moved
                            << " instance(s) reassigned, " << dropped.size()
                            << " dropped (not on any plate), " << excluded
                            << " excluded as unprintable";
    if (moved > 0)
        m_stats.issues.push_back(make_tip(-1, "PLATE_MEMBERSHIP_REBUILT",
            "The 3MF file's plate metadata disagrees with plate geometry; "
            "instance membership was reassigned geometrically (desktop Orca "
            "behaviour): " + std::to_string(moved) + " reassigned, " +
            std::to_string(dropped.size()) + " dropped, " +
            std::to_string(excluded) + " excluded as unprintable."));
}

