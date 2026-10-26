// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The one place a mutator's number meets the range the game reads it in.
#include "oa/data/mutators/hold_range.hpp"

#include "oa/data/field_schema.hpp"
#include "oa/data/mutators/files_layer.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/formats/tdf/edit.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace oa::data::mutators {
namespace tdf = oa::formats::tdf;
namespace fields = oa::data::field_schema;
namespace defs = oa::data::defs;

namespace {

int64_t trunc_toward_zero(double value) {
    if (!std::isfinite(value))
        return std::numeric_limits<int64_t>::min();
    if (value >= 9223372036854775808.0)
        return std::numeric_limits<int64_t>::min();
    if (value <= -9223372036854775808.0)
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(std::trunc(value));
}

constexpr double k_fixed_scale = 65536.0;
constexpr double k_tick_scale = 30.0;
constexpr double k_velocity_scale = 65536.0 / 30.0;
constexpr double k_acceleration_scale = 65536.0 / 900.0;
constexpr double k_weapon_turn_scale = 1.0 / 30.0;

struct Limit {
    bool scaled{};
    double scale = 1.0;
    fields::Storage width = fields::Storage::none;
    int64_t low{};
    int64_t high{};
    /// The bits a whole number keeps when the loader masks it instead of casting.
    uint32_t mask{};
};

char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold_ascii(left[index]) != fold_ascii(right[index]))
            return false;
    }
    return true;
}

std::string fold_copy(std::string_view text) {
    std::string out(text.size(), '\0');
    for (std::size_t index = 0; index < text.size(); ++index)
        out[index] = fold_ascii(text[index]);
    return out;
}

bool is_space(unsigned char byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\f' ||
           byte == '\v';
}

/// The whole number `parse_int` would keep, before its 32-bit wrap.
bool read_whole(const char* text, int64_t& out) {
    if (text == nullptr)
        return false;
    const auto* at = reinterpret_cast<const unsigned char*>(text);
    while (is_space(*at))
        ++at;
    int sign = 1;
    if (*at == '-' || *at == '+') {
        if (*at == '-')
            sign = -1;
        ++at;
    }
    if (*at < '0' || *at > '9')
        return false;
    bool overflow = false;
    int64_t total = 0;
    while (*at >= '0' && *at <= '9') {
        const int digit = static_cast<int>(*at - '0');
        if (total > (std::numeric_limits<int64_t>::max() - digit) / 10)
            overflow = true;
        else
            total = total * 10 + digit;
        ++at;
    }
    if (overflow)
        out = sign < 0 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    else if (sign < 0)
        out = -total;
    else
        out = total;
    return true;
}

int32_t loader_int(const char* text) {
    const auto* at = reinterpret_cast<const unsigned char*>(text);
    while (is_space(*at))
        ++at;
    const unsigned char sign = *at;
    if (sign == '-' || sign == '+')
        ++at;
    uint32_t total = 0;
    while (*at >= '0' && *at <= '9')
        total = total * 10u + static_cast<uint32_t>(*at++ - '0');
    if (sign == '-')
        total = 0u - total;
    return static_cast<int32_t>(total);
}

int64_t narrow_integer(fields::Storage width, int32_t parsed) {
    switch (width) {
    case fields::Storage::int8:
        return static_cast<int8_t>(parsed);
    case fields::Storage::uint8:
        return static_cast<uint8_t>(parsed);
    case fields::Storage::int16:
        return static_cast<int16_t>(parsed);
    case fields::Storage::uint16:
        return static_cast<uint16_t>(parsed);
    case fields::Storage::uint32:
        return static_cast<uint32_t>(parsed);
    case fields::Storage::int32:
        return parsed;
    default:
        return parsed;
    }
}

int64_t narrow_scaled(fields::Storage width, int64_t product) {
    switch (width) {
    case fields::Storage::int16:
    case fields::Storage::ticks16:
        return static_cast<int16_t>(product);
    case fields::Storage::uint16:
        return static_cast<uint16_t>(product);
    case fields::Storage::int32:
    case fields::Storage::ticks32:
    case fields::Storage::fixed32:
        return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(product)));
    default:
        return product;
    }
}

/// The whole number the loader keeps of `text` for a field.
int64_t stored_integer(const Limit& limit, const char* text) {
    const int32_t parsed = loader_int(text);
    if (limit.mask != 0)
        return static_cast<int64_t>(static_cast<uint32_t>(parsed) & limit.mask);
    return narrow_integer(limit.width, parsed);
}

