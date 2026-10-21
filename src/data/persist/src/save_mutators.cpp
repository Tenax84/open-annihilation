// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/persist/save_mutators.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>

namespace oa::data::persist {

bool save_write_mutators(Bank* bank, std::string_view loadout_text) {
    if (bank == nullptr || loadout_text.empty() || loadout_text.size() > save_mutators_most_bytes)
        return false;
    // The names are whole string literals, so data() is NUL-terminated.
    std::ignore = bank_open_account(bank, mutators_key::account.data());
    if (bank_find_blob_name(bank, mutators_key::loadout.data(), true) < 0)
        return false;
    bank_open_blob_name(bank, mutators_key::loadout.data());
    bank_blob_seek(bank, 0);
    const auto bytes = static_cast<uint32_t>(loadout_text.size());
    return bank_blob_write(bank, loadout_text.data(), bytes) == bytes;
}

std::optional<std::string> save_read_mutators(Bank* bank) {
    if (bank == nullptr)
        return std::nullopt;
    // Opening an absent account creates an empty one, which reads as none.
    // An empty account is left out of a save's bytes.
    std::ignore = bank_open_account(bank, mutators_key::account.data());
    if (bank_find_blob_name(bank, mutators_key::loadout.data(), false) < 0)
        return std::nullopt;
    if (!bank_open_blob_name(bank, mutators_key::loadout.data()))
        return std::nullopt;
    const int32_t blob_bytes = bank_blob_size(bank);
    if (blob_bytes <= 0 || static_cast<std::size_t>(blob_bytes) > save_mutators_most_bytes)
        return std::nullopt;
    std::string text(static_cast<std::size_t>(blob_bytes), '\0');
    bank_blob_seek(bank, 0);
    const auto wanted = static_cast<uint32_t>(blob_bytes);
    if (bank_blob_read(bank, text.data(), wanted) != wanted)
        return std::nullopt;
    return text;
}

} // namespace oa::data::persist
