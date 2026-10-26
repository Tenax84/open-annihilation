#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Put a source for the Game files screen on an iOS simulator, for testing:
# a Total Annihilation folder, the demo's installer or a small test folder
# goes into the simulator's own storage (On My iPhone or On My iPad in the
# Files app and in the system's file picker), where the game's picker can
# choose it, or a game folder under another name goes into the game's own
# Documents folder, where "Copy it yourself" finds it. Folders and files are
# cloned (instant, no extra space on the same disk) or copied. The test
# folders are written by this script; no game data is stored in the
# repository.
set -euo pipefail

ios_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
repo_dir="$(cd -- "$ios_dir/../.." && pwd)"
bundle_id="${OA_IOS_BUNDLE_IDENTIFIER:-net.coreprime.open-annihilation}"
files_app_id="com.apple.DocumentsApp"
local_storage_group="group.com.apple.FileProvider.LocalStorage"
iphone_udid="45BA4005-FBF3-4E1B-AD67-315C19AC3ED9"
ipad_udid="9BE288E6-0FBE-4567-B4B8-D998340C1D59"
udid="${OA_IOS_SIMULATOR:-$iphone_udid}"
game_dir=""
demo_installer=""
fixture=""
into_documents=""
name=""
clear=0
remembered="$repo_dir/local/ios/game-data-source"

usage() {
    cat <<'USAGE'
Usage: platforms/ios/scripts/stage-files-source.sh [--udid UDID | --ipad]
           [--game-dir PATH] [--demo-installer FILE | --fixture KIND |
            --into-documents NAME] [--name NAME] [--clear]

Puts a source for the Game files screen on a simulator. Without
--demo-installer, --fixture or --into-documents it puts a Total Annihilation
folder into the simulator's own storage (On My iPhone or On My iPad), named
Total Annihilation, where CHOOSE FOLDER's picker finds it. An item already
there under the same name is replaced.

  --udid UDID            The simulator (default: OA_IOS_SIMULATOR, else the
                         iPhone 17)
  --ipad                 The iPad Air 11-inch (M3) simulator
  --game-dir PATH        The Total Annihilation folder on this Mac (default:
                         OA_GAME_DIR, else the folder push-game-data.sh copied
                         last, else the OA_GAME_DIR of a build tree here)
  --demo-installer FILE  Put the installer of the Total Annihilation demo
                         (1997) there instead, under its own name, for CHOOSE
                         INSTALLER
  --fixture KIND         Put a test folder there instead:
                           not-a-game  a folder of text files, "Not a game"
                           nested      the game folder two levels down,
                                       "Old PC/Games/Total Annihilation"
                           mod         a folder holding a usable mod profile,
                                       "Picker Test Mod", for ADD FILES
                           mutator     a mutator package file, "Picker Test
                                       Mutator.oamut", to tap in the Files
                                       app: the game asks INSTALL MUTATOR
  --into-documents NAME  Put the game folder into the game's own Documents
                         folder under NAME (a misnamed folder, for I HAVE
                         COPIED IT); the game must be installed
  --name NAME            The name of the item put there, instead of the
                         default above
  --clear                Remove what this script put on the simulator
                         before (alone: everything; with an item: that item
                         first)
  --help                 Show this help

The simulator's own storage is the "File Provider Storage" folder of the
group.com.apple.FileProvider.LocalStorage container that
  xcrun simctl get_app_container <UDID> com.apple.DocumentsApp groups
prints. What the script put there is listed in local/ios/staged-sources-<UDID>.
USAGE
}

require_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        printf 'stage-files-source.sh: %s requires a value\n' "$1" >&2
        exit 2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help) usage; exit 0 ;;
        --udid) require_value "$@"; udid="$2"; shift 2 ;;
        --ipad) udid="$ipad_udid"; shift ;;
        --game-dir) require_value "$@"; game_dir="$2"; shift 2 ;;
        --demo-installer) require_value "$@"; demo_installer="$2"; shift 2 ;;
        --fixture) require_value "$@"; fixture="$2"; shift 2 ;;
        --into-documents) require_value "$@"; into_documents="$2"; shift 2 ;;
        --name) require_value "$@"; name="$2"; shift 2 ;;
        --clear) clear=1; shift ;;
        *) printf 'stage-files-source.sh: unknown option %s (see --help)\n' "$1" >&2; exit 2 ;;
    esac
done

kinds=0
[[ -n "$demo_installer" ]] && kinds=$((kinds + 1))
[[ -n "$fixture" ]] && kinds=$((kinds + 1))
[[ -n "$into_documents" ]] && kinds=$((kinds + 1))
if [[ "$kinds" -gt 1 ]]; then
    printf 'stage-files-source.sh: choose one of --demo-installer, --fixture and --into-documents\n' >&2
    exit 2
fi
case "$fixture" in
    ''|not-a-game|nested|mod|mutator) ;;
    *) printf 'stage-files-source.sh: --fixture is not-a-game, nested, mod or mutator, not %s\n' "$fixture" >&2; exit 2 ;;
