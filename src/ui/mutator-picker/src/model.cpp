// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which mutators are on, in which order, and with which option values.
// Prerequisites come from the Library's closure, and their version ranges
// are checked with the manifest's condition matcher. A number is compared as
// decimal text, never as a floating-point value. Nothing here reads a file,
// a clock, a preference or the network.
#include "oa/ui/mutator_picker/model.hpp"

#include "internal.hpp"

#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <algorithm>
#include <cstddef>
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

/// A decimal kept as digits. The value is ± digits × 10^(−scale).
struct Decimal {
    bool negative = false;
    std::string digits{"0"};
    int scale = 0;
};

/// Drops leading zeros, leaving one zero when the text is all zeros.
///
/// @param text the digits
/// @return the digits
std::string strip_zeros(std::string text) {
    std::size_t start = 0;
    while (start + 1 < text.size() && text[start] == '0')
        ++start;
    if (start > 0)
        text.erase(0, start);
    if (text.empty())
        return "0";
    return text;
}

/// Compares two magnitudes. Both have no leading zeros.
///
/// @param left one magnitude
/// @param right the other
/// @return negative when left is smaller, positive when it is larger, else 0
int compare_magnitude(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return left.size() < right.size() ? -1 : 1;
    if (left == right)
        return 0;
    return left < right ? -1 : 1;
}

/// Adds two magnitudes.
///
/// @param left one magnitude
/// @param right the other
/// @return the sum, without leading zeros
std::string add_magnitude(std::string_view left, std::string_view right) {
    std::string sum_text;
    sum_text.reserve(std::max(left.size(), right.size()) + 1);
    int carry = 0;
    std::size_t index = 0;
    while (index < left.size() || index < right.size() || carry != 0) {
        int digit_sum = carry;
        if (index < left.size())
            digit_sum += left[left.size() - 1 - index] - '0';
        if (index < right.size())
            digit_sum += right[right.size() - 1 - index] - '0';
        sum_text.push_back(static_cast<char>('0' + (digit_sum % 10)));
        carry = digit_sum / 10;
        ++index;
    }
    std::reverse(sum_text.begin(), sum_text.end());
    return strip_zeros(std::move(sum_text));
}

/// Subtracts two magnitudes. `left` is at least `right`.
///
/// @param left the larger magnitude
/// @param right the smaller
/// @return the difference, without leading zeros
std::string subtract_magnitude(std::string_view left, std::string_view right) {
    std::string difference;
    difference.reserve(left.size());
    int borrow = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        int top = (left[left.size() - 1 - index] - '0') - borrow;
        int bottom = 0;
        if (index < right.size())
            bottom = right[right.size() - 1 - index] - '0';
        if (top < bottom) {
            top += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        difference.push_back(static_cast<char>('0' + (top - bottom)));
    }
    std::reverse(difference.begin(), difference.end());
    return strip_zeros(std::move(difference));
}

/// Tells whether `dividend` is a multiple of `divisor`. Both are magnitudes.
///
/// @param dividend the magnitude
/// @param divisor the step's magnitude; zero is not a step
/// @return true when the remainder is zero
bool remainder_zero(std::string_view dividend, std::string_view divisor) {
    if (divisor == "0")
        return false;
    if (dividend == "0")
        return true;
    std::string remainder = "0";
    for (const char digit : dividend) {
        if (remainder == "0")
            remainder.assign(1, digit);
        else
            remainder.push_back(digit);
        remainder = strip_zeros(std::move(remainder));
        int taken = 0;
        while (compare_magnitude(remainder, divisor) >= 0 && taken < 9) {
            remainder = subtract_magnitude(remainder, divisor);
            ++taken;
        }
        if (compare_magnitude(remainder, divisor) >= 0)
            return false;
    }
    return remainder == "0";
}

/// The most digits a number may use. A longer text is refused.
constexpr std::size_t most_digits = 4096;

