#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check --mutator on a headless skirmish.

The game plays two commanders. With no mutator their health is neither 4000
nor 1234 nor 2000. ridge-tough sets both to 4000, and ridge-tough:health=2000
sets both to 2000, the number the option writes (1234 is not on that
option's step). ridge-tough then ridge-tougher leaves 5000, and the reverse
order leaves 4000. A missing id, an option the package does not have, a
mutator listed twice, a prerequisite that is not above it and two mutators
that conflict end with status 2, one refusal line and no match.
ridge-commander-boom ends with a non-zero status, a line naming the mutator
and its error, and no match. --no-mutators plays the stock health and
logs the refusal once. A match with mutators leaves no crash record and
writes no remembered set. A record planted beside the preferences is
logged once and deleted. A campaign start that the mutators refuse leaves
no crash record.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
PREPARED = "Offline match world prepared"
UNIT_LINE = re.compile(r"^tick=(\d+) slot=\d+ type=\d+ owner=(\d+) .* health=(-?\d+)")
RUN_TIMEOUT = 300
# Canonical loadout text for ridge-tough 1.0. The crash record stores this
# text; the next start names the id and the version and does not resolve it.
TOUGH_LOADOUT = (
    '{"mutators":[{"folder":false,"id":"ridge-tough","options":{},'
    '"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",'
    '"version":"1.0"}],"oaloadout":1}'
)


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def run(native, game_dir, workdir, name, mutators, extra=()):
    """Runs one headless start. Returns the status and the output lines."""
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    digest = workdir / f"{name}.trace"
    units = workdir / f"{name}.units"
    command = [
        *RUNNER,
        str(native),
        "--game-dir",
        str(game_dir),
        "--skip-intro",
        "--mute",
        "--headless-check",
        "--match-ticks",
        "2",
        "--seed",
        "1",
        "--trace-digest",
        str(digest),
        "--trace-units",
        str(units),
        "--preferences-file",
        str(workdir / f"{name}.conf"),
    ]
    command.extend(extra)
    for value in mutators:
        command += ["--mutator", value]
    result = subprocess.run(
        command,
        cwd=workdir,
        env=environment,
        timeout=RUN_TIMEOUT,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    output = result.stdout + result.stderr
    return result.returncode, output.splitlines(), units


def commanders(units_path, lines):
    """The two commanders' health at the earliest traced tick."""
    if not units_path.is_file():
        raise ScriptFailure("the units trace was not written")
    text = units_path.read_text(encoding="utf-8", errors="replace")
    found = []
    earliest = None
    for line in text.splitlines():
        match = UNIT_LINE.match(line)
        if match is None:
            continue
        tick = int(match.group(1))
        if earliest is None or tick < earliest:
            earliest = tick
            found = [(int(match.group(2)), int(match.group(3)))]
        elif tick == earliest:
            found.append((int(match.group(2)), int(match.group(3))))
    if len(found) != 2:
        print("\n".join(lines))
        raise ScriptFailure(f"the first tick has {len(found)} units, not two commanders")
    owners = {owner for owner, _health in found}
    if len(owners) != 2:
        raise ScriptFailure("the two commanders do not belong to two players")
    health = sorted(health for _owner, health in found)
    return health[0], health[1]


def expect_health(native, game_dir, workdir, name, mutators, health):
    """A skirmish whose two commanders hold one health."""
    status, lines, units = run(native, game_dir, workdir, name, mutators)
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"{name} exited with {status}")
    if not any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure(f"{name} prepared no match")
    first, second = commanders(units, lines)
    if first != health or second != health:
        raise ScriptFailure(f"{name} left the commanders at {first} and {second}, not {health}")


def expect_stock(native, game_dir, workdir):
    """A skirmish with no mutator, at the commanders' own health."""
    status, lines, units = run(native, game_dir, workdir, "stock", [])
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"the stock skirmish exited with {status}")
    if not any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure("the stock skirmish prepared no match")
    if any("mutators:" in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure("a skirmish with no mutator logged mutators")
    first, second = commanders(units, lines)
    for health in (first, second):
        if health in (4000, 1234, 2000):
            raise ScriptFailure(f"the stock commanders hold {health}")
    return first, second


def separate(workdir, name):
    """A folder of one run, so its crash record is not another run's."""
    folder = workdir / name
    folder.mkdir()
    return folder


def assert_no_sentinel(folder):
    """The crash record is not left beside this run's preferences."""
    sentinel = folder / "mutators-sentinel.conf"
    if sentinel.exists():
        raise ScriptFailure(f"{sentinel} was left")


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def expect_no_mutators(native, game_dir, workdir, package, health):
    """--no-mutators plays the stock health and leaves no crash record."""
    folder = separate(workdir, "no-mutators")
    status, lines, units = run(
        native, game_dir, folder, "run", [package], extra=("--no-mutators",)
    )
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"--no-mutators exited with {status}")
    if not any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure("--no-mutators prepared no match")
    first, second = commanders(units, lines)
    if (first, second) != health:
        raise ScriptFailure(
            f"--no-mutators left the commanders at {first} and {second}, not {health[0]} and {health[1]}"
        )
    said = [line for line in lines if "--no-mutators refused" in line]
    if len(said) != 1:
        print("\n".join(lines))
        raise ScriptFailure(f"--no-mutators reported the refusal {len(said)} times, not once")
    assert_no_sentinel(folder)


