// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The progress dialog for a moment of work: a panel, a title, one line and a
// bar or a spinner, with no buttons. It is drawn over whatever is already on
// the window, only once the work has lasted a short while, and it closes on
// its own when the work is done and the line has been up long enough to read.
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string_view>

namespace oa::ui::frontend_renderer {
struct Surface;
}

namespace oa::ui::kit {
struct Frame;
}

namespace oa::ui::paint {
struct Canvas;
}

namespace oa::app {

class RendererHost;

/// How long work runs before the progress dialog appears, in milliseconds.
/// Quicker work shows nothing.
inline constexpr uint32_t long_work_show_after_ms = 300;

/// How long the progress dialog stays up once it has appeared, in
/// milliseconds, even when the work finishes first.
inline constexpr uint32_t long_work_stay_ms = 800;

/// How far a piece of work has come, written by the worker and read by the
/// thread that draws. `done` of -1 is a spinner. Otherwise `done` and
/// `whole` are counts the dialog scales to a full bar.
struct LongWork {
    std::atomic<int32_t> done{-1};    ///< how far it has come; -1 for a spinner
    std::atomic<int32_t> whole{1000}; ///< the whole amount; ignored while done is -1
};

/// What one run of the progress dialog did.
struct LongWorkResult {
    bool shown{false};        ///< the dialog was drawn
    int distinct_turns{0};    ///< spinner eighths that were drawn, when a spinner showed
    bool bar{false};          ///< a bar was drawn
    bool closed_itself{true}; ///< nothing the player did was asked to close it
    int32_t least_bar{-1};    ///< the least bar amount drawn, or -1 when no bar was
    int32_t most_bar{-1};     ///< the greatest bar amount drawn, or -1 when no bar was
};

/// Tells whether the progress dialog's work is already running on this thread's
/// behalf. A nested call runs its work inline and draws no second dialog.
///
/// @return true while run_long_work is drawing for a job
[[nodiscard]] bool long_work_running() noexcept;

/// Fills a canvas with the quiet launch colour, or with `under` stretched
/// over it when that picture has pixels.
///
/// @param canvas the canvas
/// @param under the picture behind the dialog; null, or empty, is the quiet colour
void fill_progress_behind(
    oa::ui::paint::Canvas& canvas, const oa::ui::frontend_renderer::Surface* under
);

/// Paints the progress dialog centred on a frame, over `under` or the quiet
/// colour, dimmed. A progress below 0 draws the spinner at `turn`; otherwise
/// the bar is that far along a full bar.
///
/// @param canvas the window's pixels
/// @param frame where the dialog is centred, and its size class
/// @param title the header, as shown
/// @param line the one line, as shown
/// @param progress -1 for the spinner, otherwise 0 to a full bar
/// @param turn the spinner's eighth
/// @param under the picture behind the dialog; null is the quiet colour
void paint_progress_frame(
    oa::ui::paint::Canvas& canvas,
    const oa::ui::kit::Frame& frame,
    std::string_view title,
    std::string_view line,
    int32_t progress,
    uint32_t turn,
    const oa::ui::frontend_renderer::Surface* under
);

/// Shows the progress dialog once, its spinner still, over the quiet colour,
/// and leaves it on the window until the next picture replaces it: for work
/// on the thread that draws, such as a soft restart's. The texts are shown
/// as given. With no window, nothing is drawn.
///
/// @param window the window; null draws nothing
/// @param renderer the window's renderer; null draws nothing
/// @param host told of the presented frame; null is left untold
/// @param title the header, as shown
/// @param line the one line, as shown
void show_work_frame(
    SDL_Window* window,
    SDL_Renderer* renderer,
    RendererHost* host,
    std::string_view title,
    std::string_view line
);

/// Runs `job` off the thread that draws, and shows the progress dialog while
/// it lasts longer than long_work_show_after_ms.
///
/// The dialog is the work dialog: no buttons, over `behind`, animating until
/// the job has finished and the dialog has been up for long_work_stay_ms.
/// The line and the title are English keys of the interface catalogue,
/// copied before the job starts and read again once it has finished. Input
/// is discarded, and a quit is put back for after the dialog has closed.
/// The dialog closes on its own.
///
/// With no window, or while a dialog is already running, `job` runs on the
/// caller and nothing is drawn. The job must not call back onto this thread
/// and wait. An exception from the job is thrown again after the dialog has
/// closed.
///
/// @param window the window; null runs the job inline
/// @param renderer the window's renderer; null runs the job inline
/// @param host told of each presented frame; null is left untold
/// @param title the header's English key
/// @param line the line's English key
/// @param behind paints the picture the dialog lies on; it runs on the drawing thread
/// @param job the work; it may store how far it has come
/// @return what the dialog did
[[nodiscard]] LongWorkResult run_long_work(
    SDL_Window* window,
    SDL_Renderer* renderer,
    RendererHost* host,
    std::string_view title,
    std::string_view line,
    const std::function<void(oa::ui::paint::Canvas&)>& behind,
    const std::function<void(LongWork&)>& job
);

} // namespace oa::app
