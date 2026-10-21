// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which mutators are on, in which order, and with which option values.
// Prerequisites come from the Library's closure, and their version ranges
// are checked with the manifest's condition matcher. A number is read with
// the manifest's number grammar and kept in its canonical text, never as a
// floating-point value. Nothing here reads a file, a clock, a preference or
// the network.
#include "oa/ui/mutator_picker/model.hpp"

#include "internal.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::ui::mutator_picker {
namespace {

namespace oamod = formats::oamod;
namespace oamut = formats::oamut;

/// The largest significand a sum or a product may reach.
constexpr uint64_t most_significand = std::numeric_limits<uint64_t>::max();

/// Reads a number in the manifest's grammar: no plus sign, no leading zeros.
///
/// @param text the number's text
/// @return the number, or nothing when the text is not one
std::optional<oamod::Number> read_number(std::string_view text) {
    oamod::Number number{};
    if (oamod::parse_number(text, number) != oamod::NumberStatus::ok)
        return std::nullopt;
    return number;
}

/// Makes a number, moving the significand's trailing zeros into the exponent.
///
/// @param negative below zero
/// @param significand the digits' value
/// @param exponent the power of ten the significand is scaled by
/// @return the number; zero is never negative
oamod::Number trimmed(bool negative, uint64_t significand, int64_t exponent) {
    oamod::Number number{};
    if (significand == 0)
        return number;
    while (significand % 10 == 0) {
        significand /= 10;
        ++exponent;
    }
    number.negative = negative;
    number.significand = significand;
    number.exponent = static_cast<int32_t>(exponent);
    return number;
}

/// Returns a number with its sign turned over.
///
/// @param number the number
/// @return its negation; zero stays zero
oamod::Number negated(oamod::Number number) {
    if (number.significand != 0)
        number.negative = !number.negative;
    return number;
}

/// Multiplies a number by a count, exactly.
///
/// @param number the number
/// @param count the count
/// @return the product, or nothing when its significand would not fit
std::optional<oamod::Number> times(const oamod::Number& number, uint64_t count) {
    if (count == 0 || number.significand == 0)
        return oamod::Number{};
    if (number.significand > most_significand / count)
        return std::nullopt;
    return trimmed(number.negative, number.significand * count, number.exponent);
}

/// Adds two numbers, exactly.
///
/// The sum is worked out at the lower exponent. When its significand would
/// not fit, the sum has more significant digits than the number grammar
/// holds (its last digit is the lower number's, which is not zero), so no
/// number the grammar reads can equal it.
///
/// @param left one number
/// @param right the other
/// @return the sum, or nothing when its significand would not fit
std::optional<oamod::Number> sum(const oamod::Number& left, const oamod::Number& right) {
    if (left.significand == 0)
        return trimmed(right.negative, right.significand, right.exponent);
    if (right.significand == 0)
        return trimmed(left.negative, left.significand, left.exponent);
    const bool left_high = left.exponent >= right.exponent;
    const oamod::Number& high = left_high ? left : right;
    const oamod::Number& low = left_high ? right : left;
    uint64_t scaled = high.significand;
    for (int64_t shift = int64_t{high.exponent} - int64_t{low.exponent}; shift > 0; --shift) {
        if (scaled > most_significand / 10)
            return std::nullopt;
        scaled *= 10;
    }
    if (high.negative == low.negative) {
        if (scaled > most_significand - low.significand)
            return std::nullopt;
        return trimmed(high.negative, scaled + low.significand, low.exponent);
    }
    if (scaled >= low.significand)
        return trimmed(high.negative, scaled - low.significand, low.exponent);
    return trimmed(low.negative, low.significand - scaled, low.exponent);
}

/// The numbers a number option offers, lowest first.
///
/// Each is the minimum plus a whole number of steps, up to the maximum, and
/// is kept only when the number grammar writes it and reads it back as the
/// same number. None when the option is not a number, when its minimum,
/// maximum or step does not read, when the step is not above zero, when the
/// minimum is above the maximum, or when the range holds more than
/// `most_number_steps` steps.
///
/// @param spec the option
/// @return the numbers
std::vector<oamod::Number> stops_of(const OptionSpec& spec) {
    std::vector<oamod::Number> stops;
    if (spec.type != OptionSpec::Type::number)
        return stops;
    const std::optional<oamod::Number> least = read_number(spec.least);
    const std::optional<oamod::Number> most = read_number(spec.most);
    const std::optional<oamod::Number> step = read_number(spec.step);
    if (!least || !most || !step)
        return stops;
    if (oamod::compare_numbers(*step, oamod::Number{}) <= 0 ||
        oamod::compare_numbers(*least, *most) > 0)
        return stops;
    const std::optional<oamod::Number> span = sum(*most, negated(*least));
    if (!span)
        return stops;
    std::vector<oamod::Number> offsets;
    for (std::size_t count = 0;; ++count) {
        const std::optional<oamod::Number> offset = times(*step, count);
        if (!offset || oamod::compare_numbers(*offset, *span) > 0)
            break;
        if (count > oamut::most_number_steps)
            return stops;
        offsets.push_back(*offset);
    }
    stops.reserve(offsets.size());
    for (const oamod::Number& offset : offsets) {
        const std::optional<oamod::Number> value = sum(*least, offset);
        if (!value)
            continue;
        oamod::Number written{};
        if (oamod::parse_number(oamod::canonical_number_text(*value), written) !=
                oamod::NumberStatus::ok ||
            oamod::compare_numbers(written, *value) != 0)
            continue;
        stops.push_back(written);
    }
    return stops;
}

/// The option with `id`, or null.
///
/// @param mutator the mutator
/// @param id the option's id
/// @return the option
const OptionSpec* find_option(const Mutator& mutator, std::string_view id) {
    for (const OptionSpec& spec : mutator.options)
        if (spec.id == id)
            return &spec;
    return nullptr;
}

/// The text an option keeps for `value`, when the option allows it.
///
/// A choice or a toggle keeps one of its values, compared byte for byte. A
/// number keeps the canonical text of one of its stops.
///
/// @param spec the option
/// @param value the value as given
/// @return the text to keep, or nothing when the value is refused
std::optional<std::string> kept_value(const OptionSpec& spec, std::string_view value) {
    if (spec.type == OptionSpec::Type::number) {
        const std::optional<oamod::Number> number = read_number(value);
        if (!number)
            return std::nullopt;
        for (const oamod::Number& stop : stops_of(spec)) {
            if (oamod::compare_numbers(stop, *number) == 0)
                return oamod::canonical_number_text(stop);
        }
        return std::nullopt;
    }
    for (const std::string& allowed : spec.values)
        if (allowed == value)
            return allowed;
    return std::nullopt;
}

/// The default an option starts from: a number in its canonical text.
///
/// @param spec the option
/// @return the default
std::string default_value(const OptionSpec& spec) {
    if (spec.type == OptionSpec::Type::number) {
        if (const std::optional<oamod::Number> number = read_number(spec.fallback))
            return oamod::canonical_number_text(*number);
    }
    return spec.fallback;
}

/// The options a mutator holds, in its order. An unknown id is dropped.
/// A value that is not allowed, and an option the set does not name, take
/// the default.
///
/// @param mutator the mutator
/// @param given the values to keep, when they are allowed
/// @return the options
std::vector<std::pair<std::string, std::string>>
normalize(const Mutator& mutator, const std::vector<std::pair<std::string, std::string>>& given) {
    std::vector<std::pair<std::string, std::string>> options;
    options.reserve(mutator.options.size());
    for (const OptionSpec& spec : mutator.options) {
        std::string value = default_value(spec);
        for (const auto& [option_id, stored] : given) {
            if (option_id != spec.id)
                continue;
            if (std::optional<std::string> kept = kept_value(spec, stored))
                value = std::move(*kept);
            break;
        }
        options.emplace_back(spec.id, std::move(value));
    }
    return options;
}

/// The clash mode an On mutator keeps.
///
/// @param mutator the mutator
/// @param inputs the host's inputs
/// @param given the mode already chosen; empty when none was
/// @return the mode, or empty when the mutator brings nothing
std::optional<Clash>
kept_clash(const Mutator& mutator, const Inputs& inputs, std::optional<Clash> given) {
    if (given && mutator.brings)
        return given;
    return detail::default_clash(mutator, inputs);
}

/// The ids in a closure that need each other.
///
/// @param graph the prerequisite graph
/// @param closure the closure
/// @return those ids; the closure's order when the loop cannot be named
std::vector<std::string>
cycle_ids(std::span<const library::Need> graph, const library::Closure& closure) {
    std::vector<std::string> loop;
    for (const std::string& item : closure.order) {
        const std::vector<std::string> dependants =
            library::dependants_of(graph, closure.order, item);
        if (detail::listed(dependants, item))
            loop.push_back(item);
    }
    if (loop.empty())
        return closure.order;
    return loop;
}

/// Tells whether a prerequisite's version meets the range its dependant asks for.
///
/// An empty range is any version. A range that does not read is not met.
///
/// @param need the dependant's record of the prerequisite
/// @param version the prerequisite's version, as written
/// @return true when the range is met
bool range_met(const library::Prerequisite& need, std::string_view version) {
    if (need.range.empty())
        return true;
    oamut::Condition condition;
    condition.id = need.id;
    condition.range_text = need.range;
    condition.range = oamut::parse_version_range(need.range);
    if (!condition.range)
        return false;
    return oamut::condition_met(condition, need.id, version);
}

/// The name a prerequisite is called by: its record's name, else the
/// mutator's, else its id.
///
/// @param picker the picker
/// @param need the dependant's record of the prerequisite
/// @return the name
std::string need_name(const Picker& picker, const library::Prerequisite& need) {
    if (!need.name.empty())
        return need.name;
    return detail::display_name_of(picker, need.id);
}

/// The reason for a prerequisite that cannot be had at a version in range.
///
/// @param picker the picker
/// @param need the dependant's record of the prerequisite
/// @return "Needs {name} {range}, which no registry offers", or the same
///         without the range when it names none
std::string not_offered(const Picker& picker, const library::Prerequisite& need) {
    const library::TextHooks& text = picker.inputs.text;
    const std::string range = library::range_words(need.range, text);
    if (range.empty()) {
        return library::filled(
            text, words::blocked_prerequisite_any, {{"name", need_name(picker, need)}}
        );
    }
    return library::filled(
        text,
        library::words::blocked_prerequisite,
        {{"name", need_name(picker, need)}, {"range", range}}
    );
}

/// A locked row's line for a reason that is not a conflict.
///
/// @param picker the picker
/// @param reason the reason
/// @return "Not compatible with this mod: {reason}"
std::string not_compatible(const Picker& picker, const std::string& reason) {
    return library::filled(picker.inputs.text, words::line_not_compatible, {{"reason", reason}});
}

/// A locked row's line for a conflict.
///
/// @param picker the picker
/// @param other the mutator it cannot run beside
/// @return "Cannot run beside {name}"
std::string conflict_line(const Picker& picker, const Mutator& other) {
    return library::filled(
        picker.inputs.text, words::line_conflicts, {{"name", detail::display_name(other)}}
    );
}

} // namespace

