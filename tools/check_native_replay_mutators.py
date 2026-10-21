#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check a recording carries the played loadout and playback uses it.

A loopback game with ridge-art-a installed records one mutators sector and a
content note. The same game without a mutator records neither. Playback with
the same folder applies that mutator. An empty folder, another package
SHA-256, and a sector that is not a loadout each refuse before the match.
A recording without the sector plays no mutators even when one is chosen.
"""
import argparse
import os
from pathlib import Path
import queue
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time

# A command that starts the executable under test; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Seconds a game may take to end. A 6000-tick loopback is the long one.
RUN_TIMEOUT = 600
# The package file named in ridge-art-a's origin record.
ART_SHA = "c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2"
TOUGH_SHA = "d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3"
DATE_SECTOR = 4
MUTATORS_SECTOR = 0x4F414D31
MISSING = (
    'This needs a mutator you do not have: "ridge-art-a" is not an installed mutator'
)
DIFFERENT = 'This needs a mutator you do not have: "ridge-art-a" is a different package'
UNREADABLE = "The mutators this was made with cannot be read."


class ReplayMutatorsFailure(Exception):
    """A recording or a playback that differs from the expected one."""


def origin_text(sha):
    """Returns a file origin record naming sha."""
    return (
        "# Written by Open Annihilation when it installed this folder; not part of the package.\n"
        "oa-origin: 1\n"
        "origin: file\n"
        f'sha256: "{sha}"\n'
        'installed: "2026-10-21"\n'
    )


def install(package, folder, sha):
    """Copies one package into folder and writes its origin record."""
    dest = folder / package.name
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(package, dest)
    (dest / ".oa-origin.yaml").write_text(origin_text(sha), encoding="utf-8")


class Game:
    """One running game, whose output the check keeps."""

    def __init__(self, native, game_dir, workdir, name, arguments):
        self.name = name
        self.lines = []
        self._queue = queue.Queue()
        self._ended = False
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        self.process = subprocess.Popen(
            [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
             "--preferences-file", str(workdir / f"{name}.conf"), *arguments],
            cwd=workdir, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self._queue.put(line.rstrip("\n"))
        self._queue.put(None)

    def _take(self, timeout):
        """Moves the next line into self.lines. Returns False once the output has ended."""
        if self._ended:
            return False
        try:
            line = self._queue.get(timeout=timeout)
        except queue.Empty:
            return True
        if line is None:
            self._ended = True
            return False
        self.lines.append(line)
        return True

    def wait_end(self, status):
        """Waits for the game to end; raises ReplayMutatorsFailure on another status."""
        try:
            got = self.process.wait(timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise ReplayMutatorsFailure(
                f"{self.name} did not end within {RUN_TIMEOUT} s") from None
        deadline = time.monotonic() + 5
        while self._take(0.2) and time.monotonic() < deadline:
            pass
        if got != status:
            raise ReplayMutatorsFailure(f"{self.name} exited with {got}, not {status}")

    def stop(self):
        """Ends the game if it still runs."""
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def output(self):
        """Returns what the game printed."""
        while self._take(0):
            pass
        return "\n".join(self.lines)


def run(native, game_dir, workdir, name, arguments, status):
    """Runs one game. Returns its output. Raises ReplayMutatorsFailure when it differs."""
    game = Game(native, game_dir, workdir, name, arguments)
    try:
        game.wait_end(status)
    except ReplayMutatorsFailure:
        game.stop()
        print(game.output())
        raise
    return game.output()


def chunks(data):
    """Yields each chunk's bytes, without its length word."""
    pos = 0
    while pos + 2 <= len(data):
        length = int.from_bytes(data[pos:pos + 2], "little")
        if length < 2 or pos + length > len(data):
            break
        yield data[pos + 2:pos + length]
        pos += length


def sectors(path):
    """Returns the recording's sectors as (type, data) pairs, in order."""
    found = list(chunks(path.read_bytes()))
    if len(found) < 2 or len(found[1]) < 4:
        raise ReplayMutatorsFailure(f"{path.name} has no sector count")
    count = int.from_bytes(found[1][:4], "little")
    bodies = found[2:2 + count]
    if len(bodies) != count:
        raise ReplayMutatorsFailure(f"{path.name} holds {len(bodies)} sectors, not {count}")
    parsed = []
    for body in bodies:
        if len(body) < 4:
            raise ReplayMutatorsFailure(f"{path.name} has a short sector")
        parsed.append((int.from_bytes(body[:4], "little"), body[4:]))
    return parsed


def sector_text(data):
    """Returns a sector's text, up to its first NUL."""
    end = data.find(b"\x00")
    if end >= 0:
        data = data[:end]
    return data.decode("utf-8", errors="replace")


def require_loadout_sector(path, sha):
    """Raises when the mutators sector is missing, repeated, or not the loadout."""
    parsed = sectors(path)
    types = [kind for kind, _ in parsed]
    if types.count(MUTATORS_SECTOR) != 1:
        raise ReplayMutatorsFailure(f"{path.name} holds {types.count(MUTATORS_SECTOR)} mutator sectors")
    at = types.index(MUTATORS_SECTOR)
    if at == 0 or types[at - 1] != DATE_SECTOR:
        raise ReplayMutatorsFailure(f"{path.name}'s mutators sector does not follow the date")
    text = sector_text(parsed[at][1])
    if f'"id":"ridge-art-a"' not in text or sha not in text or '"oaloadout":1' not in text:
        raise ReplayMutatorsFailure(f"{path.name}'s loadout is {text}")


