// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The parsed documents one pipeline run edits. The files layer fills the set;
// rules and later passes share it and write back into these trees.
#pragma once

#include "oa/formats/tdf.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mutation_rules {

/// One parsed document, addressed the way the asset store lists it.
struct LoadedDocument {
    std::string path;                         ///< store path, '/' separated
    oa::formats::tdf::OwnedDocument document; ///< parsed tree; empty if parsing failed
    bool removed{};                           ///< true when a pass has taken the document out
    bool added{};                             ///< true when a pass added the document
};

/// The documents a pipeline run reads and writes, in the order they were added.
///
/// Paths are compared ignoring ASCII case and treating '\\' as '/'. A removed
/// document stays in the set so a later pass can see it, and is left out of
/// folder listings.
class DocumentSet {
  public:

    /// Parses a document and appends it.
    ///
    /// A second document of the same path, ignoring ASCII case and slash
    /// direction, is refused. A text that does not parse is refused and the
    /// set is left unchanged.
    ///
    /// @param path store path, '/' or '\\' separated
    /// @param text TDF text
    /// @param[out] error the parse or duplicate error; may be null
    /// @return true when the document was added
    bool add(std::string path, std::string_view text, std::string* error);

    /// Finds a document by path, ignoring ASCII case and slash direction.
    ///
    /// @param path store path
    /// @return the document, or null when none is loaded at that path
    [[nodiscard]] LoadedDocument* find(std::string_view path);

    /// Returns how many documents the set holds, removed ones included.
    ///
    /// @return the count
    [[nodiscard]] std::size_t size() const;

    /// Returns the document at an index.
    ///
    /// @param index position in the order added; must be less than size()
    /// @return the document
    [[nodiscard]] LoadedDocument& at(std::size_t index);

    /// Returns the document at an index.
    ///
    /// @param index position in the order added; must be less than size()
    /// @return the document
    [[nodiscard]] const LoadedDocument& at(std::size_t index) const;

    /// Lists documents whose path is in a folder and whose file name ends with a suffix.
    ///
    /// Matching ignores ASCII case. Removed documents are left out. The indices
    /// are in the order the documents were added. `folder` is not recursive
    /// unless `recursive` is set: `features/corpses` then reaches
    /// `features/corpses/a.tdf` and not `features/corpses/deep/a.tdf`.
    ///
    /// @param folder folder, '/' or '\\' separated; an empty folder is the root
    /// @param suffix file-name suffix, such as `fbi` or `sidedata.tdf`
    /// @param recursive true to include documents in folders below `folder`
    /// @return indices of the matching documents
    [[nodiscard]] std::vector<std::size_t>
    documents_in(std::string_view folder, std::string_view suffix, bool recursive) const;

    /// Reports whether a document was edited, added or removed.
    ///
    /// @param index position in the order added
    /// @return true when the document is marked added or removed, or an edit changed its tree;
    ///         false when the index is past the end
    [[nodiscard]] bool changed(std::size_t index) const;

  private:

    std::vector<LoadedDocument> documents_;
};

} // namespace oa::data::mutation_rules