int64_t clamp_to(int64_t value, int64_t low, int64_t high) {
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

bool bounds_of(fields::Storage width, int64_t& low, int64_t& high) {
    switch (width) {
    case fields::Storage::int8:
        low = std::numeric_limits<int8_t>::min();
        high = std::numeric_limits<int8_t>::max();
        return true;
    case fields::Storage::uint8:
        low = 0;
        high = std::numeric_limits<uint8_t>::max();
        return true;
    case fields::Storage::int16:
    case fields::Storage::ticks16:
        low = std::numeric_limits<int16_t>::min();
        high = std::numeric_limits<int16_t>::max();
        return true;
    case fields::Storage::uint16:
        low = 0;
        high = std::numeric_limits<uint16_t>::max();
        return true;
    case fields::Storage::int32:
    case fields::Storage::ticks32:
    case fields::Storage::fixed32:
        low = std::numeric_limits<int32_t>::min();
        high = std::numeric_limits<int32_t>::max();
        return true;
    case fields::Storage::uint32:
        low = 0;
        high = std::numeric_limits<uint32_t>::max();
        return true;
    default:
        return false;
    }
}

/// Flags of a section that change how the game reads some of its fields,
/// each the low bit the loader keeps, as the run left it.
struct SectionFlags {
    bool flies{};       ///< a unit's canfly: it moves and builds as an aircraft
    bool interceptor{}; ///< a weapon's interceptor
    bool stockpile{};   ///< a weapon's stockpile
};

/// The fastest 16.16 speed a unit that does not fly brakes from: braking
/// squares the speed and keeps 32 bits of the whole part, about 181.02.
constexpr int64_t k_ground_speed_high = 11863283;
/// The fastest 16.16 speed of an aircraft: a dogfight run is 30 ticks of it,
/// kept in 32 bits, about 1092.27.
constexpr int64_t k_air_speed_high = 71582788;
/// The 16.16 brake rates of a unit that does not fly: braking doubles the
/// rate and keeps 32 bits, so about -16384 to 16384.
constexpr int64_t k_ground_brake_low = -1073741824;
constexpr int64_t k_ground_brake_high = 1073741823;
/// The widest build distance of an aircraft, which arrives within it as a
/// signed 16-bit radius and hovers at it as a signed 16.16 distance.
constexpr int64_t k_air_build_distance_high = std::numeric_limits<int16_t>::max();
/// What an aircraft's strike run adds to attackrunlength before keeping the
/// sum as a signed 16.16 distance.
constexpr int64_t k_strike_run_extension = 960;
/// The longest attack run whose strike run keeps its sign.
constexpr int64_t k_attack_run_high = std::numeric_limits<int16_t>::max() - k_strike_run_extension;
/// The largest whole number a float keeps below 2^31: from 2147483584 a cost
/// rounds to 2^31, which reads back as no 32-bit whole number.
constexpr int64_t k_float_whole_high = 2147483583;
/// What a unit's spread adds to accuracy as its health falls to nothing.
constexpr int64_t k_damaged_spread = 2048;
/// The widest accuracy whose spread keeps its 16 bits on a damaged unit.
constexpr int64_t k_accuracy_high = std::numeric_limits<uint16_t>::max() - k_damaged_spread;
/// The widest area of effect of an interceptor, whose blast compares the
/// area squared as a signed 32-bit number.
constexpr int64_t k_interceptor_area_high = 46340;
/// The widest range or coverage: the game keeps it as a signed 16.16
/// distance, and an interceptor's coverage box doubles it in 32 bits.
constexpr int64_t k_world_distance_high = std::numeric_limits<int16_t>::max();
/// The longest reload, in ticks, of a weapon that does not stockpile: a
/// damaged unit reloads up to 120% as long, kept in 16 bits.
constexpr int64_t k_reload_ticks_high = 54613;

/// The range the game reads a unit, weapon or damage field in, when that
/// differs from what the field schema says the loader stores; nothing for
/// a field the game reads at the stored width.
///
/// Each range is what every reader of the field keeps as the number
/// written: the width and sign it reads, and what it does to the field
/// alone in 16 or 32 bits.
/// - Unit: `waterline`, `transportsize`, `transportcapacity` and
///   `makesmetal` are stored in a signed byte and read as unsigned bytes, 0
///   to 255. `turnrate`, `workertime`, `healtime`, `kamikazedistance` and
///   `maneuverleashlength` are stored in signed 16 bits and read unsigned,
///   0 to 65535; so is `builddistance` on the ground, while an aircraft
///   keeps it as a signed 16-bit radius, 0 to 32767. `attackrunlength` is
///   read unsigned and gains 960 in a signed 16.16 distance, 0 to 31807.
///   The costs `buildcostenergy`, `buildcostmetal`, `cloakcost` and
///   `cloakcostmoving` are kept in a float, which holds no whole number
///   from 2147483584 up. `maxvelocity` is squared by braking on the ground
///   and taken 30 times by an aircraft's dogfight run, in 32 bits; a ground
///   unit's `brakerate` is doubled in 32 bits.
/// - Weapon: `burst`, `sprayangle`, `turnrate` (divided by 30) and the
///   tick counts are stored in signed 16 bits and read unsigned, 0 to 65535;
///   `reloadtime` is at most 54613 ticks unless the weapon stockpiles,
///   because a damaged unit reloads up to 120% as long in 16 bits.
///   `areaofeffect` is read unsigned, and an interceptor squares it, at
///   most 46340. `accuracy` is read unsigned and a damaged unit's spread adds
///   up to 2048, so 0 to 63487. `rendertype` is an unsigned byte. `range` and
///   `coverage` become signed 16.16 distances, 0 to 32767. The speeds and
///   acceleration are compared and divided unsigned, so a negative one is
///   a wrap.
/// - `[DAMAGE]` `default` is read unsigned, then a hit reaches the unit as
///   signed 16 bits, so 0 to 32767.
///
/// @param spec the field
/// @param flags the section's flags
/// @return the limit, or nothing when the stored width is the read range
std::optional<Limit> read_limit(const fields::FieldSpec& spec, const SectionFlags& flags) {
    const auto whole = [](fields::Storage width, int64_t low, int64_t high) {
        Limit limit;
        limit.width = width;
        limit.low = low;
        limit.high = high;
        return limit;
    };
    const auto scaled = [](fields::Storage width, double scale, int64_t low, int64_t high) {
        Limit limit;
        limit.scaled = true;
        limit.scale = scale;
        limit.width = width;
        limit.low = low;
        limit.high = high;
        return limit;
    };
    constexpr int64_t byte_high = std::numeric_limits<uint8_t>::max();
    constexpr int64_t word_high = std::numeric_limits<uint16_t>::max();
    constexpr int64_t fixed_high = std::numeric_limits<int32_t>::max();
    const std::string_view key = spec.key;
    if (spec.record == fields::RecordKind::unit) {
        if (same_text(key, "waterline") || same_text(key, "transportsize") ||
            same_text(key, "transportcapacity") || same_text(key, "makesmetal"))
            return whole(fields::Storage::uint8, 0, byte_high);
        if (same_text(key, "turnrate") || same_text(key, "workertime") ||
            same_text(key, "healtime") || same_text(key, "kamikazedistance") ||
            same_text(key, "maneuverleashlength"))
            return whole(fields::Storage::uint16, 0, word_high);
        if (same_text(key, "builddistance"))
            return whole(
                fields::Storage::uint16, 0, flags.flies ? k_air_build_distance_high : word_high
            );
        if (same_text(key, "attackrunlength"))
            return whole(fields::Storage::uint16, 0, k_attack_run_high);
        if (same_text(key, "buildcostenergy") || same_text(key, "buildcostmetal") ||
            same_text(key, "cloakcost") || same_text(key, "cloakcostmoving"))
            return whole(
                fields::Storage::int32, std::numeric_limits<int32_t>::min(), k_float_whole_high
            );
        if (same_text(key, "maxvelocity")) {
            const int64_t high = flags.flies ? k_air_speed_high : k_ground_speed_high;
            return scaled(fields::Storage::fixed32, k_fixed_scale, -high, high);
        }
        if (same_text(key, "brakerate") && !flags.flies)
            return scaled(
                fields::Storage::fixed32, k_fixed_scale, k_ground_brake_low, k_ground_brake_high
            );
        return std::nullopt;
    }
    if (spec.record == fields::RecordKind::weapon) {
        if (same_text(key, "accuracy"))
            return whole(fields::Storage::uint16, 0, k_accuracy_high);
        if (same_text(key, "areaofeffect"))
            return whole(
                fields::Storage::uint16, 0, flags.interceptor ? k_interceptor_area_high : word_high
            );
        if (same_text(key, "burst") || same_text(key, "sprayangle"))
            return whole(fields::Storage::uint16, 0, word_high);
        if (same_text(key, "rendertype"))
            return whole(fields::Storage::uint8, 0, byte_high);
        if (same_text(key, "range") || same_text(key, "coverage"))
            return whole(fields::Storage::int32, 0, k_world_distance_high);
        if (same_text(key, "turnrate"))
            return scaled(fields::Storage::uint16, k_weapon_turn_scale, 0, word_high);
        if (same_text(key, "weaponvelocity") || same_text(key, "startvelocity"))
            return scaled(fields::Storage::fixed32, k_velocity_scale, 0, fixed_high);
        if (same_text(key, "weaponacceleration"))
            return scaled(fields::Storage::fixed32, k_acceleration_scale, 0, fixed_high);
        if (same_text(key, "reloadtime"))
            return scaled(
                fields::Storage::uint16,
                k_tick_scale,
                0,
                flags.stockpile ? word_high : k_reload_ticks_high
            );
        if (same_text(key, "weapontimer") || same_text(key, "burstrate") ||
            same_text(key, "duration") || same_text(key, "randomdecay") ||
            same_text(key, "smokedelay") || same_text(key, "flighttime") ||
            same_text(key, "holdtime"))
            return scaled(fields::Storage::uint16, k_tick_scale, 0, word_high);
        return std::nullopt;
    }
    if (spec.record == fields::RecordKind::weapon_damage && same_text(key, "default"))
        return whole(fields::Storage::int16, 0, std::numeric_limits<int16_t>::max());
    return std::nullopt;
}

/// The range a field is held in, from what the game reads or else the schema's width.
Limit limit_of(const fields::FieldSpec& spec, const SectionFlags& flags) {
    Limit limit;
    if (spec.type != fields::FieldType::integer && spec.type != fields::FieldType::decimal &&
        spec.type != fields::FieldType::fixed)
        return limit;
    // Three whole numbers reach the game narrower than their record stores
    // them. A unit's health is the low 16 bits of maxdamage, signed
    // (unit-spawn spawn.cpp), and below 1 it is dead; the field schema warns
    // outside 1 to 32767 for that reason.
    if (spec.record == fields::RecordKind::unit && same_text(spec.key, "maxdamage")) {
        limit.width = fields::Storage::int16;
        limit.low = 1;
        limit.high = std::numeric_limits<int16_t>::max();
        return limit;
    }
    // A [DAMAGE] entry for a unit name is kept in 32 bits, but a hit's
    // amount is 16 (unit_health make_health_event), as `default` is: 40000
    // takes a unit with 3000 health to 28536.
    if (spec.record == fields::RecordKind::weapon_damage && spec.key.empty()) {
        limit.width = fields::Storage::int16;
        limit.low = std::numeric_limits<int16_t>::min();
        limit.high = std::numeric_limits<int16_t>::max();
        return limit;
    }
    // The countdown is 3 bits of the abilities word (unit_def_loader.cpp),
    // so 10 seconds would count 2.
    if (spec.record == fields::RecordKind::unit && same_text(spec.key, "selfdestructcountdown")) {
        limit.width = fields::Storage::uint32;
        limit.mask = 7u;
        limit.low = 0;
        limit.high = 7;
        return limit;
    }
    if (const std::optional<Limit> read = read_limit(spec, flags))
        return *read;
    if (!bounds_of(spec.storage, limit.low, limit.high))
        return limit;
    limit.width = spec.storage;
    if (spec.storage == fields::Storage::ticks16 || spec.storage == fields::Storage::ticks32) {
        limit.scaled = true;
        limit.scale = k_tick_scale;
        return limit;
    }
    if (spec.storage == fields::Storage::fixed32) {
        limit.scaled = true;
        limit.scale = k_fixed_scale;
        return limit;
    }
    if (spec.type == fields::FieldType::integer)
        limit.scaled = false;
    else
        return Limit{};
    return limit;
}

/// The most ticks of spark time whose spread countdown fits its byte: the
/// countdown is half the spark time plus a draw below that half.
constexpr int64_t k_spark_ticks_high = 257;
/// The widest footprint whose cells keep their column and row back to the
/// origin, one byte each.
constexpr int64_t k_footprint_high = 256;

/// The range the game reads a feature field in, when that is narrower than
/// the text a hook can write; nothing for a field it reads whole or not at all.
///
/// The feature loader (map-runtime feature_defs.cpp) keeps metal and energy
/// as their low 16 bits, damage, the footprints and spark time as signed 16
/// bits, and height, spread chance, reproduce and reproduce area as signed
/// 8 bits. The game reads several of them narrower, or unsigned:
/// - Damage is the feature's hit points, unsigned 16 bits (feature-runtime
///   damage_feature): 0 to 65535.
/// - Height, spread chance, reproduce and reproduce area are read as
///   unsigned bytes: 0 to 255. Stock spires stand up to 250 high.
/// - Spark time is seconds times 30, read as unsigned 16 bits, and a fire's
///   spread countdown is half of it plus a draw below that half, kept in a
///   byte: past 257 ticks (8.57 seconds) the countdown wraps short, or to 0
///   and the fire never spreads.
/// - A footprint cell keeps its column and row back to the origin in a byte
///   each, so a footprint past 256 cells loses its origin. A negative
///   footprint covers no cells, as in 3.1c, and is held only past -32768.
/// - An indestructible feature on the map paints the low byte of its metal
///   on the plots it covers (paint_feature_metal). When it cannot be
///   reclaimed that byte is the only use of its metal, so it is 0 to 255;
///   a reclaimable one keeps 0 to 65535 for its reclaim.
/// The loader does not read reclaim time, hit density or burn times. A flag
/// keeps one bit, and neither end is nearer to a value past it, so it is
/// left as written.
///
/// @param key feature field
/// @param metal_spot whether the feature is indestructible and not reclaimable, by the low bits
/// @return the limit, or nothing when the field is not held
std::optional<Limit> feature_limit(std::string_view key, bool metal_spot) {
    const auto whole = [](fields::Storage width, int64_t low, int64_t high, uint32_t mask) {
        Limit limit;
        limit.width = width;
        limit.low = low;
        limit.high = high;
        limit.mask = mask;
        return limit;
    };
    if (same_text(key, "metal") && metal_spot)
        return whole(fields::Storage::uint8, 0, std::numeric_limits<uint8_t>::max(), 0xffu);
    if (same_text(key, "metal") || same_text(key, "energy"))
        return whole(fields::Storage::uint16, 0, std::numeric_limits<uint16_t>::max(), 0xffffu);
    if (same_text(key, "damage"))
        return whole(fields::Storage::uint16, 0, std::numeric_limits<uint16_t>::max(), 0);
    if (same_text(key, "footprintx") || same_text(key, "footprintz"))
        return whole(
            fields::Storage::int16, std::numeric_limits<int16_t>::min(), k_footprint_high, 0
        );
    if (same_text(key, "height") || same_text(key, "spreadchance") || same_text(key, "reproduce") ||
        same_text(key, "reproducearea"))
        return whole(fields::Storage::uint8, 0, std::numeric_limits<uint8_t>::max(), 0);
    if (same_text(key, "sparktime")) {
        Limit limit;
        limit.scaled = true;
        limit.scale = k_tick_scale;
        limit.width = fields::Storage::ticks16;
        limit.low = 0;
        limit.high = k_spark_ticks_high;
        return limit;
    }
    return std::nullopt;
}

std::string spell_integer(int64_t value) {
    char buffer[32];
    const auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, written.ptr);
}