namespace detail {

Chosen with_defaults(const Picker& picker, const Mutator& mutator) {
    Chosen chosen;
    chosen.id = mutator.id;
    chosen.options = normalize(mutator, {});
    chosen.clash = kept_clash(mutator, picker.inputs, std::nullopt);
    return chosen;
}

std::string blocked_line(
    const Picker& picker,
    std::span<const library::Need> graph,
    std::span<const std::string> on,
    std::string_view id
) {
    const Mutator* self = find_mutator(picker, id);
    if (self == nullptr)
        return {};
    if (!self->unavailable.empty())
        return not_compatible(picker, self->unavailable);
    const std::string roots[]{std::string(id)};
    const library::Closure closure = library::prerequisite_closure(graph, roots);
    if (closure.cyclic) {
        const library::TextHooks& text = picker.inputs.text;
        return not_compatible(
            picker,
            library::filled(
                text,
                library::words::blocked_cycle,
                {{"ids", library::join_names(text, cycle_ids(graph, closure))}}
            )
        );
    }
    for (const std::string& item : closure.order) {
        const Mutator* holder = find_mutator(picker, item);
        if (holder == nullptr)
            continue;
        for (const library::Prerequisite& need : holder->needs) {
            if (need.id.empty())
                continue;
            const Mutator* needed = find_mutator(picker, need.id);
            if (needed == nullptr || !range_met(need, needed->version))
                return not_compatible(picker, not_offered(picker, need));
            if (!needed->unavailable.empty())
                return not_compatible(picker, needed->unavailable);
        }
    }
    // The mutator and the prerequisites it would bring, against the On list
    // and against each other. Pairs already on are not this mutator's.
    std::vector<const Mutator*> coming;
    for (const std::string& item : closure.order) {
        if (item != id && listed(on, item))
            continue;
        if (const Mutator* one = find_mutator(picker, item))
            coming.push_back(one);
    }
    for (const Mutator* one : coming) {
        for (const std::string& other_id : on) {
            if (other_id == one->id)
                continue;
            const Mutator* other = find_mutator(picker, other_id);
            if (other != nullptr && conflicts(*one, *other))
                return conflict_line(picker, *other);
        }
    }
    for (std::size_t first = 0; first < coming.size(); ++first) {
        for (std::size_t second = first + 1; second < coming.size(); ++second) {
            if (!conflicts(*coming[first], *coming[second]))
                continue;
            const Mutator& named = coming[second]->id == id ? *coming[first] : *coming[second];
            return conflict_line(picker, named);
        }
    }
    return {};
}

} // namespace detail

