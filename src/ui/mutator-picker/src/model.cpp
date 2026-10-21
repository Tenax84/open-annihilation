// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which mutators are on, in which order, and with which option values.
// Prerequisites come from the Library's closure. A number is compared as
// decimal text, never as a floating-point value. Nothing here reads a file,
// a clock, a preference or the network.
#include "oa/ui/mutator_picker/model.hpp"

#include "internal.hpp"

#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::ui::mutator_picker {
namespace {

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

/// The ids in a closure that need each other.
///
/// @param graph the prerequisite graph
/// @param closure the closure
/// @return those ids; the closure's order when the loop cannot be named
std::vector<std::string>
cycle_ids(const std::vector<library::Need>& graph, const library::Closure& closure) {
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

/// Joins ids for the cycle sentence.
///
/// @param picker the picker
/// @param ids the ids
/// @return the sentence
std::string cycle_sentence(const Picker& picker, const std::vector<std::string>& ids) {
    return library::filled(
        picker.inputs.text,
        words::line_cycle,
        {{"names", detail::join_names(picker.inputs.text, ids)}}
    );
}

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

/// What switching one mutator on decided, before the picker is changed.
struct Admission {
    bool changed = false;
    std::vector<std::string> brought;
    std::string refused;
    std::vector<std::string> cycle;
};

/// An On mutator `id` cannot run beside, naming the one a player already has on
/// when there is one.
///
/// @param picker the picker
/// @param graph_ids the ids that would be on
/// @param id the mutator being switched on
/// @return the sentence, or empty when nothing conflicts
std::string conflict_sentence(
    const Picker& picker, const std::vector<std::string>& graph_ids, std::string_view id
) {
    const Mutator* self = detail::find_mutator(picker, id);
    if (self != nullptr) {
        for (const Chosen& chosen : picker.on) {
            const Mutator* other = detail::find_mutator(picker, chosen.id);
            if (other != nullptr && detail::conflicts(*self, *other)) {
                return library::filled(
                    picker.inputs.text,
                    words::line_conflicts,
                    {{"name", detail::display_name(*other)}}
                );
            }
        }
    }
    for (const std::string& item : graph_ids) {
        const Mutator* one = detail::find_mutator(picker, item);
        if (one == nullptr)
            continue;
        for (const std::string& other_id : graph_ids) {
            if (other_id == item)
                continue;
            const Mutator* other = detail::find_mutator(picker, other_id);
            if (other != nullptr && detail::conflicts(*one, *other)) {
                return library::filled(
                    picker.inputs.text,
                    words::line_conflicts,
                    {{"name", detail::display_name(*other)}}
                );
            }
        }
    }
    return {};
}

/// Switches `id` on when it can be, bringing missing prerequisites.
///
/// `given` supplies the options and clash of a last set. Null starts from
/// the defaults. Refusals change nothing.
///
/// @param[in,out] picker the picker
/// @param id the mutator
/// @param place where it sits; the end when none
/// @param given the last set's record, or null
/// @param enforce_cap refuse when the On list would pass most_on
/// @param refuse_conflicts refuse when the set would hold a conflict
/// @return what changed
Admission admit(
    Picker& picker,
    std::string_view id,
    std::optional<std::size_t> place,
    const Chosen* given,
    bool enforce_cap,
    bool refuse_conflicts
) {
    if (picker.inputs.read_only) {
        Admission admission;
        admission.refused = std::string(library::shown(picker.inputs.text, words::hint_view));
        return admission;
    }
    const Mutator* mutator = detail::find_mutator(picker, id);
    if (mutator == nullptr)
        return {};
    if (detail::on_index(picker, id))
        return {};
    if (!mutator->unavailable.empty()) {
        Admission admission;
        admission.refused = library::filled(
            picker.inputs.text, words::line_not_compatible, {{"reason", mutator->unavailable}}
        );
        return admission;
    }
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::vector<std::string> roots{std::string(id)};
    const library::Closure closure = library::prerequisite_closure(graph, roots);
    if (closure.cyclic) {
        Admission admission;
        admission.cycle = cycle_ids(graph, closure);
        admission.refused = cycle_sentence(picker, admission.cycle);
        return admission;
    }
    for (const std::string& item : closure.order) {
        const Mutator* one = detail::find_mutator(picker, item);
        if (one == nullptr) {
            Admission admission;
            admission.refused =
                library::filled(picker.inputs.text, words::line_not_compatible, {{"reason", item}});
            return admission;
        }
        if (item != id && !one->unavailable.empty()) {
            Admission admission;
            admission.refused = library::filled(
                picker.inputs.text, words::line_not_compatible, {{"reason", one->unavailable}}
            );
            return admission;
        }
    }
    if (refuse_conflicts) {
        std::vector<std::string> future = detail::on_ids(picker);
        for (const std::string& item : closure.order) {
            if (!detail::listed(future, item))
                future.push_back(item);
        }
        const std::string conflict = conflict_sentence(picker, future, id);
        if (!conflict.empty()) {
            Admission admission;
            admission.refused = conflict;
            return admission;
        }
    }
    std::size_t added = 0;
    for (const std::string& item : closure.order) {
        if (!detail::on_index(picker, item))
            ++added;
    }
    if (enforce_cap && (added > most_on || picker.on.size() > most_on - added)) {
        Admission admission;
        admission.refused = library::filled(
            picker.inputs.text, words::refused_cap, {{"count", std::to_string(most_on)}}
        );
        return admission;
    }

    Chosen chosen;
    chosen.id = std::string(id);
    if (given != nullptr) {
        chosen.options = normalize(*mutator, given->options);
        chosen.clash = kept_clash(*mutator, picker.inputs, given->clash);
    } else {
        chosen.options = normalize(*mutator, {});
        chosen.clash = kept_clash(*mutator, picker.inputs, std::nullopt);
    }
    const std::size_t at = insertion_at(picker, graph, closure, id, place);
    picker.on.insert(picker.on.begin() + static_cast<std::ptrdiff_t>(at), std::move(chosen));

    Admission admission;
    admission.changed = true;
    for (const std::string& item : closure.order) {
        if (item == id || detail::on_index(picker, item))
            continue;
        const Mutator* one = detail::find_mutator(picker, item);
        if (one == nullptr)
            continue;
        Chosen brought;
        brought.id = item;
        brought.options = normalize(*one, {});
        brought.clash = kept_clash(*one, picker.inputs, std::nullopt);
        insert_above_dependant(picker, graph, std::move(brought));
        CameWith record;
        record.prerequisite = item;
        record.dependant = std::string(id);
        picker.came_with.push_back(std::move(record));
        admission.brought.push_back(item);
    }
    mend(picker);
    return admission;
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

/// Why a last-set mutator cannot be placed, ignoring conflicts and the cap.
///
/// @param picker the picker
/// @param id the mutator, known to be present and available
/// @return the block; empty when it can be placed
Admission placement_block(const Picker& picker, std::string_view id) {
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::vector<std::string> roots{std::string(id)};
    const library::Closure closure = library::prerequisite_closure(graph, roots);
    Admission admission;
    if (closure.cyclic) {
        admission.cycle = cycle_ids(graph, closure);
        admission.refused = cycle_sentence(picker, admission.cycle);
        return admission;
    }
    for (const std::string& item : closure.order) {
        const Mutator* one = detail::find_mutator(picker, item);
        if (one == nullptr) {
            admission.refused =
                library::filled(picker.inputs.text, words::line_not_compatible, {{"reason", item}});
            return admission;
        }
        if (item != id && !one->unavailable.empty()) {
            admission.refused = library::filled(
                picker.inputs.text, words::line_not_compatible, {{"reason", one->unavailable}}
            );
            return admission;
        }
    }
    return admission;
}

/// Brings prerequisites the On list does not already hold.
///
/// Each one is recorded against the On mutator whose closure named it, which
/// is the mutator the player switched on, not an intermediate.
///
/// @param[in,out] picker the picker
void bring_lacking(Picker& picker) {
    const std::vector<std::string> asked = detail::on_ids(picker);
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    for (const std::string& id : asked) {
        const std::vector<std::string> roots{id};
        const library::Closure closure = library::prerequisite_closure(graph, roots);
        if (closure.cyclic)
            continue;
        for (const std::string& item : closure.order) {
            if (item == id || detail::on_index(picker, item))
                continue;
            const Mutator* one = detail::find_mutator(picker, item);
            if (one == nullptr || !one->unavailable.empty())
                continue;
            Chosen brought;
            brought.id = item;
            brought.options = normalize(*one, {});
            brought.clash = kept_clash(*one, picker.inputs, std::nullopt);
            insert_above_dependant(picker, graph, std::move(brought));
            CameWith record;
            record.prerequisite = item;
            record.dependant = id;
            picker.came_with.push_back(std::move(record));
        }
    }
}

/// Appends a name to `left_out` when it is not there yet.
///
/// @param[in,out] picker the picker
/// @param name the name
void leave_out(Picker& picker, std::string name) {
    if (!detail::listed(picker.left_out, name))
        picker.left_out.push_back(std::move(name));
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
        if (!mutator->unavailable.empty()) {
            leave_out(next, detail::display_name(*mutator));
            continue;
        }
        if (detail::on_index(next, chosen.id))
            continue;
        const Admission blocked = placement_block(next, chosen.id);
        if (!blocked.cycle.empty()) {
            for (const std::string& item : blocked.cycle)
                leave_out(next, item);
            continue;
        }
        if (!blocked.refused.empty()) {
            leave_out(next, detail::display_name(*mutator));
            continue;
        }
        Chosen placed;
        placed.id = chosen.id;
        placed.options = normalize(*mutator, chosen.options);
        placed.clash = kept_clash(*mutator, next.inputs, chosen.clash);
        next.on.push_back(std::move(placed));
    }
    bring_lacking(next);
    mend(next);
    while (next.on.size() > most_on) {
        const std::string id = next.on.back().id;
        leave_out(next, detail::display_name_of(next, id));
        detail::drop_came_with(next, id);
        next.on.pop_back();
    }
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
            detail::drop_came_with(picker, chosen.id);
            continue;
        }
        chosen.options = normalize(*mutator, chosen.options);
        chosen.clash = kept_clash(*mutator, picker.inputs, chosen.clash);
        kept.push_back(std::move(chosen));
    }
    picker.on = std::move(kept);