double game_double(const char* text) {
    const auto* at = reinterpret_cast<const unsigned char*>(text);
    while (is_space(*at))
        ++at;
    char buffer[512];
    std::size_t used = 0;
    const auto put = [&](char byte) {
        if (used + 1 < sizeof buffer)
            buffer[used++] = byte;
    };
    if (*at == '-' || *at == '+')
        put(static_cast<char>(*at++));
    bool digits = false;
    while (*at >= '0' && *at <= '9') {
        put(static_cast<char>(*at++));
        digits = true;
    }
    if (*at == '.') {
        put('.');
        ++at;
        while (*at >= '0' && *at <= '9') {
            put(static_cast<char>(*at++));
            digits = true;
        }
    }
    if (!digits)
        return 0.0;
    if (*at == 'e' || *at == 'E' || *at == 'd' || *at == 'D') {
        const unsigned char* exponent = at + 1;
        if (*exponent == '-' || *exponent == '+')
            ++exponent;
        if (*exponent >= '0' && *exponent <= '9') {
            put('e');
            ++at;
            if (*at == '-' || *at == '+')
                put(static_cast<char>(*at++));
            while (*at >= '0' && *at <= '9')
                put(static_cast<char>(*at++));
        }
    }
    buffer[used] = '\0';
    return std::strtod(buffer, nullptr);
}

