// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// mutator pack and mutator check. Each refuses a package that is not a
// mutator, and otherwise runs the same pack or check as the generic command.

#include "command.hpp"

#include "arguments.hpp"

#include "oa/app/package_install.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace install = oa::app::package_install;

/// Tells whether two texts are equal with ASCII letters folded.
///
/// @param left one text
/// @param right the other
/// @return true when they are equal
bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

/// Returns a path made from UTF-8 text.
///
/// @param text the text
/// @return the path
fs::path path_of(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Tells whether a folder's top holds oamut.yaml.
///
/// @param folder the folder
/// @return true when a file there is named oamut.yaml, without regard to case
bool top_holds_oamut(const fs::path& folder) {
    std::error_code error;
    if (!fs::is_directory(fs::symlink_status(folder, error)))
        return false;
    const fs::directory_iterator end;
    for (fs::directory_iterator cursor(folder, error); !error && cursor != end;
         cursor.increment(error)) {
        std::error_code status_error;
        if (fs::is_directory(cursor->symlink_status(status_error)))
            continue;
        const auto name = cursor->path().filename().generic_u8string();
        const std::string text(name.begin(), name.end());
        if (same_ascii(text, "oamut.yaml"))
            return true;
    }
    return false;
}

/// Prints a usage failure for a mutator command.
///
/// @param output where it is printed
/// @param command `mutator pack` or `mutator check`
/// @param problem what is wrong
void report_mutator_usage(Output& output, std::string_view command, std::string_view problem) {
    output.err << "oa-tool " << command << ": " << problem << '\n';
    output.err << "run 'oa-tool help " << command << "'\n";
}

} // namespace

int run_mutator_pack(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"out", true, false},
        {"force", false, false},
        {"game-dir", true, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed || parsed->positional.size() != 1)
        return run_pack(arguments, output);
    if (!top_holds_oamut(path_of(parsed->positional[0]))) {
        report_mutator_usage(output, "mutator pack", "the folder holds no oamut.yaml");
        return exit_usage;
    }
    return run_pack(arguments, output);
}

int run_mutator_check(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"game-dir", true, false},
        {"mod", true, true},
        {"accept-unimplemented-hacks", false, false},
        {"json", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed || parsed->positional.size() != 1)
        return run_check(arguments, output);
    const install::PackageKind* kind = install::kind_for_file(path_of(parsed->positional[0]));
    if (kind == nullptr || std::string_view{kind->name} != "oamut") {
        report_mutator_usage(output, "mutator check", "the file is not a .oamut");
        return exit_usage;
    }
    return run_check(arguments, output);
}

} // namespace oa::tool