namespace {

/// Where a mutator that is not yet on is inserted, clamped.
///
/// The place is below every On prerequisite and above every On dependant.
/// The end is `on.size()`. When the On list is already out of order, the
/// place keeps the prerequisites' side and mending repairs the rest.
///
/// @param picker the picker
/// @param graph the prerequisite graph
/// @param closure the mutator's closure
/// @param id the mutator
/// @param requested the place asked for; the end when none
/// @return the index to insert at
std::size_t insertion_at(
    const Picker& picker,
    const std::vector<library::Need>& graph,
    const library::Closure& closure,
    std::string_view id,
    std::optional<std::size_t> requested
) {
    std::size_t earliest = 0;
    for (const std::string& need : closure.order) {
        if (need == id)
            continue;
        const std::optional<std::size_t> at = detail::on_index(picker, need);
        if (at && *at + 1 > earliest)
            earliest = *at + 1;
    }
    std::size_t latest = picker.on.size();
    const std::vector<std::string> ids = detail::on_ids(picker);
    const std::vector<std::string> dependants = library::dependants_of(graph, ids, id);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (!detail::listed(dependants, ids[index]))
            continue;
        if (index < latest)
            latest = index;
    }
    if (latest < earliest)
        latest = earliest;
    std::size_t at = requested.value_or(picker.on.size());
    if (at < earliest)
        at = earliest;
    if (at > latest)
        at = latest;
    return at;
}