/// The shortest decimal the loader scales and truncates back to `stored`.
/// At each length the nearest spelling comes first, then the one a last
/// digit further from zero: 32767 ticks is 1092.24 seconds, not
/// 1092.2333333333333.
std::string spell_scaled(int64_t stored, double scale) {
    const double raw = static_cast<double>(stored) / scale;
    char buffer[64];
    for (int digits = 0; digits <= 15; ++digits) {
        const double step = std::pow(10.0, -digits);
        const double nudged = raw < 0.0 ? raw - step : raw + step;
        for (const double candidate : {raw, nudged}) {
            std::snprintf(buffer, sizeof buffer, "%.*f", digits, candidate);
            const double parsed = std::strtod(buffer, nullptr);
            if (trunc_toward_zero(parsed * scale) == stored)
                return buffer;
        }
    }
    std::snprintf(buffer, sizeof buffer, "%.17g", raw);
    return buffer;
}

int64_t stored_of(const Limit& limit, const char* text) {
    if (limit.scaled) {
        const double number = game_double(text);
        const double scaled = number * limit.scale;
        if (!std::isfinite(number) || !std::isfinite(scaled))
            return limit.low;
        if (scaled >= 9223372036854775808.0)
            return limit.high;
        if (scaled <= -9223372036854775808.0)
            return limit.low;
        return narrow_scaled(limit.width, trunc_toward_zero(scaled));
    }
    return stored_integer(limit, text);
}

