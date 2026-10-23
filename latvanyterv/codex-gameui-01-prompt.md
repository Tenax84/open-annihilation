# Codex prompt: játék közbeni kezelőfelület kijelölő gombokkal (gameui-01.png)

Mintakép: latvanyterv/gameui-jelenlegi.png (skirmish, a Commander kijelölve, 1024x768).

A gombok a motor meglévő gyorsbillentyűit adják egérrel (src/app/runtime_hotkeys.cpp,
src/app/runtime_selection_shortcuts.cpp, docs/mods/standard-hacks/ui.selection-shortcuts.md):

- Ctrl+1..9: a kijelölés csoportba tétele; Alt+1..9: a csoport kijelölése (Shift-tel hozzáadja);
- Ctrl+A: minden egység; Ctrl+C: a Commander (és követése);
- Ctrl+V: minden repülő; Ctrl+B: minden építő jármű; Ctrl+F: minden gyár;
- Ctrl+S: a képernyőn látható egységek (a ui.selection-shortcuts hackkel: a képernyőn
  látható mozgó harci egységek, Ctrl+B / Ctrl+F pedig a tétlen építőn / gyáron lép végig);
- Ctrl+Z: a kijelöltekkel azonos típusú egységek az egész térképen;
- dupla kattintás egy egységen: azonos típusúak a képernyőn.

## Prompt

Use case: ui-mockup
Asset type: in-game HUD design concept, 4:3 (1024x768), full-screen game frame, no browser or OS chrome.

Primary request: redesign the in-battle interface of OPEN ANNIHILATION, a remake of the 1997 real-time strategy game Total Annihilation. The attached screenshot is the current in-game screen: keep its layout and content (left sidebar with minimap at the top, ORDERS and BUILD tabs, a 2x3 grid of build pictures with page arrows, MOVE / STOP / GUARD / PATROL / ATTACK / D-GUN order buttons; top bar with METAL and ENERGY meters; bottom info strip; the battlefield seen from above in the middle) but restyle the chrome in our menu style: heavy bolted gunmetal frames, engraved header plates, dark graphite recessed insides, beveled steel buttons with light engraved captions, amber glow for the active or hovered button, cold steel gray for the rest. The battlefield in the middle stays a top-down view of sandy terrain with green shrubs, a few rocks and the black unexplored area, with a small group of units: the blue-yellow Commander robot with a green selection box and a few tanks and aircraft near it.

New element: a SELECTION BAR, a slim bolted steel strip along the bottom edge of the battlefield, directly above the bottom info strip, spanning the battlefield's width. It holds two groups of small square/rectangular buttons, each with its keyboard shortcut engraved small in its bottom corner:
- Left group, header plate "GROUPS": nine numbered square buttons "1" to "9". Groups that hold units show the count below the number in small text and a tiny unit silhouette; empty groups are dim. Group "1" is lit amber (the selected group) and shows "12"; "2" shows "4"; "3" shows "6"; the others are dim and empty. Hint text under the group: "Ctrl+# store  Alt+# select".
- Right group, header plate "SELECT": icon buttons with a small pictogram and a short caption:
  - "ALL" (pictogram: four unit dots), shortcut "Ctrl+A"
  - "COMMANDER" (pictogram: commander helmet/star), shortcut "Ctrl+C"
  - "AIR" (pictogram: aircraft), shortcut "Ctrl+V"
  - "COMBAT" (pictogram: tank), shortcut "Ctrl+S"
  - "BUILDERS" (pictogram: construction vehicle with crane), shortcut "Ctrl+B"
  - "FACTORIES" (pictogram: factory building), shortcut "Ctrl+F"
  - "ON SCREEN" (pictogram: dashed selection rectangle), shortcut "Ctrl+S"
  - "SAME TYPE" (pictogram: two identical units with an equals sign), shortcut "Ctrl+Z"

The sidebar, the order buttons and the resource bar keep their places; only their look changes to our steel and amber style. The bottom info strip shows the selected unit: "Arm Commander" with a green health bar and "3000/3000".

Text, spelled exactly: "METAL", "ENERGY", "1000", "+1.0", "-0.0", "+25", "-0", "ORDERS", "BUILD", "MOVE", "STOP", "GUARD", "PATROL", "ATTACK", "D-GUN", "GROUPS", "1", "2", "3", "4", "5", "6", "7", "8", "9", "12", "4", "6", "Ctrl+# store  Alt+# select", "SELECT", "ALL", "Ctrl+A", "COMMANDER", "Ctrl+C", "AIR", "Ctrl+V", "COMBAT", "Ctrl+S", "BUILDERS", "Ctrl+B", "FACTORIES", "Ctrl+F", "ON SCREEN", "SAME TYPE", "Ctrl+Z", "Arm Commander", "3000/3000".

Colors: label text #ECE6C6, hover text #FFFADE, disabled text #969890, dark text outline #121410; gunmetal steel frames; amber glow for the active button; dark graphite fields; METAL in blue-white and ENERGY in yellow as in the screenshot. No green in the chrome (green only in the terrain, the selection box and the health bar).

Constraints: the battlefield stays the largest area of the screen; the selection bar is compact (about 6% of the screen height) and does not cover the units; all text in English, crisp and readable; no watermark; no real Total Annihilation logos; a clean, readable UI layout mockup, not a painting. Style: gritty 1990s military sci-fi, metal, readable.

Text check: spell every label exactly as listed, plain letters, no ligatures, no special characters.
