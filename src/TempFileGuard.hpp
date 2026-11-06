#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

// RAII guard that removes a list of temp files on destruction
struct TempFileGuard
{
    std::vector<std::string>& mFiles;
    explicit TempFileGuard(std::vector<std::string>& f) : mFiles(f)
    {
    }
    ~TempFileGuard()
    {
        cleanup();
    }
    void cleanup()
    {
        // Forensics escape hatch (precedent P-27): when a run fails, the
        // per-plate plate_N.gcode files are the primary evidence for diffing
        // conflict/outside footprints against a desktop reference, but this
        // guard deletes them on unwind. ORCA_KEEP_FAILED_GCODE=1 keeps every
        // tracked temp file and prints its path to stderr. Debug aid only —
        // callers must not depend on it: kept files sit in the per-run temp
        // dir, which the OS/next cleanup pass may still sweep.
        if (std::getenv("ORCA_KEEP_FAILED_GCODE") != nullptr)
        {
            for (const auto& file : mFiles)
                std::fprintf(stderr, "[ORCA_KEEP_FAILED_GCODE] kept: %s\n", file.c_str());
            return;
        }
        for (const auto& file : mFiles)
        {
            try
            {
                if (boost::filesystem::exists(file))
                    boost::filesystem::remove(file);
            }
            catch (...)
            {
            }
        }
    }
    // Prevent copy
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};