/// Text the loader reads as `ideal`, or the nearest end toward zero that it can.
std::string text_at_end(const Limit& limit, int64_t ideal) {
    for (int step = 0; step < 64; ++step) {
        int64_t candidate = ideal;
        if (ideal >= 0)
            candidate = ideal - static_cast<int64_t>(step);
        else
            candidate = ideal + static_cast<int64_t>(step);
        if (candidate < limit.low || candidate > limit.high)
            break;
        const std::string spelled =
            limit.scaled ? spell_scaled(candidate, limit.scale) : spell_integer(candidate);
        if (stored_of(limit, spelled.c_str()) == candidate)
            return spelled;
    }
    return {};
}

bool needs_hold(const Limit& limit, const char* text, int64_t& ideal) {
    if (limit.scaled) {
        const double number = game_double(text);
        if (!std::isfinite(number))
            return false;
        const double scaled = number * limit.scale;
        int64_t product = 0;
        if (!std::isfinite(scaled) || scaled >= 9223372036854775808.0)
            product = scaled < 0.0 ? limit.low : limit.high;
        else if (scaled <= -9223372036854775808.0)
            product = limit.low;
        else
            product = trunc_toward_zero(scaled);
        ideal = clamp_to(product, limit.low, limit.high);
        return narrow_scaled(limit.width, product) != ideal;
    }
    int64_t whole = 0;
    if (!read_whole(text, whole))
        return false;
    ideal = clamp_to(whole, limit.low, limit.high);
    return stored_integer(limit, text) != ideal;
}