/// Inserts `chosen` just above the highest On mutator that needs it.
///
/// At the end when none of them is on.
///
/// @param[in,out] picker the picker
/// @param graph the prerequisite graph
/// @param chosen the mutator
void insert_above_dependant(
    Picker& picker, const std::vector<library::Need>& graph, Chosen chosen
) {
    const std::vector<std::string> ids = detail::on_ids(picker);
    const std::vector<std::string> dependants = library::dependants_of(graph, ids, chosen.id);
    std::size_t at = picker.on.size();
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (detail::listed(dependants, ids[index])) {
            at = index;
            break;
        }
    }
    picker.on.insert(picker.on.begin() + static_cast<std::ptrdiff_t>(at), std::move(chosen));
}

/// Lifts each On mutator above the first dependant that precedes it.
///
/// A pass walks the order it started with. It stops when a pass moves
/// nothing, or the order already keeps every prerequisite above what needs
/// it, and after at most one pass per mutator.
///
/// @param[in,out] picker the picker
void mend(Picker& picker) {
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::size_t passes = picker.on.size();
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const std::vector<std::string> order = detail::on_ids(picker);
        if (library::prerequisites_above(graph, order))
            return;
        bool moved = false;
        for (const std::string& id : order) {
            const std::vector<std::string> current = detail::on_ids(picker);
            const std::optional<std::size_t> at = detail::on_index(picker, id);
            if (!at)
                continue;
            const std::vector<std::string> dependants = library::dependants_of(graph, current, id);
            std::optional<std::size_t> first;
            for (std::size_t index = 0; index < *at; ++index) {
                if (detail::listed(dependants, current[index])) {
                    first = index;
                    break;
                }
            }
            if (!first)
                continue;
            Chosen chosen = std::move(picker.on[*at]);
            picker.on.erase(picker.on.begin() + static_cast<std::ptrdiff_t>(*at));
            picker.on.insert(
                picker.on.begin() + static_cast<std::ptrdiff_t>(*first), std::move(chosen)
            );
            moved = true;
        }
        if (!moved)
            return;
    }
}

