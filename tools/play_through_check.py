#!/usr/bin/env python3
"""play_through_check.py - the whole game, from the front door to the records.

**Everything else checks a piece.** The unit tests drive the menu module, the
render tests draw its screens, `play_check.py` races the real client and watches
the race. What nothing did was walk the thing a person actually does, end to
end, in the real binary: arrive at the door, get to a race, drive it, and then
find the time afterwards on the screen that keeps times.

That last step is the one worth having. A race that runs and a records screen
that draws are both fine on their own while the time between them goes nowhere -
and every part of that path has broken at least once: the store refused every
write for weeks and blamed the disk, and the conditions a record is filed under
were wrong so a lap could never be found again.

Every step is run against the real client with its preferences pointed at a
throwaway, and every step's evidence is a `--trace` line, which reports the
screen and what is on it.
"""

import os
import subprocess
import sys
import tempfile

# A session drives itself and stops on the results; it does not exit. Long
# enough for two laps of a stock track with the AI driving, and generous
# because CI machines are slow.
RACE_SECONDS = 90

# A screen only has to come up and say so.
SCREEN_SECONDS = 8


def run(game, args, seconds, store):
    """The client, headless and silent, pointed at a throwaway store."""
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_RENDER_DRIVER"] = "software"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["GEARSTICK_PREF_DIR"] = store
    # The store is the point of this check, so it is the one thing that must
    # not land in the player's own directory.
    env["XDG_DATA_HOME"] = store
    env["HOME"] = store
    env["APPDATA"] = store

    p = subprocess.Popen([game] + args, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, env=env)
    try:
        out, _ = p.communicate(timeout=seconds)
    except subprocess.TimeoutExpired:
        # **Killed rather than asked**, the same way every other check here
        # stops a client: terminate is a signal where signals exist and
        # TerminateProcess where they do not, and a harness that behaves
        # differently on one platform is how a check passes for a reason it
        # does not have everywhere.
        p.kill()
        out, _ = p.communicate()
    return out.decode("utf-8", "replace")


def traces(text):
    """Every trace line, as a list of {screen, tick, drivers, tracks, records}."""
    rows = []
    for line in text.splitlines():
        if "trace screen=" not in line:
            continue
        row = {}
        for part in line.split("trace ", 1)[1].split():
            if "=" in part:
                key, value = part.split("=", 1)
                row[key] = value
        rows.append(row)
    return rows


def fail(what, text):
    print(f"play_through_check: {what}\n" + text[-2000:])
    return 1


def main():
    if len(sys.argv) < 2:
        print("usage: play_through_check.py <gearstick>")
        return 2
    game = sys.argv[1]

    with tempfile.TemporaryDirectory() as store:
        # --- the door, and every room behind it ---------------------------
        #
        # Each screen the client can be told to open, opened. This is the half
        # nothing covered: the render tests draw these screens from the menu
        # module, and the module is not the binary.
        for screen in ("login", "title", "drivers", "setup", "tracks"):
            text = run(game, ["--screen", screen, "--trace"],
                       SCREEN_SECONDS, store)
            rows = traces(text)
            if not rows:
                return fail(f"asked for the {screen} screen and the client "
                            "never said what it was showing", text)
            if rows[0].get("screen") != screen:
                return fail(f"asked for the {screen} screen and got "
                            f"{rows[0].get('screen')!r}", text)
            print(f"play_through_check: {screen} opens")

        # --- a race, driven by the machine, kept ---------------------------
        text = run(game, ["--session", "--autodrive", "--trace", "--keep",
                          "--watch-check"],
                   RACE_SECONDS, store)
        rows = traces(text)
        if "session:" not in text:
            return fail("a session was asked for and never finished a race",
                        text)
        if not rows or rows[-1].get("screen") != "results":
            return fail("the race did not end on the results screen", text)
        # **Watched back from every seat, the same race.** The viewer plays the
        # recording into the simulation with one view following a car; the
        # world every watch ends in has to be the same world, or the viewer is
        # an input to the race.
        watched = [l for l in text.splitlines() if "watch: from car" in l]
        hashes = {l.split("hash ")[1].split()[0] for l in watched if "hash " in l}
        if len(watched) < 2 or len(hashes) != 1:
            return fail("the race watched back from each car did not agree: "
                        f"{len(watched)} watches, {len(hashes)} different worlds", text)
        print(f"play_through_check: watched back from {len(watched)} cars, one world")

        laps = rows[-1].get("tick", "0")
        print(f"play_through_check: a race ran to the results at tick {laps}")

        # **And it left a time behind.** The step everything else stops short
        # of: the race is over, the results are up, and something has to be in
        # the records or the whole path led nowhere.
        if int(rows[-1].get("records", "0")) < 1:
            return fail("the race finished and set no record, so the time "
                        "went nowhere", text)

        # --- and the time is there on a fresh run --------------------------
        #
        # A different process, reading the store off the disk. This is what
        # makes it a *path* rather than one program's memory: a record that
        # exists only until the game is closed is not a record.
        text = run(game, ["--screen", "records", "--trace"],
                   SCREEN_SECONDS, store)
        rows = traces(text)
        if not rows:
            return fail("the records screen never said what it was showing",
                        text)
        kept = int(rows[0].get("records", "0"))
        if kept < 1:
            return fail("the time set in the race was not there when the game "
                        "was opened again", text)

        drivers = int(rows[0].get("drivers", "0"))
        tracks = int(rows[0].get("tracks", "0"))
        print(f"play_through_check: opened again, {kept} record(s), "
              f"{drivers} driver(s) and {tracks} track(s) still there")

    print("play_through_check: the door leads to a race and the race leads to "
          "a time somebody can find again, correct")
    return 0


if __name__ == "__main__":
    sys.exit(main())