/// Whether the base text holds the same number the loader would read.
bool same_number(const Limit& limit, const char* base, const char* value) {
    if (base == nullptr)
        return false;
    if (std::strcmp(base, value) == 0)
        return true;
    if (limit.scaled) {
        const double left = game_double(base);
        const double right = game_double(value);
        return std::isfinite(left) && left == right;
    }
    int64_t left = 0;
    int64_t right = 0;
    if (!read_whole(base, left) || !read_whole(value, right))
        return false;
    return left == right;
}

std::string file_stem(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = file.find_last_of('.');
    if (dot != std::string_view::npos)
        file = file.substr(0, dot);
    return std::string{file};
}

std::string record_name(const tdf::Block* block, std::string_view path, bool unit) {
    if (unit) {
        const char* written = tdf::find_value(block, "UnitName");
        if (written != nullptr && written[0] != '\0')
            return written;
        return file_stem(path);
    }
    if (block != nullptr && block->name != nullptr && block->name[0] != '\0')
        return block->name;
    return file_stem(path);
}

bool moveinfo_path(std::string_view path, const defs::DataLayout& layout) {
    const std::string folder =
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::gamedata)];
    std::string folded = fold_copy(path);
    for (char& character : folded) {
        if (character == '\\')
            character = '/';
    }
    const std::string suffix = "/" + fold_copy(folder) + "/moveinfo.tdf";
    return folded.size() >= suffix.size() &&
           folded.compare(folded.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool feature_tdf(std::string_view path) {
    std::string folded = fold_copy(path);
    for (char& character : folded) {
        if (character == '\\')
            character = '/';
    }
    constexpr std::string_view prefix = "features/";
    constexpr std::string_view suffix = ".tdf";
    if (folded.size() < prefix.size() + suffix.size())
        return false;
    if (folded.compare(0, prefix.size(), prefix) != 0)
        return false;
    return folded.compare(folded.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Whether a flag is on: the loader keeps its low bit, read as the run left it.
bool flag_on(const tdf::Block* block, const char* name) {
    const char* text = tdf::find_value(block, name);
    return text != nullptr && (loader_int(text) & 1) != 0;
}

void hold_feature_block(
    std::vector<HeldField>& held,
    tdf::Document* document,
    tdf::Block* block,
    const tdf::Block* base,
    const std::string& path,
    const std::string& record
) {
    if (block == nullptr)
        return;
    const bool metal_spot = flag_on(block, "indestructible") && !flag_on(block, "reclaimable");
    const int32_t count = static_cast<int32_t>(tdf::property_count(block));
    for (int32_t index = 0; index < count; ++index) {
        const char* key = tdf::property_key_at(block, index);
        const char* value = tdf::property_value_at(block, index);
        if (key == nullptr || value == nullptr || key[0] == '\0')
            continue;
        const std::optional<Limit> found = feature_limit(key, metal_spot);
        if (!found.has_value())
            continue;
        const Limit& limit = *found;
        int64_t ideal = 0;
        if (!needs_hold(limit, value, ideal))
            continue;
        if (same_number(limit, tdf::find_value(base, key), value))
            continue;
        const std::string replacement = text_at_end(limit, ideal);
        if (replacement.empty() || replacement == value)
            continue;
        const std::string previous = value;
        const std::string field_name = fold_copy(key);
        if (tdf::set_value(document, block, key, replacement) != tdf::EditStatus::ok)
            continue;
        HeldField field;
        field.path = path;
        field.record = record;
        field.field = field_name;
        field.from_text = previous;
        field.to_text = replacement;
        held.push_back(std::move(field));
    }
}

void hold_block(
    std::vector<HeldField>& held,
    tdf::Document* document,
    tdf::Block* block,
    const tdf::Block* base,
    fields::RecordKind kind,
    const SectionFlags& flags,
    const std::string& path,
    const std::string& record,
    const std::string& block_name
) {
    if (block == nullptr)
        return;
    const int32_t count = static_cast<int32_t>(tdf::property_count(block));
    for (int32_t index = 0; index < count; ++index) {
        const char* key = tdf::property_key_at(block, index);
        const char* value = tdf::property_value_at(block, index);
        if (key == nullptr || value == nullptr || key[0] == '\0')
            continue;
        const fields::FieldSpec* spec = fields::find_field(kind, key);
        if (spec == nullptr)
            continue;
        const Limit limit = limit_of(*spec, flags);
        if (limit.width == fields::Storage::none && !limit.scaled)
            continue;
        int64_t ideal = 0;
        if (!needs_hold(limit, value, ideal))
            continue;
        // A value the run left as the base has it is the game's own.
        if (same_number(limit, tdf::find_value(base, key), value))
            continue;
        const std::string replacement = text_at_end(limit, ideal);
        if (replacement.empty() || replacement == value)
            continue;
        const std::string previous = value;
        const std::string field_name = fold_copy(key);
        if (tdf::set_value(document, block, key, replacement) != tdf::EditStatus::ok)
            continue;
        HeldField field;
        field.path = path;
        field.record = record;
        field.block = block_name;
        field.field = field_name;
        field.from_text = previous;
        field.to_text = replacement;
        held.push_back(std::move(field));
    }
}

/// The base section of the same name, or null when the run added it.
const tdf::Block* base_child(const tdf::Block* base, const tdf::Block* section) {
    if (base == nullptr || section == nullptr || section->name == nullptr)
        return nullptr;
    return tdf::find_child(base, section->name);
}

void hold_unit(
    std::vector<HeldField>& held,
    tdf::Document* document,
    const tdf::Block* base,
    const std::string& path
) {
    tdf::Block* root = tdf::editable_root(document);
    const uint32_t count = tdf::child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        tdf::Block* section = tdf::editable_child(root, index);
        if (section == nullptr || section->name == nullptr || !same_text(section->name, "UNITINFO"))
            continue;
        SectionFlags flags;
        flags.flies = flag_on(section, "canfly");
        hold_block(
            held,
            document,
            section,
            base_child(base, section),
            fields::RecordKind::unit,
            flags,
            path,
            record_name(section, path, true),
            std::string{}
        );
    }
}

void hold_weapon(
    std::vector<HeldField>& held,
    tdf::Document* document,
    const tdf::Block* base,
    const std::string& path
) {
    tdf::Block* root = tdf::editable_root(document);
    const uint32_t count = tdf::child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        tdf::Block* section = tdf::editable_child(root, index);
        if (section == nullptr || section->name == nullptr)
            continue;
        const std::string name = record_name(section, path, false);
        const tdf::Block* base_section = base_child(base, section);
        SectionFlags flags;
        flags.interceptor = flag_on(section, "interceptor");
        flags.stockpile = flag_on(section, "stockpile");
        hold_block(
            held,
            document,
            section,
            base_section,
            fields::RecordKind::weapon,
            flags,
            path,
            name,
            std::string{}
        );
        const uint32_t children = tdf::child_count(section);
        for (uint32_t child = 0; child < children; ++child) {
            tdf::Block* damage = tdf::editable_child(section, child);
            if (damage == nullptr || damage->name == nullptr || !same_text(damage->name, "DAMAGE"))
                continue;
            hold_block(
                held,
                document,
                damage,
                base_child(base_section, damage),
                fields::RecordKind::weapon_damage,
                flags,
                path,
                name,
                damage->name
            );
        }
    }
}

void hold_features(
    std::vector<HeldField>& held,
    tdf::Document* document,
    const tdf::Block* base,
    const std::string& path
) {
    tdf::Block* root = tdf::editable_root(document);
    const uint32_t count = tdf::child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        tdf::Block* section = tdf::editable_child(root, index);
        if (section == nullptr)
            continue;
        hold_feature_block(
            held,
            document,
            section,
            base_child(base, section),
            path,
            record_name(section, path, false)
        );
    }
}