/// Reads a decimal. The grammar is an optional sign, digits, an optional
/// fraction and an optional exponent. A leading plus is allowed.
///
/// @param text the text
/// @param[out] parsed the number, when the text matches
/// @return false when the text does not match, or it is too long
bool parse_decimal(std::string_view text, Decimal& parsed) {
    if (text.empty() || text.size() > most_digits)
        return false;
    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '+' || text[index] == '-') {
        negative = text[index] == '-';
        ++index;
        if (index >= text.size())
            return false;
    }
    if (text[index] < '0' || text[index] > '9')
        return false;
    const std::size_t whole_at = index;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9')
        ++index;
    const std::string whole(text.substr(whole_at, index - whole_at));
    std::string fraction;
    if (index < text.size() && text[index] == '.') {
        ++index;
        if (index >= text.size() || text[index] < '0' || text[index] > '9')
            return false;
        const std::size_t fraction_at = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9')
            ++index;
        fraction.assign(text.substr(fraction_at, index - fraction_at));
    }
    int exponent = 0;
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index >= text.size())
            return false;
        bool exponent_negative = false;
        if (text[index] == '+' || text[index] == '-') {
            exponent_negative = text[index] == '-';
            ++index;
        }
        if (index >= text.size() || text[index] < '0' || text[index] > '9')
            return false;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            if (exponent > 409)
                return false;
            exponent = exponent * 10 + (text[index] - '0');
            ++index;
        }
        if (exponent_negative)
            exponent = -exponent;
    }
    if (index != text.size())
        return false;
    if (whole.size() + fraction.size() > most_digits)
        return false;
    std::string magnitude = whole + fraction;
    int scale = static_cast<int>(fraction.size());
    if (exponent > 0) {
        if (exponent > scale) {
            const int extra = exponent - scale;
            if (magnitude.size() + static_cast<std::size_t>(extra) > most_digits)
                return false;
            magnitude.append(static_cast<std::size_t>(extra), '0');
            scale = 0;
        } else {
            scale -= exponent;
        }
    } else if (exponent < 0) {
        const long grown = static_cast<long>(scale) + static_cast<long>(-exponent);
        if (grown > static_cast<long>(most_digits))
            return false;
        scale = static_cast<int>(grown);
    }
    magnitude = strip_zeros(std::move(magnitude));
    Decimal number;
    number.negative = magnitude != "0" && negative;
    number.digits = std::move(magnitude);
    number.scale = number.digits == "0" ? 0 : scale;
    parsed = std::move(number);
    return true;
}

/// Writes the magnitude at `scale`, which is at least the number's own.
///
/// Zero stays `"0"` at every scale.
///
/// @param number the number
/// @param scale the scale to write
/// @return the magnitude
std::string scaled(const Decimal& number, int scale) {
    if (number.digits == "0")
        return "0";
    std::string magnitude = number.digits;
    if (scale > number.scale)
        magnitude.append(static_cast<std::size_t>(scale - number.scale), '0');
    return magnitude;
}

/// Compares two decimals.
///
/// @param left one number
/// @param right the other
/// @return negative when left is smaller, positive when it is larger, else 0
int compare_decimal(const Decimal& left, const Decimal& right) {
    const bool left_zero = left.digits == "0";
    const bool right_zero = right.digits == "0";
    if (left_zero && right_zero)
        return 0;
    if (left_zero)
        return right.negative ? 1 : -1;
    if (right_zero)
        return left.negative ? -1 : 1;
    if (left.negative != right.negative)
        return left.negative ? -1 : 1;
    const int scale = std::max(left.scale, right.scale);
    const int magnitude = compare_magnitude(scaled(left, scale), scaled(right, scale));
    return left.negative ? -magnitude : magnitude;
}

