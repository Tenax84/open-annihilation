// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Mutators account: a loadout text written and read back through a bank
// image, a save without the account reading as none, and a save written by
// the profile steps staying byte for byte the same when the account is not
// asked for.

#include "check.hpp"

#include "oa/data/persist/save_mutators.hpp"
#include "oa/data/persist/save_profile.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using namespace oa::data::persist;

namespace {

/// Serializes a bank and reads the image into a fresh bank.
///
/// @param bank the bank written
/// @param[out] read the bank read back; reset first
/// @return true when the image wrote and read
bool round_trip(const Bank& bank, Bank& read) {
    ByteImage image{};
    const bool written = bank_write_image(&bank, "Total Annihilation Saved Game", true, &image);
    bool ok = written && bank_read_image(&read, image.data, image.size, nullptr, nullptr, nullptr);
    byte_image_free(&image);
    return ok;
}

/// The steps persist-profile uses to write a save under a profile.
///
/// @param[out] bank the bank; initialised here
void profile_steps(Bank& bank) {
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    bank_set_int(&bank, "Game Time", 30);
    const SavedProfile profile{"a-mod", "1.2", 1, std::string(64, 'a'), std::string(64, 'b')};
    const std::array<uint8_t, 3> reuse{1, 2, 3};
    const std::array<SavedRuleState, 2> tables{{{"reuse", reuse}, {"empty", {}}}};
    CHECK(save_write_profile(&bank, profile, tables));
}

void loadout_round_trips() {
    const std::string text(4096, 'q');
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    bank_set_int(&bank, "Game Time", 30);
    CHECK(save_write_mutators(&bank, text));
    CHECK(!save_write_mutators(&bank, {}));
    CHECK(!save_write_mutators(&bank, std::string(save_mutators_most_bytes + 1, 'q')));
    Bank read;
    bank_init(&read);
    CHECK(round_trip(bank, read));
    const auto loaded = save_read_mutators(&read);
    CHECK(loaded.has_value() && *loaded == text);
    bank_destroy(&read);
    bank_destroy(&bank);
}

void save_without_mutators_reads_none() {
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    bank_set_int(&bank, "Game Time", 30);
    Bank read;
    bank_init(&read);
    CHECK(round_trip(bank, read));
    CHECK(!save_read_mutators(&read).has_value());
    bank_destroy(&read);
    bank_destroy(&bank);
}

void over_long_blob_reads_none() {
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, mutators_key::account.data());
    CHECK(bank_find_blob_name(&bank, mutators_key::loadout.data(), true) >= 0);
    bank_open_blob_name(&bank, mutators_key::loadout.data());
    const std::string huge(save_mutators_most_bytes + 1, 'q');
    const auto bytes = static_cast<uint32_t>(huge.size());
    CHECK(bank_blob_write(&bank, huge.data(), bytes) == bytes);
    CHECK(!save_read_mutators(&bank).has_value());
    bank_destroy(&bank);
}

void bytes_match_profile_steps_without_the_account() {
    Bank profile_bank;
    profile_steps(profile_bank);
    Bank same;
    profile_steps(same);
    ByteImage left{};
    ByteImage right{};
    CHECK(bank_write_image(&profile_bank, "Total Annihilation Saved Game", true, &left));
    CHECK(bank_write_image(&same, "Total Annihilation Saved Game", true, &right));
    CHECK(
        left.size > 0 && left.size == right.size && left.data != nullptr && right.data != nullptr &&
        std::memcmp(left.data, right.data, left.size) == 0
    );
    CHECK(save_write_mutators(&same, "ridge-pace 1"));
    ByteImage with_loadout{};
    CHECK(bank_write_image(&same, "Total Annihilation Saved Game", true, &with_loadout));
    const bool same_bytes = with_loadout.size == left.size && with_loadout.data != nullptr &&
                            left.data != nullptr &&
                            std::memcmp(with_loadout.data, left.data, left.size) == 0;
    CHECK(!same_bytes);
    byte_image_free(&left);
    byte_image_free(&right);
    byte_image_free(&with_loadout);
    bank_destroy(&profile_bank);
    bank_destroy(&same);
}

} // namespace

int main() {
    loadout_round_trips();
    save_without_mutators_reads_none();
    over_long_blob_reads_none();
    bytes_match_profile_steps_without_the_account();
    return test::finish("persist-mutators");
}