    std::vector<CameWith> records;
    for (const CameWith& record : picker.came_with) {
        if (detail::on_index(picker, record.prerequisite) &&
            detail::on_index(picker, record.dependant))
            records.push_back(record);
    }
    picker.came_with = std::move(records);
    if (picker.asking && !detail::on_index(picker, picker.asking->id))
        picker.asking.reset();

    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::vector<std::string> order = detail::on_ids(picker);
    if (!library::prerequisites_above(graph, order))
        mend(picker);
    if (picker.selected && detail::find_mutator(picker, *picker.selected) == nullptr)
        select_first(picker);
}

SwitchResult switch_on(Picker& picker, std::string_view id, std::optional<std::size_t> place) {
    const Admission admission = admit(picker, id, place, nullptr, true, true);
    SwitchResult outcome;
    outcome.changed = admission.changed;
    outcome.brought = admission.brought;
    outcome.refused = admission.refused;
    return outcome;
}

std::optional<TurnOff> switch_off(Picker& picker, std::string_view id) {
    if (picker.inputs.read_only)
        return std::nullopt;
    const std::optional<std::size_t> at = detail::on_index(picker, id);
    if (!at)
        return std::nullopt;
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::vector<std::string> ids = detail::on_ids(picker);
    const std::vector<std::string> dependants = library::dependants_of(graph, ids, id);
    TurnOff question;
    question.id = std::string(id);
    for (const std::string& dependant : dependants) {
        if (dependant != id)
            question.dependants.push_back(dependant);
    }
    if (!question.dependants.empty()) {
        picker.asking = question;
        return question;
    }
    picker.on.erase(picker.on.begin() + static_cast<std::ptrdiff_t>(*at));
    detail::drop_came_with(picker, id);
    if (picker.asking && picker.asking->id == id)
        picker.asking.reset();
    return std::nullopt;
}

void confirm_turn_off(Picker& picker) {
    if (picker.inputs.read_only || !picker.asking)
        return;
    std::vector<std::string> dropping = picker.asking->dependants;
    dropping.push_back(picker.asking->id);
    for (const std::string& id : dropping) {
        const std::optional<std::size_t> at = detail::on_index(picker, id);
        if (!at)
            continue;
        picker.on.erase(picker.on.begin() + static_cast<std::ptrdiff_t>(*at));
        detail::drop_came_with(picker, id);
    }
    picker.asking.reset();
}

void cancel_turn_off(Picker& picker) {
    picker.asking.reset();
}

Band allowed_places(const Picker& picker, std::string_view id) {
    const std::optional<std::size_t> at = detail::on_index(picker, id);
    if (!at)
        return {};
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::vector<std::string> roots{std::string(id)};
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
    for (const Chosen& chosen : picker.on) {
        const Mutator* mutator = detail::find_mutator(picker, chosen.id);
        if (mutator == nullptr || !mutator->installed || !mutator->unavailable.empty())
            return false;
        if (detail::on_conflict(picker, *mutator) != nullptr)
            return false;
    }
    return true;
}

} // namespace oa::ui::mutator_picker
