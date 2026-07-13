#pragma once

#include <string>
#include <vector>

#include <boost/filesystem.hpp>

/**
 * @file TempFileGuard.hpp
 * @brief RAII guard that removes a list of temp files/dirs on destruction.
 */

/** RAII guard that removes a list of temp files/directories on destruction. */
struct TempFileGuard
{
    std::vector<std::string>& mFiles;

    explicit TempFileGuard(std::vector<std::string>& f) : mFiles(f) {}

    ~TempFileGuard() { cleanup(); }

    /** Remove all tracked files.  Exceptions during removal are silently
     *  swallowed — this is best-effort cleanup at shutdown. */
    void cleanup()
    {
        for (const auto& file : mFiles) {
            try {
                if (boost::filesystem::exists(file))
                    boost::filesystem::remove(file);
            } catch (...) {}
        }
    }

    // Prevent copy
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};