void hold_moveinfo(
    std::vector<HeldField>& held,
    tdf::Document* document,
    const tdf::Block* base,
    const std::string& path
) {
    tdf::Block* root = tdf::editable_root(document);
    const uint32_t count = tdf::child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        tdf::Block* section = tdf::editable_child(root, index);
        if (section == nullptr)
            continue;
        hold_block(
            held,
            document,
            section,
            base_child(base, section),
            fields::RecordKind::move_class,
            SectionFlags{},
            path,
            record_name(section, path, false),
            std::string{}
        );
    }
}

} // namespace

std::optional<int64_t> written_whole(const char* text) {
    int64_t whole = 0;
    if (!read_whole(text, whole))
        return std::nullopt;
    return whole;
}

std::vector<HeldField> hold_narrow_fields(
    oa::data::mutation_rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const BaseText& base
) {
    std::vector<HeldField> held;
    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        if (!documents.changed(slot))
            continue;
        oa::data::mutation_rules::LoadedDocument& loaded = documents.at(slot);
        if (loaded.removed || loaded.document.get() == nullptr)
            continue;
        const RecordFileKind kind = record_file_kind(loaded.path, layout);
        const bool moveinfo = kind != RecordFileKind::unit && kind != RecordFileKind::weapon &&
                              moveinfo_path(loaded.path, layout);
        const bool feature = feature_tdf(loaded.path);
        if (kind != RecordFileKind::unit && kind != RecordFileKind::weapon && !moveinfo && !feature)
            continue;
        // The text without this run: a value it still holds is not held.
        tdf::OwnedDocument original;
        const tdf::Block* base_root = nullptr;
        if (base) {
            const std::optional<std::string> text = base(loaded.path);
            if (text.has_value() && original.parse(*text))
                base_root = original.root();
        }
        if (kind == RecordFileKind::unit)
            hold_unit(held, loaded.document.get(), base_root, loaded.path);
        else if (kind == RecordFileKind::weapon)
            hold_weapon(held, loaded.document.get(), base_root, loaded.path);
        else if (feature)
            hold_features(held, loaded.document.get(), base_root, loaded.path);
        else
            hold_moveinfo(held, loaded.document.get(), base_root, loaded.path);
    }
    return held;
}

} // namespace oa::data::mutators
