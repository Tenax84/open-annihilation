// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// macOS: a dataless file has SF_DATALESS set, and the file manager can be
// asked to download it without waiting for the bytes.

#include "oa/platform/dataless.hpp"

#include <sys/stat.h>

#import <Foundation/Foundation.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace oa::platform {

bool stat_flags_dataless(uint32_t flags) noexcept {
    return (flags & SF_DATALESS) != 0;
}

// stat reads the flags without fetching the file.
bool host_file_is_dataless(const std::filesystem::path& path) {
    struct stat facts{};
    if (::stat(path.c_str(), &facts) != 0)
        return false;
    return stat_flags_dataless(facts.st_flags);
}

void host_download_files(
    const std::vector<std::filesystem::path>& paths, const std::atomic<bool>& stop
) {
    @autoreleasepool {
        NSFileManager* files = [NSFileManager defaultManager];
        for (const std::filesystem::path& path : paths) {
            if (stop.load(std::memory_order_relaxed))
                return;
            NSString* text = [NSString stringWithUTF8String:path.c_str()];
            if (text == nil)
                continue;
            NSURL* url = [NSURL fileURLWithPath:text isDirectory:NO];
            if (url == nil)
                continue;
            NSError* failure = nil;
            (void)[files startDownloadingUbiquitousItemAtURL:url error:&failure];
        }
    }
}

} // namespace oa::platform