esac
case "$name" in
    */*|.|..) printf 'stage-files-source.sh: --name is one name, not a path: %s\n' "$name" >&2; exit 2 ;;
esac
case "$into_documents" in
    */*|.|..) printf 'stage-files-source.sh: --into-documents is one name, not a path: %s\n' "$into_documents" >&2; exit 2 ;;
esac
# Clearing alone stages nothing.
stage=1
if [[ "$clear" == 1 && "$kinds" == 0 && -z "$game_dir" && -z "$name" ]]; then
    stage=0
fi

if ! xcrun simctl list devices | grep -F "($udid)" | grep -qF '(Booted)'; then
    printf 'stage-files-source.sh: simulator %s is not running (xcrun simctl boot %s)\n' "$udid" "$udid" >&2
    exit 1
fi

# The simulator's own storage: On My iPhone or On My iPad.
storage_root() {
    local group
    group="$(xcrun simctl get_app_container "$udid" "$files_app_id" groups 2>/dev/null |
        awk -F '\t' -v id="$local_storage_group" '$1 == id { print $2; exit }')"
    if [[ -z "$group" || ! -d "$group" ]]; then
        printf 'stage-files-source.sh: simulator %s reports no %s container\n' "$udid" "$local_storage_group" >&2
        return 1
    fi
    printf '%s/File Provider Storage\n' "$group"
}

# The game's Documents folder; the game must be installed.
documents_root() {
    local container
    if ! container="$(xcrun simctl get_app_container "$udid" "$bundle_id" data 2>/dev/null)" || [[ -z "$container" ]]; then
        printf 'stage-files-source.sh: %s is not installed on simulator %s; run platforms/ios/run.sh --no-launch first\n' \
            "$bundle_id" "$udid" >&2
        return 1
    fi
    printf '%s/Documents\n' "$container"
}

# The OA_GAME_DIR a configured build tree of this checkout records, if any.
configured_game_dir() {
    local cache value
    for cache in "$repo_dir/build/CMakeCache.txt" "$repo_dir"/build-*/CMakeCache.txt; do
        [[ -f "$cache" ]] || continue
        value="$(sed -n 's/^OA_GAME_DIR:[A-Z]*=//p' "$cache" | head -n 1)"
        if [[ -n "$value" && -d "$value" ]]; then
            printf '%s\n' "$value"
            return 0
        fi
    done
    return 1
}

