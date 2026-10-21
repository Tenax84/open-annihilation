#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the battle room's mutator layer on two programs.

Each game runs as a program of its own through its main loop on the dummy SDL
drivers, over 127.0.0.1, at a DirectPlay port of the check's own
(--dplay-port). The host is started with --mutator on ridge-tough. A joiner
whose user folder lacks that package makes the host keep START waiting,
lacking at position 0. A joiner who has the package, laid out as an installed
folder with no origin record, builds the same layer: both programs log the
same loadout and layer digests, the same integrity line, and the host then
allows START.
"""
import argparse
import os
from pathlib import Path
import queue
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
ADDRESS = "127.0.0.1"
# Frames a joiner runs before it ends by itself: enough to join and exchange
# presence many times over.
JOIN_FRAMES = "900"
# Seconds a game may take to print a line the check waits for, or to end.
LINE_TIMEOUT = 120
BUILT = re.compile(
    r"open-annihilation: mutators: room built \d+ in \d+ ms "
    r"loadout ([0-9a-f]{16}) layer ([0-9a-f]{16})"
)
INTEGRITY = re.compile(r"open-annihilation: mutators: integrity ([0-9a-f]{64})")
PACKAGE_FILES = ("oamut.yaml", "mutator.lua")


class RoomMutatorsFailure(Exception):
    """A behaviour that differs from the expected one."""


def free_port():
    """Returns a UDP port of 127.0.0.1 that nothing holds now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((ADDRESS, 0))
        return probe.getsockname()[1]