def require_no_loadout_sector(path):
    """Raises when a recording holds a mutators sector."""
    types = [kind for kind, _ in sectors(path)]
    if MUTATORS_SECTOR in types:
        raise ReplayMutatorsFailure(f"{path.name} holds a mutators sector")


def note_text(path):
    """Returns the content note beside a recording."""
    note = Path(str(path) + ".oa-content")
    if not note.is_file():
        raise ReplayMutatorsFailure(f"{note.name} was not written")
    return note.read_text(encoding="utf-8", errors="replace")


def require_note(path, sha):
    """Raises when the note's mutator lines are not ridge-art-a's, after mod:."""
    lines = note_text(path).splitlines()
    try:
        at = lines.index("mod: ")
    except ValueError:
        at = next((index for index, line in enumerate(lines) if line.startswith("mod:")), None)
    if at is None:
        raise ReplayMutatorsFailure(f"{path.name}'s note has no mod line")
    following = lines[at + 1:at + 4]
    want = [
        f"mutator: {sha} ridge-art-a",
        "mutator-version: 1.0",
        "mutator-name: Ridge Art A",
    ]
    if following != want:
        raise ReplayMutatorsFailure(f"{path.name}'s note lists {following}")


def require_no_note_mutators(path):
    """Raises when the note lists a mutator."""
    if any(line.startswith("mutator") for line in note_text(path).splitlines()):
        raise ReplayMutatorsFailure(f"{path.name}'s note lists a mutator")


def require_output(text, present, absent, name):
    """Raises when a line is missing or one that must stay out is present."""
    for line in present:
        if line not in text:
            raise ReplayMutatorsFailure(f"{name} never printed: {line}")
    for line in absent:
        if line in text:
            raise ReplayMutatorsFailure(f"{name} printed: {line}")


def count_line(text, line):
    """Returns how many lines equal line."""
    return sum(1 for one in text.splitlines() if one == line)


def check(native, game_dir, art, tough, workdir):
    """Records and plays the loadout. Raises ReplayMutatorsFailure when one differs."""
    user = workdir / "user"
    install(art, user / "Mutators", ART_SHA)
    install(tough, user / "Mutators", TOUGH_SHA)
    recorded = workdir / "with.tad"
    plain = workdir / "without.tad"
    common = ["--headless-check", "--user-folder", str(user)]
    run(native, game_dir, workdir, "record", [
        *common, "--net-loopback-check", "6000", "--net-loopback-each-ai",
        "--net-record", str(recorded), "--mutator", "ridge-art-a",
    ], 0)
    require_loadout_sector(recorded, ART_SHA)
    require_note(recorded, ART_SHA)
    run(native, game_dir, workdir, "plain", [
        *common, "--net-loopback-check", "6000", "--net-loopback-each-ai",
        "--net-record", str(plain),
    ], 0)
    require_no_loadout_sector(plain)
    require_no_note_mutators(plain)

    def play(name, folder, recording, arguments, status):
        return run(native, game_dir, workdir, name, [
            "--headless-check", "--play-demo", str(recording), "--demo-unit-table", "strict",
            "--match-ticks", "300", "--user-folder", str(folder), *arguments,
        ], status)

    played = play("play", user, recorded, [], 0)
    if count_line(played, "demo mutators: 1 applied") != 1:
        raise ReplayMutatorsFailure("playback did not apply one mutator")
    applied = [line for line in played.splitlines() if "mutators: applied ridge-art-a" in line]
    if len(applied) != 1:
        raise ReplayMutatorsFailure("playback did not log ridge-art-a once")

    empty = workdir / "empty-user"
    empty.mkdir()
    missing = play("missing", empty, recorded, [], 1)
    require_output(missing, [MISSING], ["Offline match world prepared"], "missing")

    changed = workdir / "changed-user"
    shutil.copytree(user, changed)
    origin = changed / "Mutators" / "ridge-art-a" / ".oa-origin.yaml"
    origin.write_text(origin_text("1" * 64), encoding="utf-8")
    different = play("different", changed, recorded, [], 1)
    require_output(different, [DIFFERENT], [], "different")

    broken = workdir / "broken.tad"
    data = recorded.read_bytes()
    old = b'"oaloadout":1'
    if data.count(old) != 1:
        raise ReplayMutatorsFailure("the recording does not hold one oaloadout field")
    broken.write_bytes(data.replace(old, b'"oaloadout":2', 1))
    unread = play("unreadable", user, broken, [], 1)
    require_output(unread, [UNREADABLE], [], "unreadable")

    none = play("none", user, plain, ["--mutator", "ridge-tough"], 0)
    if count_line(none, "demo mutators: 0 applied") != 1:
        raise ReplayMutatorsFailure("a recording without the sector applied mutators")
    if any("mutators: applied" in line for line in none.splitlines()):
        raise ReplayMutatorsFailure("a recording without the sector logged an applied mutator")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--art", type=Path, required=True, help="the ridge-art-a package")
    parser.add_argument("--tough", type=Path, required=True, help="the ridge-tough package")
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-replay-mutators-",
                                     dir=args.scratch_root.resolve()) as scratch:
        try:
            check(args.native.resolve(), args.game_dir.resolve(), args.art.resolve(),
                  args.tough.resolve(), Path(scratch))
        except ReplayMutatorsFailure as failure:
            print(f"replay mutators check: {failure}", file=sys.stderr)
            return 1
    print("replay mutators check: a recording carries the loadout, playback applies it, "
          "and a missing package, another package, an unreadable sector and a recording "
          "without the sector do what they should")
    return 0


if __name__ == "__main__":
    sys.exit(main())