/// The first On mutator, or the first mutator by name.
///
/// @param[in,out] picker the picker
void select_first(Picker& picker) {
    if (!picker.on.empty()) {
        picker.selected = picker.on.front().id;
        return;
    }
    const Mutator* best = nullptr;
    for (const Mutator& mutator : picker.inputs.mutators) {
        if (best == nullptr) {
            best = &mutator;
            continue;
        }
        const std::string left = detail::folded(detail::display_name(mutator));
        const std::string right = detail::folded(detail::display_name(*best));
        if (left < right || (left == right && mutator.id < best->id))
            best = &mutator;
    }
    if (best != nullptr)
        picker.selected = best->id;
    else
        picker.selected.reset();
}

/// Appends a name to `left_out` when it is not there yet.
///
/// @param[in,out] picker the picker
/// @param name the name
void leave_out(Picker& picker, std::string name) {
    if (!detail::listed(picker.left_out, name))
        picker.left_out.push_back(std::move(name));
}

/// Makes the On list one a game can start with.
///
/// Mends the order, then reads the list from the top. A mutator that is
/// locked beside those kept above it is switched off and named in
/// `left_out`: of two that conflict, the later one goes, and a mutator that
/// needs one that went is locked for the same reason and goes too. One that
/// is not locked is kept after the prerequisites it lacks: a prerequisite
/// still further down the list is moved up with its options, any other is
/// brought with its defaults and recorded in `came_with`. Then at most
/// `most_on` stay on, the rest named in `left_out`. Records and a question
/// that name a mutator now off are dropped.
///
/// @param[in,out] picker the picker
void settle(Picker& picker) {
    mend(picker);
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    std::vector<Chosen> waiting = std::move(picker.on);
    std::vector<bool> taken(waiting.size(), false);
    picker.on.clear();
    std::vector<std::string> kept;
    for (std::size_t index = 0; index < waiting.size(); ++index) {
        if (taken[index] || detail::listed(kept, waiting[index].id))
            continue;
        taken[index] = true;
        const std::string id = waiting[index].id;
        if (!detail::blocked_line(picker, graph, kept, id).empty()) {
            leave_out(picker, detail::display_name_of(picker, id));
            continue;
        }
        const std::string roots[]{id};
        const library::Closure closure = library::prerequisite_closure(graph, roots);
        for (const std::string& item : closure.order) {
            if (item == id || detail::listed(kept, item))
                continue;
            const Mutator* one = detail::find_mutator(picker, item);
            if (one == nullptr)
                continue;
            std::optional<Chosen> later;
            for (std::size_t below = index + 1; below < waiting.size(); ++below) {
                if (!taken[below] && waiting[below].id == item) {
                    taken[below] = true;
                    later = std::move(waiting[below]);
                    break;
                }
            }
            if (later) {
                picker.on.push_back(std::move(*later));
            } else {
                picker.on.push_back(detail::with_defaults(picker, *one));
                CameWith record;
                record.prerequisite = item;
                record.dependant = id;
                picker.came_with.push_back(std::move(record));
            }
            kept.push_back(item);
        }
        picker.on.push_back(std::move(waiting[index]));
        kept.push_back(id);
    }
    while (picker.on.size() > most_on) {
        leave_out(picker, detail::display_name_of(picker, picker.on.back().id));
        picker.on.pop_back();
    }
    std::vector<CameWith> records;
    for (const CameWith& record : picker.came_with) {
        if (detail::on_index(picker, record.prerequisite) &&
            detail::on_index(picker, record.dependant))
            records.push_back(record);
    }
    picker.came_with = std::move(records);
    if (picker.asking && !detail::on_index(picker, picker.asking->id))
        picker.asking.reset();
}