# The Total Annihilation folder on this Mac, as push-game-data.sh finds it.
find_game_dir() {
    local found="$game_dir"
    if [[ -z "$found" && -n "${OA_GAME_DIR:-}" ]]; then found="$OA_GAME_DIR"; fi
    if [[ -z "$found" && -f "$remembered" ]]; then found="$(head -n 1 "$remembered")"; fi
    if [[ -z "$found" ]]; then found="$(configured_game_dir || true)"; fi
    if [[ -z "$found" ]]; then
        printf 'stage-files-source.sh: name the Total Annihilation folder with --game-dir or OA_GAME_DIR\n' >&2
        return 1
    fi
    case "$found" in /*) ;; *) found="$PWD/$found" ;; esac
    found="${found%/}"
    if [[ ! -d "$found" ]] || ! find "$found" -maxdepth 1 -iname 'totala1.hpi' | grep -q .; then
        printf 'stage-files-source.sh: %s is not a folder that holds totala1.hpi\n' "$found" >&2
        return 1
    fi
    printf '%s\n' "$found"
}

# Clones source (a folder or a file) to target, or copies it where the disk
# cannot clone. Whatever was at target is replaced.
clone() {
    local source="$1" target="$2"
    rm -rf "$target"
    mkdir -p "$(dirname -- "$target")"
    if [[ -d "$source" ]]; then
        if ! cp -cR "$source" "$target" 2>/dev/null; then
            rm -rf "$target"
            ditto "$source" "$target"
        fi
    elif ! cp -c "$source" "$target" 2>/dev/null; then
        rm -f "$target"
        cp "$source" "$target"
    fi
}

manifest="$repo_dir/local/ios/staged-sources-$udid"

# Records an item in the manifest, once.
remember() {
    mkdir -p "$(dirname -- "$manifest")"
    touch "$manifest"
    if ! grep -qxF "$1" "$manifest"; then printf '%s\n' "$1" >> "$manifest"; fi
}

# Removes an item and forgets it.
forget() {
    local item="$1"
    rm -rf "$item"
    if [[ -f "$manifest" ]]; then
        grep -vxF "$item" "$manifest" > "$manifest.new" || true
        mv "$manifest.new" "$manifest"
    fi
}

if [[ "$clear" == 1 && "$stage" == 0 ]]; then
    count=0
    if [[ -f "$manifest" ]]; then
        while IFS= read -r item; do
            [[ -n "$item" ]] || continue
            if [[ -e "$item" ]]; then
                printf 'Removing %s\n' "$item"
                count=$((count + 1))
            fi
            rm -rf "$item"
        done < "$manifest"
        rm -f "$manifest"
    fi
    printf 'Removed %d staged item(s) from simulator %s\n' "$count" "$udid"
    exit 0
fi

start=$SECONDS
if [[ -n "$into_documents" ]]; then
    source_dir="$(find_game_dir)"
    documents="$(documents_root)"
    target="$documents/$into_documents"
    if [[ "$into_documents" == "Total Annihilation" ]]; then
        printf 'stage-files-source.sh: Documents/Total Annihilation is the game folder itself; use push-game-data.sh for it\n' >&2
        exit 2
    fi
    if [[ "$clear" == 1 ]]; then forget "$target"; fi
    printf 'Copying %s\n     to %s (the game'"'"'s Documents folder)\n' "$source_dir" "$target"
    clone "$source_dir" "$target"
    remember "$target"
    printf 'Done in %d s: Open Annihilation › %s\n' "$((SECONDS - start))" "$into_documents"
    exit 0
fi

root="$(storage_root)"
mkdir -p "$root"
place="On My iPhone"
if [[ "$udid" == "$ipad_udid" ]] || xcrun simctl list devices | grep -F "($udid)" | grep -q 'iPad'; then
    place="On My iPad"
fi

if [[ -n "$demo_installer" ]]; then
    case "$demo_installer" in /*) ;; *) demo_installer="$PWD/$demo_installer" ;; esac
    if [[ ! -f "$demo_installer" ]]; then
        printf 'stage-files-source.sh: the installer does not exist: %s\n' "$demo_installer" >&2
        exit 1
    fi
    item_name="${name:-$(basename -- "$demo_installer")}"
    target="$root/$item_name"
    if [[ "$clear" == 1 ]]; then forget "$target"; fi
    printf 'Copying %s\n     to %s\n' "$demo_installer" "$target"
    clone "$demo_installer" "$target"
elif [[ "$fixture" == not-a-game ]]; then
    item_name="${name:-Not a game}"
    target="$root/$item_name"
    if [[ "$clear" == 1 ]]; then forget "$target"; fi
    rm -rf "$target"
    mkdir -p "$target/Notes"
    printf 'A test folder of the Game files screen: it holds no Total Annihilation files.\n' > "$target/readme.txt"
    for n in 1 2 3; do
        printf 'Note %d of a folder that is not a game.\n' "$n" > "$target/Notes/note-$n.txt"
    done
    printf 'Wrote the test folder %s\n' "$target"
elif [[ "$fixture" == nested ]]; then
    source_dir="$(find_game_dir)"
    item_name="${name:-Old PC}"
    target="$root/$item_name"
    if [[ "$clear" == 1 ]]; then forget "$target"; fi
    rm -rf "$target"
    mkdir -p "$target/Documents"
    printf 'A test folder of the Game files screen: the game folder is in Games › Total Annihilation.\n' > "$target/readme.txt"
    printf 'A file beside the game folder.\n' > "$target/Documents/letter.txt"
    printf 'Copying %s\n     to %s\n' "$source_dir" "$target/Games/Total Annihilation"
    clone "$source_dir" "$target/Games/Total Annihilation"
elif [[ "$fixture" == mod ]]; then
    item_name="${name:-Picker Test Mod}"
    target="$root/$item_name"
    if [[ "$clear" == 1 ]]; then forget "$target"; fi
    rm -rf "$target"
    mkdir -p "$target"
    cat > "$target/oamod.yaml" <<'PROFILE'
# A test mod of the Game files screen: it changes nothing, so it plays as 3.1c.
oamod: 1
id: picker-test-mod
name: Picker Test Mod
version: "1.0"
author: {name: unknown}
packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}
PROFILE
    printf 'A test mod of the Game files screen. It changes nothing.\n' > "$target/readme.txt"
    printf 'Wrote the test mod %s\n' "$target"
elif [[ "$fixture" == mutator ]]; then
    item_name="${name:-Picker Test Mutator.oamut}"
    target="$root/$item_name"
    if [[ "$clear" == 1 ]]; then forget "$target"; fi
    rm -rf "$target"
    work="$(mktemp -d)"
    cat > "$work/oamut.yaml" <<'MANIFEST'
# A test mutator package: it changes nothing.
oamut: 1
id: picker-test-mutator
name: "Picker Test Mutator"
version: "1.0"
author:
  name: "Ada"
description: "A made-up mutator."
tags: ["gameplay"]
works-with: ["*"]
layers: ["files"]
entry: "mutator.lua"
MANIFEST
    printf 'return {}\n' > "$work/mutator.lua"
    printf 'A test mutator package. It changes nothing.\n' > "$work/README.md"
    (cd "$work" && zip -q -X "$target" oamut.yaml mutator.lua README.md)
    rm -rf "$work"
    printf 'Wrote the test mutator package %s\n' "$target"
else
    source_dir="$(find_game_dir)"
    item_name="${name:-Total Annihilation}"
    target="$root/$item_name"
    if [[ "$clear" == 1 ]]; then forget "$target"; fi
    printf 'Copying %s\n     to %s\n' "$source_dir" "$target"
    clone "$source_dir" "$target"
fi
remember "$target"
printf 'Done in %d s: %s › %s\n' "$((SECONDS - start))" "$place" "$item_name"
