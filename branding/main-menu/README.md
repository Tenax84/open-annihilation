# Main menu art

The art of Open Annihilation's main menu, which the game embeds
(`oa/app/main_menu_art.hpp`, `src/app/runtime_main_menu.cpp`). Every file
is an 8-bit RGBA PNG.

| File | Size | What it is |
|---|---|---|
| `background.png` | 1920 × 1080 | The battlefield behind the menu: a commander on the left, factories on the right, calm in the middle. A narrower window cuts it mostly on the right, so the commander stays in view. |
| `title.png` | 1920 × 488 | OPEN spaced out over ANNIHILATION, whose first and last letters stand taller, on a clear ground. |
| `button-normal.png` | 844 × 128 | A button's face at rest: a worn metal frame with a screw in each corner round a sunken, speckled plate. |
| `button-hover.png` | 844 × 128 | The face under the pointer or the focus: the plate lighter and its edge lit warm. |
| `button-pressed.png` | 844 × 128 | The face held down: the plate darker and sunk deeper. |
| `button-disabled.png` | 844 × 128 | The face of a button that does nothing now: grey and faded. |

The menu is laid out for a window 1080 rows high and scaled to every other
window: the title 960 columns wide, the buttons 422 by 64, which is why a
face is drawn at twice that size, and stretched to the button's size. The
captions are not part of the faces; the game writes them in the language
shown.

The background is kept at 1920 × 1080, and larger windows show it scaled
up.