/// Removes `id` from the On list and drops the records that name it.
///
/// @param[in,out] picker the picker
/// @param id the mutator
void take_off(Picker& picker, std::string_view id) {
    const std::optional<std::size_t> at = detail::on_index(picker, id);
    if (!at)
        return;
    picker.on.erase(picker.on.begin() + static_cast<std::ptrdiff_t>(*at));
    detail::drop_came_with(picker, id);
}

} // namespace

void open(Picker& picker, Inputs inputs) {
    Picker next;
    next.inputs = std::move(inputs);
    for (const Chosen& chosen : next.inputs.last_set) {
        const Mutator* mutator = detail::find_mutator(next, chosen.id);
        if (mutator == nullptr) {
            leave_out(next, chosen.id);
            continue;
        }
        if (detail::on_index(next, chosen.id))
            continue;
        Chosen placed;
        placed.id = chosen.id;
        placed.options = normalize(*mutator, chosen.options);
        placed.clash = kept_clash(*mutator, next.inputs, chosen.clash);
        next.on.push_back(std::move(placed));
    }
    settle(next);
    next.from_last_set = next.inputs.remembered && !next.on.empty();
    select_first(next);
    picker = std::move(next);
}

void refresh(Picker& picker, Inputs inputs) {
    picker.inputs = std::move(inputs);
    std::vector<Chosen> kept;
    kept.reserve(picker.on.size());
    for (Chosen& chosen : picker.on) {
        const Mutator* mutator = detail::find_mutator(picker, chosen.id);
        if (mutator == nullptr) {
            leave_out(picker, chosen.id);
            continue;
        }
        chosen.options = normalize(*mutator, chosen.options);
        chosen.clash = kept_clash(*mutator, picker.inputs, chosen.clash);
        kept.push_back(std::move(chosen));
    }
    picker.on = std::move(kept);
    settle(picker);
    if (picker.selected && detail::find_mutator(picker, *picker.selected) == nullptr)
        select_first(picker);
}

