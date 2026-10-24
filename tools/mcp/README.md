# tools/mcp

An [MCP](https://modelcontextprotocol.io) server, `oa_mcp_server.py`, that
lets an MCP client (Claude Code, Claude Desktop or another) start the game,
watch it and drive it through the
[automation endpoint](../../docs/automation.md). The server speaks MCP over
its standard input and output and the automation protocol to the game. It
needs Python 3.8 or later and nothing beyond its standard library. It is a
program beside the game, not part of it: the game is unchanged, and the
endpoint is on only in a game the server starts with `--fark`, or one
started so by hand.

## Contents

- [Registering it](#registering-it)
- [The tools](#the-tools)
- [A session](#a-session)
- [Which game it starts](#which-game-it-starts)
- [What it never does](#what-it-never-does)
- [Checks](#checks)

## Registering it

Claude Code reads [`.mcp.json`](../../.mcp.json) at the repository's root,
which starts the server as `python3 tools/mcp/oa_mcp_server.py` from the
root, and asks once whether to use it. To register it by hand, or for
another folder:

```
claude mcp add open-annihilation -- python3 /path/to/open-annihilation/tools/mcp/oa_mcp_server.py
```

On Windows, where `python3` may be missing, use `python` or `py -3`
instead. Another client takes the same command in its own configuration,
such as `mcpServers` in Claude Desktop's `claude_desktop_config.json`.

## The tools

| Tool | What it does |
|---|---|
| `game_launch` | starts the game with `--fark` and connects; `headless` runs it on SDL's dummy drivers, with no window and no sound; `preferences_file` keeps the player's own preferences untouched; `arguments` adds options such as `--host` or `--join` |
| `game_attach` | connects to a game already started with `--fark --fark-file FILE` |
| `game_status` | whether a game is connected and running, its executable and the end of its log |
| `game_quit` | quits as the main menu's Exit does, leaving a match first; `force` kills the game |
| `screen` | the screen shown, the dialogs over it, the focused control, the pointer and the camera |
| `controls` | the controls that take the pointer, topmost first, with their names, kinds, places on the 640x480 canvas, state, text and a list's rows; `dialog` and `name_contains` filter them |
| `screenshot` | a PNG picture of the frame presented (or composed), whole or a region, shrunk by whole steps to at most `max_width` |
| `frame_hash` | the frame's FNV-1a hash and mean colour, to compare frames without a picture |
| `click_control` | clicks a control at its centre, by its name or its text; refused when another screen shows by the time the click arrives |
| `select_list_item` | chooses a list's row by its text or its key, scrolling the list with its arrows until the row shows |
| `click`, `drag`, `move_pointer`, `scroll` | the pointer and the wheel at a point of the canvas or the window |
| `press_key` | a key or a chord (`escape`, `return`, `f2`, `ctrl+s`), `repeat` times; `hold_frames` keeps it held, as for the arrow keys that scroll the camera |
| `type_text` | types into the field that takes text; `submit` presses Return after it |
| `send_input` | any of the endpoint's input events as they are |
| `wait_for` | waits until a screen, a dialog or a control shows, or with `gone` until it no longer does |
| `wait_frames` | lets the game run for a number of frames |
| `prefs`, `match`, `room` | the preferences, the running match (with `state_hash`, the digest a saved game carries) and the battle room |
| `subscribe`, `read_events` | chooses the kinds of event kept, and takes those that came |
| `request` | any automation request by its `op` and fields |

Every answer of the endpoint is passed on as JSON text, without the
request's `id`. A refusal is a tool error naming the endpoint's error code,
such as `screen_changed`.

## A session

A test asked of the client in words, such as "start the game headless,
open Skirmish and tell me which maps it lists", becomes calls like these:

1. `game_launch` with `headless` true, then `wait_for` `main_menu`;
2. `click_control` `SINGLE`, then `click_control` `Skirmish`, then
   `wait_for` `skirmish`;
3. `controls` with `name_contains` naming the map list, or `screenshot`
   to see the screen;
4. `game_quit`.

The controls' names are the ones the game's GUI files give, and
`oa.`-names for Open Annihilation's own screens
([docs/automation.md](../../docs/automation.md#oas-own-screens)).

## Which game it starts

`game_launch` starts the executable it is given, else the one
`OA_EXECUTABLE` names, else the newest `open-annihilation` under the
repository's `build*` folders; it answers the path and when that file was
built. It never builds: run [run.sh](../../run.sh), or build the
`oa-game` target, first, so that the game is the current source's. The installation
is `game_dir`, else `OA_GAME_DIR`; without either the game uses the folder
chosen before. `--skip-intro` is passed unless `skip_intro` is false.

The endpoint file and the game's output go to a folder of the server's own
in the system's temporary folder, which `game_status` names and which is
removed when the server ends. A game the server started is stopped when the
server ends; one it attached to is left running.

## What it never does

It never changes the game or what goes to other players: it sends only the
requests the endpoint answers, so everything
[the endpoint never does](../../docs/automation.md#what-it-never-does)
holds for it too. It reads no file but the endpoint file and the game's
log, and writes none but the endpoint's folder.

## Checks

| ctest | What it checks |
|---|---|
| `mcp-server-selftest` | `oa_mcp_server.py --self-test`: the MCP requests, and every kind of tool against a stand-in endpoint on the loopback address |
| `native-mcp-server` | `tools/check_native_mcp_server.py`: the server started as a client starts it drives the built game on the dummy drivers, from the main menu to Single Player and back, and quits it |
