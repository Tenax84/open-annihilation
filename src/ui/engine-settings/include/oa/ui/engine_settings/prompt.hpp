// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A prompt of Open Annihilation's own: a notice (notice.hpp) that asks a
// question with one to three buttons of its own captions, and can show a
// progress bar under its text. The prompt is the OA UI kit's question
// (kit::Question in oa/ui/kit/components_more.hpp) under its old names: it
// shares the notice's look and text placement, and its text is given
// finished, in the language shown, and drawn as given.
#pragma once

#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/kit/components_more.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace oa::ui::engine_settings {

/// The most buttons a prompt has.
inline constexpr std::size_t most_prompt_buttons = oa::ui::kit::most_question_buttons;
/// A full progress bar (Prompt::progress).
inline constexpr int32_t prompt_progress_whole = oa::ui::kit::question_progress_whole;
/// The control prompt_layout gives the progress bar's part.
inline constexpr int32_t prompt_bar_control = 100;

/// One of a prompt's buttons: the kit's question button.
using PromptButton = oa::ui::kit::QuestionButton;

/// One prompt: the kit's question.
using Prompt = oa::ui::kit::Question;

/// What an event on a prompt asks of the host: the kit's.
using PromptAction = oa::ui::kit::QuestionAction;

/// What an event on a prompt did: the kit's.
using PromptAnswer = oa::ui::kit::QuestionAnswer;

/// Returns a prompt's height: its header, its text and progress bar wrapped
/// in the fonts (at estimated_character_width a character without them)
/// and its footer, from least_notice_height to greatest_notice_height.
///
/// @param prompt the prompt
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return the height, in source pixels
[[nodiscard]] int32_t prompt_height(const Prompt& prompt, const DialogFonts* fonts = nullptr);

/// Returns the parts a prompt draws: its icon's place, its title, each line
/// of its text that its height does not cut, the failure, the progress bar
/// (prompt_bar_control) and its buttons, whose controls are their numbers.
///
/// @param prompt the prompt
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return the parts, in source pixels from the prompt's top left corner
[[nodiscard]] std::vector<LayoutPart>
prompt_layout(const Prompt& prompt, const DialogFonts* fonts = nullptr);

/// Tells whether every line of a prompt's text fits its height.
///
/// @param prompt the prompt
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return true when no line is cut
[[nodiscard]] bool prompt_fits(const Prompt& prompt, const DialogFonts* fonts = nullptr);

/// Moves the pointer: the button under it lights.
///
/// @param[in,out] prompt the prompt
/// @param x the pointer's column, in source pixels from the prompt's left edge
/// @param y the pointer's row, in source pixels from the prompt's top edge
/// @param height the prompt's height (prompt_height)
/// @param fonts the fonts it is drawn in, which place its buttons; null to estimate widths
/// @return a redraw when the look changed
[[nodiscard]] PromptAnswer prompt_pointer_move(
    Prompt& prompt, int32_t x, int32_t y, int32_t height, const DialogFonts* fonts = nullptr
);

/// Presses the pointer's button: a press on a button holds it.
///
/// @param[in,out] prompt the prompt
/// @param x the pointer's column
/// @param y the pointer's row
/// @param height the prompt's height
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return a redraw when a button is held
[[nodiscard]] PromptAnswer prompt_pointer_down(
    Prompt& prompt, int32_t x, int32_t y, int32_t height, const DialogFonts* fonts = nullptr
);

/// Presses with a finger: as prompt_pointer_down, but a press on no button
/// takes the nearest one within `reach`, pressed at its point nearest the
/// finger; the press's moves and its release are moved as far.
///
/// @param[in,out] prompt the prompt
/// @param x the finger's column
/// @param y the finger's row
/// @param height the prompt's height
/// @param reach how far a button may lie from the finger, in source pixels
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return a redraw when a button is held
[[nodiscard]] PromptAnswer prompt_finger_down(
    Prompt& prompt,
    int32_t x,
    int32_t y,
    int32_t height,
    int32_t reach,
    const DialogFonts* fonts = nullptr
);

/// Releases the pointer's button: a release over the button the press held
/// answers it.
///
/// @param[in,out] prompt the prompt
/// @param x the pointer's column
/// @param y the pointer's row
/// @param height the prompt's height
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return the answer, or a redraw
[[nodiscard]] PromptAnswer prompt_pointer_up(
    Prompt& prompt, int32_t x, int32_t y, int32_t height, const DialogFonts* fonts = nullptr
);

/// Takes a key: Enter and Space answer the marked button, Escape and N the
/// cancel button, Y the primary button; Left, Up and Shift+Tab mark the
/// button before, Right, Down and Tab the one after, round to the other end.
///
/// @param[in,out] prompt the prompt
/// @param key the key's meaning
/// @return the answer, a redraw, or nothing for a key it does not take
[[nodiscard]] PromptAnswer prompt_key(Prompt& prompt, DialogKey key);

/// Draws a prompt through the kit (kit::draw_question), in a notice's look:
/// its header with the Open Annihilation
/// icon (or the OA mark) and its title, its text, the failure in amber, the
/// progress bar, and its buttons right-aligned in the footer, an accent one
/// as OK looks and the others as Cancel, the marked one ringed in green.
///
/// @param[in,out] target the surface
/// @param placement where the prompt's top left corner lands, and its scale
/// @param prompt the prompt
/// @param fonts its fonts
/// @param icon the Open Annihilation icon; an empty picture draws the OA mark
void draw_prompt(
    oa::ui::frontend_renderer::Surface& target,
    const oa::ui::frontend_renderer::Placement& placement,
    const Prompt& prompt,
    const DialogFonts& fonts,
    const oa::ui::frontend_renderer::RgbaPicture& icon
);

/// Draws a prompt as draw_prompt does on a canvas of either form, laid out
/// in the canvas's game fonts.
///
/// @param canvas where the prompt's top left corner lands, its fonts and
///     its icon; without game fonts nothing is drawn
/// @param prompt the prompt
void draw_prompt(const oa::ui::kit::Canvas& canvas, const Prompt& prompt);

} // namespace oa::ui::engine_settings