SwitchResult switch_on(Picker& picker, std::string_view id, std::optional<std::size_t> place) {
    SwitchResult outcome;
    if (picker.inputs.read_only) {
        outcome.refused = std::string(library::shown(picker.inputs.text, words::hint_view));
        return outcome;
    }
    const Mutator* mutator = detail::find_mutator(picker, id);
    if (mutator == nullptr || detail::on_index(picker, id))
        return outcome;
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::vector<std::string> ids = detail::on_ids(picker);
    outcome.refused = detail::blocked_line(picker, graph, ids, id);
    if (!outcome.refused.empty())
        return outcome;
    const std::string roots[]{std::string(id)};
    const library::Closure closure = library::prerequisite_closure(graph, roots);
    std::size_t added = 0;
    for (const std::string& item : closure.order) {
        if (!detail::listed(ids, item))
            ++added;
    }
    if (picker.on.size() + added > most_on) {
        outcome.refused = library::filled(
            picker.inputs.text, words::refused_cap, {{"count", std::to_string(most_on)}}
        );
        return outcome;
    }

    const std::size_t at = insertion_at(picker, graph, closure, id, place);
    picker.on.insert(
        picker.on.begin() + static_cast<std::ptrdiff_t>(at), detail::with_defaults(picker, *mutator)
    );
    outcome.changed = true;
    for (const std::string& item : closure.order) {
        if (item == id || detail::on_index(picker, item))
            continue;
        const Mutator* one = detail::find_mutator(picker, item);
        if (one == nullptr)
            continue;
        insert_above_dependant(picker, graph, detail::with_defaults(picker, *one));
        CameWith record;
        record.prerequisite = item;
        record.dependant = std::string(id);
        picker.came_with.push_back(std::move(record));
        outcome.brought.push_back(item);
    }
    mend(picker);
    return outcome;
}

std::optional<TurnOff> switch_off(Picker& picker, std::string_view id) {
    if (picker.inputs.read_only)
        return std::nullopt;
    if (!detail::on_index(picker, id))
        return std::nullopt;
    TurnOff question;
    question.id = std::string(id);
    question.dependants = detail::on_dependants(picker, id);
    if (!question.dependants.empty()) {
        picker.asking = question;
        return question;
    }
    take_off(picker, id);
    if (picker.asking && picker.asking->id == id)
        picker.asking.reset();
    return std::nullopt;
}

void confirm_turn_off(Picker& picker) {
    if (picker.inputs.read_only || !picker.asking)
        return;
    const std::string id = picker.asking->id;
    picker.asking.reset();
    if (!detail::on_index(picker, id))
        return;
    std::vector<std::string> dropping = detail::on_dependants(picker, id);
    dropping.push_back(id);
    for (const std::string& item : dropping)
        take_off(picker, item);
}

void cancel_turn_off(Picker& picker) {
    picker.asking.reset();
}

Band allowed_places(const Picker& picker, std::string_view id) {
    const std::optional<std::size_t> at = detail::on_index(picker, id);
    if (!at)
        return {};
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::string roots[]{std::string(id)};
    const library::Closure closure = library::prerequisite_closure(graph, roots);
    std::size_t first = 0;
    for (const std::string& need : closure.order) {
        if (need == id)
            continue;
        const std::optional<std::size_t> need_at = detail::on_index(picker, need);
        if (need_at && *need_at + 1 > first)
            first = *need_at + 1;
    }
    const std::vector<std::string> ids = detail::on_ids(picker);
    const std::vector<std::string> dependants = library::dependants_of(graph, ids, id);
    std::optional<std::size_t> highest;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (ids[index] == id)
            continue;
        if (!detail::listed(dependants, ids[index]))
            continue;
        if (!highest || index < *highest)
            highest = index;
    }
    if (highest && *highest == 0)
        return Band{*at, *at};
    std::size_t last = picker.on.size() - 1;
    if (highest)
        last = *highest - 1;
    if (first > last)
        return Band{*at, *at};
    return Band{first, last};
}

