// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every installed FBI, weapon, feature, download, gamedata, GUI and OTA text
// writes back unchanged, and one new key in its first section leaves every
// other top-level section's body hash where it was.

#include "oa/formats/tdf.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/write.hpp"
#include "oa/test/game_assets.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

[[nodiscard]] bool
same_properties(const oa::formats::tdf::Block* left, const oa::formats::tdf::Block* right) {
    using namespace oa::formats::tdf;
    if (property_count(left) != property_count(right))
        return false;
    for (uint32_t index = 0; index < property_count(left); ++index) {
        const auto place = static_cast<int32_t>(index);
        const char* left_key = property_key_at(left, place);
        const char* right_key = property_key_at(right, place);
        const char* left_value = property_value_at(left, place);
        const char* right_value = property_value_at(right, place);
        if (left_key == nullptr || right_key == nullptr || left_value == nullptr ||
            right_value == nullptr)
            return false;
        if (std::strcmp(left_key, right_key) != 0 || std::strcmp(left_value, right_value) != 0)
            return false;
        if (left->properties[index].order != right->properties[index].order)
            return false;
    }
    return true;
}

[[nodiscard]] bool
same_tree(const oa::formats::tdf::Block* left, const oa::formats::tdf::Block* right) {
    if (left == nullptr || right == nullptr)
        return left == right;
    if (std::strcmp(left->name, right->name) != 0)
        return false;
    if (!same_properties(left, right))
        return false;
    if (left->child_count != right->child_count)
        return false;
    for (uint32_t index = 0; index < left->child_count; ++index) {
        if (!same_tree(left->children[index], right->children[index]))
            return false;
    }
    return true;
}

/// Writes one installed file back and checks it. Returns false, after printing the path and the reader's
/// reason, when the reader refuses the file: it has no document to write, so it is skipped, not failed.
bool check_file(const oa::AssetStore& assets, const std::string& path) {
    using namespace oa::formats::tdf;
    const auto contents = assets.load_file_contents(path);
    if (!contents) {
        check(false, path + " did not load");
        return true;
    }
    OwnedDocument document;
    ParseError error{};
    const auto bytes =
        std::string_view(reinterpret_cast<const char*>(contents->data()), contents->size());
    if (!document.parse(bytes, &error)) {
        std::printf(
            "skipped %s: the reader refuses it: %s\n", path.c_str(), describe(error).c_str()
        );
        return false;
    }
    const auto unedited = write_text(document.get());
    check(unedited.has_value(), path + " unedited write was refused");
    if (!unedited)
        return true;
    const std::string stored(document.get()->text, document.get()->text_length);
    check(*unedited == stored, path + " unedited write changed the text");

    Block* root = editable_root(document.get());
    if (root == nullptr || root->child_count == 0)
        return true;
    std::vector<uint32_t> other_hashes;
    other_hashes.reserve(root->child_count - 1u);
    for (uint32_t index = 1; index < root->child_count; ++index)
        other_hashes.push_back(root->children[index]->body_hash);

    const EditStatus status = set_value(document.get(), root->children[0], "oa_edit_mark", "1");
    check(status == EditStatus::ok, path + " could not set oa_edit_mark");
    if (status != EditStatus::ok)
        return true;
    const auto edited = write_text(document.get());
    check(edited.has_value(), path + " edited write was refused");
    if (!edited)
        return true;
    OwnedDocument again;
    ParseError again_error{};
    if (!again.parse(*edited, &again_error)) {
        check(false, path + " edited text did not parse: " + describe(again_error));
        return true;
    }
    check(same_tree(document.root(), again.root()), path + " edited text read back differently");
    check(
        again.root() != nullptr && again.root()->child_count == root->child_count,
        path + " edited text changed the section count"
    );
    if (again.root() == nullptr)
        return true;
    for (uint32_t index = 0; index < other_hashes.size(); ++index) {
        const Block* section = child_at(again.root(), index + 1u);
        check(
            section != nullptr && section->body_hash == other_hashes[index],
            path + " changed another section's body hash"
        );
    }
    return true;
}

} // namespace

int main() {
    const oa::AssetStore assets = oa::test::require_game_assets("installed TDF writer");

    const struct {
        const char* directory;
        const char* extension;
        bool recursive;
    } kinds[] = {
        {"units", ".fbi", false},
        {"weapons", ".tdf", false},
        {"features", ".tdf", true},
        {"download", ".tdf", false},
        {"gamedata", ".tdf", false},
        {"guis", ".gui", false},
        {"maps", ".ota", false},
    };

    int checked = 0;
    int skipped = 0;
    for (const auto& kind : kinds) {
        const std::vector<std::string> paths =
            kind.recursive ? assets.list_effective_recursive(kind.directory, kind.extension)
                           : assets.list_effective(kind.directory, kind.extension);
        for (const std::string& path : paths) {
            if (check_file(assets, path))
                ++checked;
            else
                ++skipped;
        }
    }
    std::printf(
        "installed-tdf-writer: checked %d files, skipped %d the reader refuses\n", checked, skipped
    );
    if (checked == 0) {
        std::fprintf(stderr, "FAIL: no files were checked\n");
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
