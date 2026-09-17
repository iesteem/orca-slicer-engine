/**
 * @file TestEngineRun.hpp
 * @brief Shared test helper: owns the temp-file list that SliceEngine keeps
 * a reference to, without leaking it.
 *
 * The old pattern `make_unique<SliceEngine>(cfg, *(new std::vector<std::string>()))`
 * heap-allocated the vector per call and never freed it (SliceEngine stores a
 * reference; nothing owns the allocation). EngineRun keeps both objects with
 * a destruction order that satisfies the reference: members die in reverse
 * declaration order, so `engine` is destroyed BEFORE `temp_files`.
 *
 * Pointer-like operators keep existing call sites (`e->stats()`, `*e`)
 * unchanged.
 */

#pragma once

#include <memory>
#include <vector>

#include "SliceEngine.hpp"

struct EngineRun
{
    std::vector<std::string> temp_files;
    std::unique_ptr<SliceEngine> engine;

    SliceEngine* operator->() const { return engine.get(); }
    SliceEngine& operator*() const { return *engine; }
    explicit operator bool() const { return static_cast<bool>(engine); }
    SliceEngine* get() const { return engine.get(); }
};