def expect_no_remembered_set(folder, name):
    """A match with mutators left no crash record and wrote no remembered set."""
    assert_no_sentinel(folder)
    preferences = folder / f"{name}.conf"
    if not preferences.is_file():
        return
    text = preferences.read_text(encoding="utf-8", errors="replace")
    if "open-annihilation.mutators." in text:
        raise ScriptFailure(f"{preferences.name} holds a remembered set")


def expect_planted_record(native, game_dir, workdir):
    """A record planted before a stock run is logged once and deleted."""
    folder = separate(workdir, "planted")
    sentinel = folder / "mutators-sentinel.conf"
    sentinel.write_text(
        "open-annihilation-preferences 1\n"
        + quote_preference("mutators")
        + " "
        + quote_preference(TOUGH_LOADOUT)
        + "\n"
        + quote_preference("setup")
        + " "
        + quote_preference("none")
        + "\n",
        encoding="utf-8",
    )
    status, lines, _units = run(native, game_dir, folder, "run", [])
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"the planted record's run exited with {status}")
    phrase = "the last game ended in a crash with ridge-tough 1.0"
    said = [line for line in lines if phrase in line]
    if len(said) != 1:
        print("\n".join(lines))
        raise ScriptFailure(f"the planted record was reported {len(said)} times, not once")
    assert_no_sentinel(folder)


def expect_campaign_refusal(native, game_dir, workdir, package):
    """A campaign start the mutators refuse leaves no crash record."""
    folder = separate(workdir, "campaign-boom")
    status, lines, _units = run(
        native,
        game_dir,
        folder,
        "run",
        [str(package / "ridge-commander-boom")],
        extra=("--campaign", "Arm Campaign", "--mission", "1"),
    )
    if status == 0:
        print("\n".join(lines))
        raise ScriptFailure("the campaign refusal exited with status 0")
    assert_no_sentinel(folder)


def expect_refused(native, game_dir, workdir, name, mutators, phrase, status_code):
    """A start that is refused and prepares no match."""
    status, lines, _units = run(native, game_dir, workdir, name, mutators)
    if status != status_code:
        print("\n".join(lines))
        raise ScriptFailure(f"{name} exited with {status}, not {status_code}")
    if any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure(f"{name} prepared a match")
    said = [line for line in lines if phrase in line]
    if len(said) != 1:
        print("\n".join(lines))
        raise ScriptFailure(f"{name} reported the refusal {len(said)} times, not once")


def expect_boom(native, game_dir, workdir, package):
    """A mutator whose script stops after a write, with no match prepared."""
    mutator_id = "ridge-commander-boom"
    status, lines, _units = run(native, game_dir, workdir, "boom", [str(package / mutator_id)])
    if status == 0:
        print("\n".join(lines))
        raise ScriptFailure(f"{mutator_id} exited with status 0")
    if any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure(f"{mutator_id} prepared a match")
    named = False
    for line in lines:
        at = line.find(mutator_id)
        if at >= 0 and "boom" in line[at + len(mutator_id):]:
            named = True
    if not named:
        print("\n".join(lines))
        raise ScriptFailure(f"{mutator_id} was not named with its error")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--packages", type=Path, required=True, help="tests/data/mutators/packages")
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    packages = args.packages.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    tough = str(packages / "ridge-tough")
    tougher = str(packages / "ridge-tougher")
    with tempfile.TemporaryDirectory(prefix="native-mutators-", dir=args.scratch_root.resolve()) as scratch:
        workdir = Path(scratch)
        try:
            health = expect_stock(native, game_dir, workdir)
            expect_health(native, game_dir, workdir, "tough", [tough], 4000)
            expect_no_remembered_set(workdir, "tough")
            expect_health(native, game_dir, workdir, "option", [tough + ":health=2000"], 2000)
            expect_health(native, game_dir, workdir, "order", [tough, tougher], 5000)
            expect_health(native, game_dir, workdir, "reverse", [tougher, tough], 4000)
            expect_refused(
                native,
                game_dir,
                workdir,
                "missing",
                ["ridge-nothere"],
                "is not an installed mutator",
                2,
            )
            expect_refused(
                native,
                game_dir,
                workdir,
                "speed",
                [tough + ":speed=2"],
                "has no option speed",
                2,
            )
            expect_refused(
                native,
                game_dir,
                workdir,
                "twice",
                [tough, tough],
                "is listed twice",
                2,
            )
            expect_refused(
                native,
                game_dir,
                workdir,
                "needs",
                [str(packages / "ridge-needs-swift")],
                "needs ridge-swift >= 1.0 above it",
                2,
            )
            expect_refused(
                native,
                game_dir,
                workdir,
                "conflict",
                [str(packages / "ridge-swift"), str(packages / "ridge-against-swift")],
                "conflicts with ridge-swift",
                2,
            )
            expect_boom(native, game_dir, workdir, packages)
            expect_no_mutators(native, game_dir, workdir, tough, health)
            expect_planted_record(native, game_dir, workdir)
            expect_campaign_refusal(native, game_dir, workdir, packages)
        except ScriptFailure as failure:
            print(f"FAIL {failure}")
            return 1
    print("mutators command line check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