bool move_to(Picker& picker, std::string_view id, std::size_t place) {
    if (picker.inputs.read_only)
        return false;
    const std::optional<std::size_t> at = detail::on_index(picker, id);
    if (!at)
        return false;
    const Band band = allowed_places(picker, id);
    if (place < band.first)
        place = band.first;
    if (place > band.last)
        place = band.last;
    if (place == *at)
        return false;
    Chosen chosen = std::move(picker.on[*at]);
    picker.on.erase(picker.on.begin() + static_cast<std::ptrdiff_t>(*at));
    if (place > picker.on.size())
        place = picker.on.size();
    picker.on.insert(picker.on.begin() + static_cast<std::ptrdiff_t>(place), std::move(chosen));
    return true;
}

bool move_up(Picker& picker, std::string_view id) {
    const std::optional<std::size_t> at = detail::on_index(picker, id);
    if (!at || *at == 0)
        return false;
    return move_to(picker, id, *at - 1);
}

bool move_down(Picker& picker, std::string_view id) {
    const std::optional<std::size_t> at = detail::on_index(picker, id);
    if (!at)
        return false;
    return move_to(picker, id, *at + 1);
}

std::optional<TurnOff> drop(Picker& picker, std::string_view id, Drop where) {
    if (picker.inputs.read_only)
        return std::nullopt;
    const bool on = detail::on_index(picker, id).has_value();
    if (where.into_on) {
        if (on)
            std::ignore = move_to(picker, id, where.place);
        else
            std::ignore = switch_on(picker, id, where.place);
        return std::nullopt;
    }
    if (!on)
        return std::nullopt;
    return switch_off(picker, id);
}

bool set_option(
    Picker& picker, std::string_view id, std::string_view option, std::string_view value
) {
    if (picker.inputs.read_only)
        return false;
    const std::optional<std::size_t> at = detail::on_index(picker, id);
    if (!at)
        return false;
    const Mutator* mutator = detail::find_mutator(picker, id);
    if (mutator == nullptr)
        return false;
    const OptionSpec* spec = find_option(*mutator, option);
    if (spec == nullptr)
        return false;
    std::optional<std::string> kept = kept_value(*spec, value);
    if (!kept)
        return false;
    Chosen& chosen = picker.on[*at];
    for (auto& [option_id, stored] : chosen.options) {
        if (option_id != option)
            continue;
        stored = std::move(*kept);
        return true;
    }
    chosen.options.emplace_back(std::string(option), std::move(*kept));
    return true;
}

std::vector<std::string> number_values(const OptionSpec& spec) {
    std::vector<std::string> values;
    const std::vector<oamod::Number> stops = stops_of(spec);
    values.reserve(stops.size());
    for (const oamod::Number& stop : stops)
        values.push_back(oamod::canonical_number_text(stop));
    return values;
}

bool set_clash(Picker& picker, std::string_view id, Clash mode) {
    if (picker.inputs.read_only)
        return false;
    const std::optional<std::size_t> at = detail::on_index(picker, id);
    if (!at)
        return false;
    const Mutator* mutator = detail::find_mutator(picker, id);
    if (mutator == nullptr || !mutator->brings)
        return false;
    picker.on[*at].clash = mode;
    return true;
}

std::vector<Chosen> result(const Picker& picker) {
    return picker.on;
}

bool ready(const Picker& picker) {
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::vector<std::string> ids = detail::on_ids(picker);
    if (!library::prerequisites_above(graph, ids))
        return false;
    for (const Chosen& chosen : picker.on) {
        const Mutator* mutator = detail::find_mutator(picker, chosen.id);
        if (mutator == nullptr || !mutator->installed)
            return false;
        if (!detail::blocked_line(picker, graph, ids, chosen.id).empty())
            return false;
        const std::string roots[]{chosen.id};
        const library::Closure closure = library::prerequisite_closure(graph, roots);
        for (const std::string& item : closure.order) {
            if (!detail::listed(ids, item))
                return false;
        }
    }
    return true;
}

} // namespace oa::ui::mutator_picker
