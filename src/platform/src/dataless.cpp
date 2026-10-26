// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/platform/dataless.hpp"

#include <iostream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

// OA_MACOS_DATALESS_FILES: macOS, where dataless_mac.mm asks the system.
#if defined(OA_MACOS_DATALESS_FILES)
namespace oa::platform {
bool host_file_is_dataless(const std::filesystem::path& path);
void host_download_files(
    const std::vector<std::filesystem::path>& paths, const std::atomic<bool>& stop
);
} // namespace oa::platform
#else
namespace oa::platform {
namespace {

bool host_file_is_dataless(const std::filesystem::path&) {
    return false;
}

} // namespace

bool stat_flags_dataless(uint32_t) noexcept {
    return false;
}

} // namespace oa::platform
#endif

namespace oa::platform {
namespace {

void consider_file(
    const std::filesystem::path& path,
    FileResidenceProbe probe,
    void* context,
    EvictedFiles& totals,
    std::vector<std::filesystem::path>* evicted
) {
    FileResidence residence;
    if (probe == nullptr || !probe(path, residence, context))
        return;
    ++totals.files;
    if (!residence.dataless)
        return;
    ++totals.evicted;
    if (evicted != nullptr)
        evicted->push_back(path);
}

bool stopped(const std::atomic<bool>* stop) {
    return stop != nullptr && stop->load(std::memory_order_relaxed);
}

void walk_root(
    const std::filesystem::path& root,
    FileResidenceProbe probe,
    void* context,
    EvictedFiles& totals,
    std::vector<std::filesystem::path>* evicted,
    const std::atomic<bool>* stop
) {
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error)
        return;
    if (std::filesystem::is_regular_file(root, error)) {
        consider_file(root, probe, context, totals, evicted);
        return;
    }
    std::filesystem::recursive_directory_iterator cursor(
        root, std::filesystem::directory_options::skip_permission_denied, error
    );
    if (error)
        return;
    const std::filesystem::recursive_directory_iterator end;
    while (cursor != end && !stopped(stop)) {
        error.clear();
        if (cursor->is_regular_file(error) && !error)
            consider_file(cursor->path(), probe, context, totals, evicted);
        error.clear();
        cursor.increment(error);
        if (error)
            return;
    }
}

// The startup line: how many files are not on this Mac, of how many.
[[maybe_unused]] std::string evicted_line(const EvictedFiles& counted, std::string_view where) {
    const std::string place(where);
    if (counted.evicted == 0)
        return "open-annihilation: all " + std::to_string(counted.files) + " files " + place +
               " are on this Mac\n";
    return "open-annihilation: " + std::to_string(counted.evicted) + " of " +
           std::to_string(counted.files) + " files " + place +
           " are in iCloud and not on this Mac; asked macOS to download them\n";
}

} // namespace

struct EvictedFetch::State {
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> stop{false};
};

EvictedFiles count_evicted_files(
    std::span<const std::filesystem::path> roots,
    FileResidenceProbe probe,
    void* context,
    std::vector<std::filesystem::path>* evicted,
    const std::atomic<bool>* stop
) {
    EvictedFiles totals;
    if (probe == nullptr)
        return totals;
    for (const std::filesystem::path& root : roots) {
        if (stopped(stop))
            break;
        walk_root(root, probe, context, totals, evicted, stop);
    }
    return totals;
}

bool host_reports_dataless_files() noexcept {
#if defined(OA_MACOS_DATALESS_FILES)
    return true;
#else
    return false;
#endif
}

bool system_file_residence(const std::filesystem::path& path, FileResidence& residence, void*) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;
    residence.dataless = host_file_is_dataless(path);
    return true;
}

EvictedFetch::EvictedFetch() : state_(std::make_unique<State>()) {
}

EvictedFetch::~EvictedFetch() {
    if (state_ == nullptr)
        return;
    state_->stop.store(true, std::memory_order_relaxed);
    if (state_->thread.joinable())
        state_->thread.join();
}

void EvictedFetch::start(std::vector<std::filesystem::path> roots, std::string_view where) {
#if !defined(OA_MACOS_DATALESS_FILES)
    (void)roots;
    (void)where;
#else
    std::lock_guard lock(state_->mutex);
    if (state_->thread.joinable())
        return;
    const std::string place(where);
    // A thread that cannot start leaves the files to download as they are read.
    try {
        state_->thread = std::thread([roots = std::move(roots), place, &stop = state_->stop] {
            std::vector<std::filesystem::path> evicted;
            const EvictedFiles counted =
                count_evicted_files(roots, system_file_residence, nullptr, &evicted, &stop);
            if (stop.load(std::memory_order_relaxed))
                return;
            std::cerr << evicted_line(counted, place);
            if (!evicted.empty())
                host_download_files(evicted, stop);
        });
    } catch (const std::system_error&) {
    }
#endif
}

void start_startup_evicted_fetch(std::vector<std::filesystem::path> roots) {
    static EvictedFetch fetch;
    fetch.start(std::move(roots), "under Mods, Maps, Languages, Mutators, Games and Expansions");
}

} // namespace oa::platform
