// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts the mutator picker passes to the interface catalogue. oa-tool
// joins this list with the other modules' lists.

#pragma once

#include "oa/ui/mutator_picker/text.hpp"

#include <string_view>

namespace oa::ui::mutator_picker {

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    words::clash_add,
    words::refused_cap,
    words::turn_off_kept_one,
    words::line_conflicts,
    words::verdict_checked,
    words::verdict_checking,
    words::hint_campaign,
    words::line_cycle,
    words::hint_left_out,
    words::line_needed_by,
    words::verdict_no_effect,
    words::line_no_effect,
    words::summary_none,
    words::line_not_compatible,
    words::verdict_not_compatible,
    words::line_not_installed,
    words::clash_replace,
    words::verdict_failed,
    words::line_failed,
    words::hint_view,
    words::turn_off_many,
    words::turn_off_kept_many,
    words::turn_off_one,
    words::clash_label,
    words::hint_last_campaign,
    words::hint_last_multiplayer,
    words::hint_last_skirmish,
    words::summary_came_with,
    words::summary_on,
    words::option_line,
    words::cancel,
    words::turn_off,
    words::turn_off_title,
};

} // namespace oa::ui::mutator_picker
