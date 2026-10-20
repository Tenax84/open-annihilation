// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A scratch Mutators folder listed from its manifests: good mutators, a
// broken manifest, a folder with none, an oversized manifest, the
// installer's own folder, a .backup holding another id, a symbolic link, two
// folders with one id, a plain file left untouched, catalogue and file
// origin records, a damaged record, an oversized record, and a missing folder.

#include "oa/data/mutator_library/library.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace lib = oa::data::mutator_library;
namespace mut = oa::formats::oamut;

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("oa-data-mutator-library-")) {}

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

  private:

    fs::path path_;
};

/// Writes a file's bytes.
///
/// @param file the file
/// @param text its text
void write_text(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

/// A manifest that reads.
///
/// @param id the mutator's id
/// @param name the name players see
/// @return the manifest
mut::Manifest manifest_of(std::string id, std::string name) {
    mut::Manifest manifest;
    manifest.format = mut::format_version;
    manifest.id = std::move(id);
    manifest.name = std::move(name);
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ada";
    manifest.tags = {"economy"};
    manifest.works_with.push_back(mut::Condition{"*", {}, {}});
    manifest.layers = {mut::Layer::units};
    return manifest;
}

/// Writes a mutator's manifest, and a script the listing must not need.
///
/// @param folder the mutator's folder
/// @param manifest the manifest
void write_mutator(const fs::path& folder, const mut::Manifest& manifest) {
    write_text(folder / "oamut.yaml", mut::write_manifest(manifest));
    write_text(folder / "mutator.lua", "return {}\n");
}

/// Tells whether a problem names a folder.
///
/// @param listing the listing
/// @param phrase the text a problem holds
/// @return true when one problem holds it
bool problem_has(const lib::Listing& listing, std::string_view phrase) {
    for (const std::string& problem : listing.problems)
        if (problem.find(phrase) != std::string::npos)
            return true;
    return false;
}

void test_listing(const Scratch& scratch) {
    const fs::path mutators = scratch.path() / "Mutators";
    write_mutator(mutators / "calm-wind", manifest_of("calm-wind", "Calm Wind"));
    write_mutator(mutators / "steady-hands", manifest_of("steady-hands", "Steady Hands"));
    write_mutator(mutators / "wide-net", manifest_of("wide-net", "Wide Net"));
    write_mutator(mutators / "quiet-mark", manifest_of("quiet-mark", "Quiet Mark"));
    write_text(
        mutators / "calm-wind" / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: catalogue\n"
        "registry: ridge\n"
        "catalogue-id: calm-wind\n"
        "release: 7\n"
        "sha256: \"abc123\"\n"
        "installed: \"2000-01-01\"\n"
    );
    // A file origin may name catalogue keys. Only the SHA-256 is kept.
    write_text(
        mutators / "steady-hands" / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: file\n"
        "registry: ridge\n"
        "catalogue-id: steady-hands\n"
        "release: 4\n"
        "sha256: \"def456\"\n"
        "installed: \"2000-01-01\"\n"
    );
    write_text(
        mutators / "wide-net" / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: file\n"
        "sha256: \"eee789\"\n"
        "extra: 1\n"
        "installed: \"2000-01-01\"\n"
    );
    write_text(mutators / "quiet-mark" / ".oa-origin.yaml", std::string(4097, 'x'));
    write_text(
        mutators / "broken-one" / "oamut.yaml",
        "oamut: 1\n"
        "id: \"broken-one\"\n"
        "name: \"Broken\"\n"
        "version: \"1\"\n"
        "author:\n"
        "  name: \"Ada\"\n"
    );
    fs::create_directories(mutators / "empty-one");
    write_text(mutators / "huge-yaml" / "oamut.yaml", std::string((256 * 1024) + 1, 'y'));
    write_mutator(mutators / "hands-copy", manifest_of("steady-hands", "Steady Hands"));
    write_mutator(mutators / "alpha-copy", manifest_of("other-id", "Other"));
    write_mutator(mutators / "beta-copy", manifest_of("other-id", "Other"));
    fs::create_directories(mutators / ".oamut-staging-x");
    write_text(
        mutators / ".oamut-staging-x" / "oamut.yaml",
        mut::write_manifest(manifest_of("staged", "Staged"))
    );
    write_text(
        mutators / "steady-hands" / ".backup" / "oamut.yaml",
        mut::write_manifest(manifest_of("hidden-backup", "Hidden"))
    );
    const fs::path notes = mutators / "notes.txt";
    const std::string note = "the player's own notes\n";
    write_text(notes, note);
    const fs::path elsewhere = scratch.path() / "elsewhere";
    write_mutator(elsewhere, manifest_of("linked-away", "Linked Away"));
    std::error_code link_error;
    fs::create_directory_symlink(elsewhere, mutators / "linked-hands", link_error);

    const lib::Listing listing = lib::list_installed(mutators);
    OA_CHECK(listing.mutators.size() == 5);
    if (listing.mutators.size() == 5) {
        OA_CHECK(listing.mutators[0].manifest.id == "calm-wind");
        OA_CHECK(listing.mutators[1].manifest.id == "other-id");
        OA_CHECK(listing.mutators[2].manifest.id == "quiet-mark");
        OA_CHECK(listing.mutators[3].manifest.id == "steady-hands");
        OA_CHECK(listing.mutators[4].manifest.id == "wide-net");
    }
    OA_CHECK(lib::find(listing, "calm-wind") != nullptr);
    OA_CHECK(lib::find(listing, "other-id") != nullptr);
    OA_CHECK(lib::find(listing, "quiet-mark") != nullptr);
    OA_CHECK(lib::find(listing, "steady-hands") != nullptr);
    OA_CHECK(lib::find(listing, "wide-net") != nullptr);
    OA_CHECK(lib::find(listing, "linked-away") == nullptr);
    OA_CHECK(lib::find(listing, "hidden-backup") == nullptr);
    OA_CHECK(lib::find(listing, "staged") == nullptr);
    OA_CHECK(lib::find(listing, "broken-one") == nullptr);
    OA_CHECK(lib::find(listing, "no-such") == nullptr);

    const lib::InstalledMutator* calm = lib::find(listing, "calm-wind");
    OA_CHECK(calm != nullptr && calm->folder_name == "calm-wind");
    OA_CHECK(calm != nullptr && calm->package_sha256 == "abc123");
    OA_CHECK(calm != nullptr && calm->registry == "ridge");
    OA_CHECK(calm != nullptr && calm->catalogue_id == "calm-wind");
    OA_CHECK(calm != nullptr && calm->release == 7);
    OA_CHECK(calm != nullptr && calm->manifest.name == "Calm Wind");

    const lib::InstalledMutator* steady = lib::find(listing, "steady-hands");
    OA_CHECK(steady != nullptr && steady->folder_name == "steady-hands");
    OA_CHECK(steady != nullptr && steady->package_sha256 == "def456");
    OA_CHECK(steady != nullptr && steady->registry.empty());
    OA_CHECK(steady != nullptr && steady->catalogue_id.empty());
    OA_CHECK(steady != nullptr && steady->release == 0);

    const lib::InstalledMutator* wide = lib::find(listing, "wide-net");
    OA_CHECK(wide != nullptr && wide->package_sha256.empty());
    OA_CHECK(wide != nullptr && wide->registry.empty());
    OA_CHECK(wide != nullptr && wide->release == 0);

    const lib::InstalledMutator* quiet = lib::find(listing, "quiet-mark");
    OA_CHECK(quiet != nullptr && quiet->package_sha256.empty());
    OA_CHECK(quiet != nullptr && quiet->registry.empty());

    const lib::InstalledMutator* other = lib::find(listing, "other-id");
    OA_CHECK(other != nullptr && other->folder_name == "alpha-copy");

    OA_CHECK(problem_has(listing, "broken-one: oamut.yaml:"));
    OA_CHECK(problem_has(listing, "tags"));
    OA_CHECK(problem_has(listing, "empty-one: oamut.yaml: it cannot be read"));
    OA_CHECK(problem_has(listing, "huge-yaml: oamut.yaml: it is larger than 256 KiB"));
    OA_CHECK(problem_has(listing, "hands-copy: it has the same id steady-hands as steady-hands"));
    OA_CHECK(problem_has(listing, "beta-copy: it has the same id other-id as alpha-copy"));
    OA_CHECK(!problem_has(listing, "staged"));
    OA_CHECK(!problem_has(listing, "hidden-backup"));
    if (!link_error)
        OA_CHECK(!problem_has(listing, "linked-away") && !problem_has(listing, "linked-hands"));

    OA_CHECK(fs::file_size(notes) == note.size());
    {
        std::ifstream in(notes, std::ios::binary);
        const std::string kept{std::istreambuf_iterator<char>{in}, {}};
        OA_CHECK(kept == note);
    }

    const lib::Listing missing = lib::list_installed(scratch.path() / "no-mutators");
    OA_CHECK(missing.mutators.empty());
    OA_CHECK(missing.problems.empty());
}

} // namespace

int main() {
    const Scratch scratch;
    test_listing(scratch);
    return oa::test::check_exit_status();
}
