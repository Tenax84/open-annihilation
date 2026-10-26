// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A prompt of Open Annihilation's own: the kit's question under its old
// names, and the parts it draws as the dialog's layout lists them.

#include "oa/ui/engine_settings/prompt.hpp"

#include "oa/ui/kit/components_more.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace oa::ui::engine_settings {

namespace kit = oa::ui::kit;

int32_t prompt_height(const Prompt& prompt, const DialogFonts* fonts) {
    return kit::place_question(prompt, fonts).height;
}

bool prompt_fits(const Prompt& prompt, const DialogFonts* fonts) {
    return !kit::place_question(prompt, fonts).cut;
}

std::vector<LayoutPart> prompt_layout(const Prompt& prompt, const DialogFonts* fonts) {
    const kit::PlacedQuestion placed = kit::place_question(prompt, fonts);
    std::vector<LayoutPart> parts;
    parts.push_back(LayoutPart{kit::notice_mark, {}, DialogFont::regular, 0, no_control});
    parts.push_back(
        LayoutPart{
            placed.title,
            prompt.title,
            DialogFont::regular,
            kit::compact_metrics.heading_tracking,
            no_control
        }
    );
    for (const auto& line : placed.lines)
        parts.push_back(
            LayoutPart{
                line.rect,
                line.text,
                line.path ? DialogFont::regular : DialogFont::small,
                0,
                no_control
            }
        );
    if (placed.bar.width > 0)
        parts.push_back(LayoutPart{placed.bar, {}, DialogFont::small, 0, prompt_bar_control});
    for (std::size_t index = 0; index < placed.buttons.size(); ++index)
        parts.push_back(
            LayoutPart{
                placed.buttons[index],
                prompt.buttons[index].caption,
                DialogFont::small,
                0,
                static_cast<int32_t>(index)
            }
        );
    return parts;
}

PromptAnswer prompt_pointer_move(
    Prompt& prompt, int32_t x, int32_t y, int32_t height, const DialogFonts* fonts
) {
    return kit::question_pointer_move(prompt, {x, y}, height, fonts);
}

PromptAnswer prompt_pointer_down(
    Prompt& prompt, int32_t x, int32_t y, int32_t height, const DialogFonts* fonts
) {
    return kit::question_pointer_down(prompt, {x, y}, height, fonts);
}

PromptAnswer prompt_finger_down(
    Prompt& prompt, int32_t x, int32_t y, int32_t height, int32_t reach, const DialogFonts* fonts
) {
    return kit::question_finger_down(prompt, {x, y}, height, reach, fonts);
}

PromptAnswer
prompt_pointer_up(Prompt& prompt, int32_t x, int32_t y, int32_t height, const DialogFonts* fonts) {
    return kit::question_pointer_up(prompt, {x, y}, height, fonts);
}

PromptAnswer prompt_key(Prompt& prompt, DialogKey key) {
    return kit::question_key(prompt, key);
}

void draw_prompt(
    oa::ui::frontend_renderer::Surface& target,
    const oa::ui::frontend_renderer::Placement& placement,
    const Prompt& prompt,
    const DialogFonts& fonts,
    const oa::ui::frontend_renderer::RgbaPicture& icon
) {
    kit::draw_question({&target, placement, &fonts, icon}, prompt);
}

void draw_prompt(const kit::Canvas& canvas, const Prompt& prompt) {
    if (canvas.fonts == nullptr)
        return;
    kit::draw_question(canvas, prompt);
}

} // namespace oa::ui::engine_settings