/// Tells whether `value - least` is a non-negative multiple of `step`.
///
/// @param value the value
/// @param least the range's low end
/// @param step the step, already known to be positive
/// @return true when the value sits on a step
bool on_step(const Decimal& value, const Decimal& least, const Decimal& step) {
    const int scale = std::max(value.scale, std::max(least.scale, step.scale));
    const std::string left = scaled(value, scale);
    const std::string right = scaled(least, scale);
    std::string gap;
    if (!value.negative && !least.negative)
        gap = subtract_magnitude(left, right);
    else if (value.negative && least.negative)
        gap = subtract_magnitude(right, left);
    else
        gap = add_magnitude(left, right);
    return remainder_zero(gap, scaled(step, scale));
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

/// Tells whether a number lies in range, on its step.
///
/// @param spec the number option
/// @param value the text
/// @return true when the text is allowed
bool number_allowed(const OptionSpec& spec, std::string_view value) {
    Decimal parsed;
    Decimal least;
    Decimal most;
    Decimal step;
    if (!parse_decimal(value, parsed) || !parse_decimal(spec.least, least) ||
        !parse_decimal(spec.most, most) || !parse_decimal(spec.step, step))
        return false;
    if (step.negative || step.digits == "0")
        return false;
    if (compare_decimal(parsed, least) < 0 || compare_decimal(parsed, most) > 0)
        return false;
    return on_step(parsed, least, step);
}

/// Tells whether `value` is one the option allows.
///
/// @param spec the option
/// @param value the text
/// @return true when it can be stored
bool value_allowed(const OptionSpec& spec, std::string_view value) {
    if (spec.type == OptionSpec::Type::number)
        return number_allowed(spec, value);
    for (const std::string& allowed : spec.values)
        if (allowed == value)
            return true;
    return false;
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
        std::string value = spec.fallback;
        for (const auto& [option_id, stored] : given) {
            if (option_id != spec.id)
                continue;
            if (value_allowed(spec, stored))
                value = stored;
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
    if (!mutator.brings)
        return std::nullopt;
    if (given)
        return given;
    if (mutator.clashes)
        return mutator.clashes;
    return inputs.clash_default;
}

/// A mutator switched on with its default options and clash mode.
///
/// @param picker the picker
/// @param mutator the mutator
/// @return its On record
Chosen with_defaults(const Picker& picker, const Mutator& mutator) {
    Chosen chosen;
    chosen.id = mutator.id;
    chosen.options = normalize(mutator, {});
    chosen.clash = kept_clash(mutator, picker.inputs, std::nullopt);
    return chosen;
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

/// Writes a version range in the Library's words for engine and mutator ranges.
///
/// @param text how texts are looked up
/// @param range_text the range as the manifest writes it
/// @return the words, such as "4.0 or later"; the range as written when it
///         does not read; empty for an empty range
std::string range_words(const library::TextHooks& text, std::string_view range_text) {
    if (range_text.empty())
        return {};
    const std::optional<oamut::VersionRange> range = oamut::parse_version_range(range_text);
    if (!range)
        return std::string(range_text);
    std::string joined;
    for (const auto& [comparison, version] : range->terms) {
        if (!joined.empty()) {
            joined += library::shown(text, library::words::list_separator);
            joined.push_back(' ');
        }
        std::string_view pattern = library::words::engine_at_least;
        switch (comparison) {
        case oamod::EngineComparison::at_least:
            pattern = library::words::engine_at_least;
            break;
        case oamod::EngineComparison::above:
            pattern = library::words::engine_above;
            break;
        case oamod::EngineComparison::at_most:
            pattern = library::words::engine_at_most;
            break;
        case oamod::EngineComparison::below:
            pattern = library::words::engine_below;
            break;
        case oamod::EngineComparison::exactly:
            pattern = library::words::engine_exactly;
            break;
        }
        joined += library::filled(text, pattern, {{"version", oamut::version_text(version)}});
    }
    return joined;
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
    const std::string range = range_words(text, need.range);
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
                {{"ids", join_names(text, cycle_ids(graph, closure))}}
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
                picker.on.push_back(with_defaults(picker, *one));
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
        picker.on.begin() + static_cast<std::ptrdiff_t>(at), with_defaults(picker, *mutator)
    );
    outcome.changed = true;
    for (const std::string& item : closure.order) {
        if (item == id || detail::on_index(picker, item))
            continue;
        const Mutator* one = detail::find_mutator(picker, item);
        if (one == nullptr)
            continue;
        insert_above_dependant(picker, graph, with_defaults(picker, *one));
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
    if (spec == nullptr || !value_allowed(*spec, value))
        return false;
    Chosen& chosen = picker.on[*at];
    for (auto& [option_id, stored] : chosen.options) {
        if (option_id != option)
            continue;
        stored = std::string(value);
        return true;
    }
    chosen.options.emplace_back(std::string(option), std::string(value));
    return true;
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
