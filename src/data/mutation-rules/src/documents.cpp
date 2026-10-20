// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The documents one pipeline run edits. Paths are stored with '/' separators
// and compared without ASCII case, so a second add of the same file is refused
// whether the caller wrote the path with '\' or with different capitals.
#include "oa/data/mutation_rules/documents.hpp"

#include "oa/formats/tdf/edit.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutation_rules {
namespace tdf = oa::formats::tdf;

namespace {

/// Folds one path byte: ASCII capitals become lower case, and '\' becomes '/'.
///
/// @param byte a byte of a path
/// @return the folded byte
char fold_path_byte(char byte) {
    if (byte == '\\')
        return '/';
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

/// Folds one text byte to lower case. Slash characters are left as they are.
///
/// @param byte a byte of a name or suffix
/// @return the folded byte
char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

/// Reports whether two paths are the same without regard to ASCII case or slash direction.
///
/// @param left a path
/// @param right another path
/// @return true when they fold to the same bytes
bool same_path(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_path_byte(left[offset]) != fold_path_byte(right[offset]))
            return false;
    }
    return true;
}

/// Reports whether `text` begins with `prefix`, ignoring ASCII case and slash direction.
///
/// @param text a path
/// @param prefix a folder prefix, including its trailing slash
/// @return true when the prefix matches
bool starts_with_folded(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size())
        return false;
    for (size_t offset = 0; offset < prefix.size(); ++offset) {
        if (fold_path_byte(text[offset]) != fold_path_byte(prefix[offset]))
            return false;
    }
    return true;
}

/// Reports whether `text` ends with `suffix`, ignoring ASCII case.
///
/// @param text a file name
/// @param suffix a suffix such as `fbi` or `sidedata.tdf`
/// @return true when the suffix matches
bool ends_with_folded(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size())
        return false;
    const size_t start = text.size() - suffix.size();
    for (size_t offset = 0; offset < suffix.size(); ++offset) {
        if (fold_ascii(text[start + offset]) != fold_ascii(suffix[offset]))
            return false;
    }
    return true;
}

/// Copies a folder with '\' turned into '/' and trailing slashes removed.
///
/// @param folder a folder argument
/// @return the folder used for matching
std::string normalize_folder(std::string_view folder) {
    std::string out;
    out.reserve(folder.size());
    for (const char byte : folder) {
        if (byte == '\\')
            out.push_back('/');
        else
            out.push_back(byte);
    }
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

/// Reports whether a stored path lies in a folder.
///
/// An empty folder is the root: without `recursive` only a path that contains
/// no slash matches. Otherwise the path must be `folder` plus one slash plus
/// a remainder, and without `recursive` that remainder contains no slash.
///
/// @param stored a document path, already using '/'
/// @param folder a normalized folder; empty for the root
/// @param recursive true to accept paths below the first level
/// @return true when the path is in the folder
bool path_in_folder(std::string_view stored, std::string_view folder, bool recursive) {
    if (folder.empty()) {
        if (recursive)
            return true;
        return stored.find('/') == std::string_view::npos;
    }
    std::string prefix;
    prefix.reserve(folder.size() + 1);
    prefix.append(folder.data(), folder.size());
    prefix.push_back('/');
    if (!starts_with_folded(stored, prefix))
        return false;
    if (recursive)
        return true;
    const std::string_view rest = stored.substr(prefix.size());
    return rest.find('/') == std::string_view::npos;
}

} // namespace

bool DocumentSet::add(std::string doc_path, std::string_view text, std::string* error) {
    for (char& byte : doc_path) {
        if (byte == '\\')
            byte = '/';
    }
    if (find(doc_path) != nullptr) {
        if (error != nullptr)
            *error = "a document is already loaded at this path";
        return false;
    }
    tdf::ParseError parse_error;
    tdf::OwnedDocument parsed;
    if (!parsed.parse(text, &parse_error)) {
        if (error != nullptr)
            *error = tdf::describe(parse_error);
        return false;
    }
    LoadedDocument loaded;
    loaded.path = std::move(doc_path);
    loaded.document = std::move(parsed);
    documents_.push_back(std::move(loaded));
    return true;
}

LoadedDocument* DocumentSet::find(std::string_view doc_path) {
    for (LoadedDocument& loaded : documents_) {
        if (same_path(loaded.path, doc_path))
            return &loaded;
    }
    return nullptr;
}

std::size_t DocumentSet::size() const {
    return documents_.size();
}

LoadedDocument& DocumentSet::at(std::size_t position) {
    return documents_[position];
}

const LoadedDocument& DocumentSet::at(std::size_t position) const {
    return documents_[position];
}

std::vector<std::size_t>
DocumentSet::documents_in(std::string_view folder, std::string_view suffix, bool recursive) const {
    const std::string folder_text = normalize_folder(folder);
    std::vector<std::size_t> found;
    for (size_t doc_index = 0; doc_index < documents_.size(); ++doc_index) {
        const LoadedDocument& loaded = documents_[doc_index];
        if (loaded.removed)
            continue;
        if (!path_in_folder(loaded.path, folder_text, recursive))
            continue;
        const auto slash = loaded.path.rfind('/');
        const std::string_view filename = slash == std::string::npos
                                              ? std::string_view(loaded.path)
                                              : std::string_view(loaded.path).substr(slash + 1);
        if (!ends_with_folded(filename, suffix))
            continue;
        found.push_back(doc_index);
    }
    return found;
}

bool DocumentSet::changed(std::size_t position) const {
    if (position >= documents_.size())
        return false;
    const LoadedDocument& loaded = documents_[position];
    return loaded.added || loaded.removed || tdf::document_edited(loaded.document.get());
}

} // namespace oa::data::mutation_rules