class Game:
    """One running game, whose standard output the check reads line by line."""

    def __init__(self, native, game_dir, workdir, name, user_folder, arguments):
        self.name = name
        self.lines = []
        self._queue = queue.Queue()
        self._ended = False
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        self.preferences = workdir / f"{name}.conf"
        self.process = subprocess.Popen(
            [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--skip-intro",
             "--preferences-file", str(self.preferences), "--user-folder", str(user_folder),
             *arguments],
            cwd=workdir, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self._queue.put(line.rstrip("\n"))
        self._queue.put(None)

    def _take(self, timeout):
        """Moves the next line read into self.lines, waiting up to timeout seconds.

        Returns False once the output has ended.
        """
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

    def wait_for(self, expected):
        """Waits for a line of the game's output; raises when it never comes."""
        deadline = time.monotonic() + LINE_TIMEOUT
        while expected not in self.lines and time.monotonic() < deadline:
            if not self._take(0.5):
                break
        if expected not in self.lines:
            raise RoomMutatorsFailure(f"{self.name} never printed: {expected}")

    def wait_until(self, description, predicate):
        """Waits until predicate(self.lines) is true; raises when it never is."""
        deadline = time.monotonic() + LINE_TIMEOUT
        while time.monotonic() < deadline:
            if predicate(self.lines):
                return
            if not self._take(0.5):
                break
        raise RoomMutatorsFailure(f"{self.name} never reached: {description}")

    def wait_end(self):
        """Waits for the game to end by itself; raises unless it ends with status 0."""
        try:
            status = self.process.wait(timeout=LINE_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise RoomMutatorsFailure(f"{self.name} did not end within {LINE_TIMEOUT} s") from None
        while self._take(0):
            pass
        if status != 0:
            raise RoomMutatorsFailure(f"{self.name} exited with {status}")

    def stop(self):
        """Ends the game if it still runs."""
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def output(self):
        """Returns what the game printed, each line under its name."""
        while not self._queue.empty() and self._take(0):
            pass
        return "".join(f"{self.name}: {line}\n" for line in self.lines)


def last_match(lines, pattern):
    """Returns the last match of pattern in lines, or None."""
    found = None
    for line in lines:
        match = pattern.search(line)
        if match:
            found = match
    return found


def install_package(source, user_folder):
    """Copies ridge-tough's manifest and script into the user's Mutators folder.

    No origin record is copied: the folder is then the same kind of package a
    path on --mutator resolves, so the two digests can agree.
    """
    package = source / "tests" / "data" / "mutators" / "packages" / "ridge-tough"
    destination = user_folder / "Mutators" / "ridge-tough"
    destination.mkdir(parents=True, exist_ok=True)
    for name in PACKAGE_FILES:
        shutil.copyfile(package / name, destination / name)


def host_game(native, game_dir, workdir, name, user_folder, port, source):
    """Starts a host whose --mutator is the ridge-tough package folder."""
    package = source / "tests" / "data" / "mutators" / "packages" / "ridge-tough"
    user_folder.mkdir(parents=True, exist_ok=True)
    return Game(
        native, game_dir, workdir, name, user_folder,
        ["--dplay-port", str(port), "--host", "--player-name", "Hoster",
         "--game-name", "Open Room", "--mutator", str(package)])


def joiner_game(native, game_dir, workdir, name, user_folder, port):
    """Starts a joiner that ends by itself after JOIN_FRAMES frames."""
    user_folder.mkdir(parents=True, exist_ok=True)
    return Game(
        native, game_dir, workdir, name, user_folder,
        ["--dplay-port", str(port), "--join", ADDRESS, "--frames", JOIN_FRAMES,
         "--player-name", "Visitor"])


def check(native, game_dir, workdir, source):
    """Runs both joiners; raises RoomMutatorsFailure when a game differs."""
    games = []
    try:
        port = free_port()
        host = host_game(native, game_dir, workdir, "lacking-host", workdir / "lacking-host-user",
                         port, source)
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        host.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        lacking = joiner_game(native, game_dir, workdir, "lacking-joiner",
                              workdir / "lacking-joiner-user", port)
        games.append(lacking)
        lacking.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
        host.wait_until(
            "START held because the joiner lacks position 0",
            lambda lines: any(
                "mutators hold START: lacking" in line and "position 0" in line for line in lines
            ),
        )
        lacking.wait_end()
        host.stop()

        port = free_port()
        built_host_user = workdir / "built-host-user"
        built_joiner_user = workdir / "built-joiner-user"
        install_package(source, built_joiner_user)
        host = host_game(native, game_dir, workdir, "built-host", built_host_user, port, source)
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        host.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        visitor = joiner_game(native, game_dir, workdir, "built-joiner", built_joiner_user, port)
        games.append(visitor)
        visitor.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
        visitor.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        host.wait_until(
            "START allowed after the joiner built",
            lambda lines: (
                (mutator_lines := [line for line in lines if line.startswith("multiplayer: mutators ")])
                and mutator_lines[-1] == "multiplayer: mutators allow START"
            ),
        )
        host_built = last_match(host.lines, BUILT)
        guest_built = last_match(visitor.lines, BUILT)
        if host_built is None or guest_built is None or host_built.groups() != guest_built.groups():
            raise RoomMutatorsFailure(
                "the host and the joiner did not log the same loadout and layer digests"
            )
        visitor.wait_until("integrity", lambda lines: last_match(lines, INTEGRITY) is not None)
        host.wait_until("integrity", lambda lines: last_match(lines, INTEGRITY) is not None)
        host_integrity = last_match(host.lines, INTEGRITY)
        guest_integrity = last_match(visitor.lines, INTEGRITY)
        if host_integrity is None or guest_integrity is None or host_integrity.group(1) != guest_integrity.group(1):
            raise RoomMutatorsFailure("the host and the joiner did not log the same integrity digest")
        visitor.wait_end()
        host.stop()
    except RoomMutatorsFailure:
        for game in games:
            game.stop()
            print(game.output(), end="")
        raise
    finally:
        for game in games:
            game.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True, help="the engine source tree")
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-net-room-mutators-",
                                     dir=args.scratch_root.resolve()) as scratch:
        try:
            check(args.native.resolve(), args.game_dir.resolve(), Path(scratch),
                  args.source.resolve())
        except RoomMutatorsFailure as failure:
            print(f"room mutators check: {failure}", file=sys.stderr)
            return 1
    print("room mutators check: a joiner without ridge-tough held START at position 0, and a "
          "joiner with it built the same loadout and layer; the host then allowed START and "
          "both integrity lines matched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
