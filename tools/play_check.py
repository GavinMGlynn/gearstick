#!/usr/bin/env python3
"""play_check.py - the game plays itself, and says what it is showing.

**Everything that has gone wrong in front of a player went wrong after the green
flag.** A camera pointed somewhere else, controls that did nothing, a race that
froze two seconds in, a wrecked car with no way off the screen. None of it is
reachable from a unit test - it is a property of what ended up on the window -
and none of it was reachable from the front door check either, which proves a
client gets *into* a race and then stops watching.

So the client can now drive itself and report itself:

  --autodrive   the AI takes this machine's car through the ordinary loop,
                input, network, camera and all
  --trace       a line a second saying what is on screen, in key=value

and this races it twice - once on this machine, once against a real server - and
makes assertions out of the lines. The rules are the ones a person checks in the
first five seconds without noticing they are checking anything:

  - a race actually starts
  - it starts held on the line, and the lights go green
  - the clock advances, and keeps advancing
  - the car this machine drives is on this machine's screen, every time
  - once it is let go it moves, and gets somewhere
  - nothing stalls

Everything after the flag is judged on ticks of racing rather than on seconds
of wall clock, so the check is the same check on a machine that runs the game
at full speed and on one with the sanitisers on.

Every one of those was false at some point today, on a build whose tests were
all green.
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

# The rate the simulation runs at, which is what the trace counts in. This is
# GS_TICK_HZ from src/core/gs_sim.h; the check below states the number it saw
# rather than only asserting on it, so the two drifting apart is visible.
TICK_HZ = 120

# **How much of a race to watch, measured from the green flag and in ticks.**
#
# Every rule here is a property of a race in motion, and a race does not begin
# in motion: the simulation holds every car on the line for GS_COUNTDOWN_TICKS
# - ten seconds - so everybody gets the same moment to react to. A window
# measured from the start of the run is therefore mostly countdown, and this
# one was. It gave a race fourteen seconds, ten of which were the lights, and
# then judged the controls on the second and a half of driving that was left.
# On a sanitised build, where getting into an online race costs another nine
# seconds of lobby and a track handed over in chunks, what was left was a car
# that had been allowed to drive for a moment - and the check called that
# "nowhere" and went red, on a client with nothing wrong with it.
#
# Long enough to be past a rollback window (256 ticks, 2.13s) several times
# over, which is where the online race used to freeze, and past the first
# corner, which is where a camera fault shows.
DRIVE_TICKS = 6 * TICK_HZ

# How long to wait in wall-clock seconds for that much driving. Everything
# before the flag runs on the machine's own time - a lobby, a 148 KB track sent
# in chunks, and the countdown - and a build with the sanitisers on does all of
# it slower, so this is a deadline for giving up rather than a window to fill.
# The race is watched until it has driven enough, and then stopped.
LOCAL_SECONDS = 45.0
SERVER_SECONDS = 75.0

# How far the car has to get from where it started, in tiles, once it is
# allowed to drive. A car that is being driven goes somewhere; a car whose
# controls are not connected sits on the line looking exactly like a car that
# is stationary on purpose.
MOVED_TILES = 3.0

TRACE = re.compile(r"^trace (.*)$")


class Reader:
    """Everything a child writes, read as it is written, on a thread."""

    def __init__(self, stream):
        self.lines = []
        self.done = False
        self.lock = threading.Lock()
        self.stream = stream
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.stream:
            with self.lock:
                self.lines.append(line)
        with self.lock:
            self.done = True

    def text(self):
        with self.lock:
            return "".join(self.lines)

    def traces(self):
        """The trace lines so far, each as a dict of what it said."""
        out = []
        for line in self.text().splitlines():
            # SDL_Log puts its own prefix on; the trace is what follows.
            at = line.find("trace ")
            if at < 0:
                continue
            fields = {}
            for pair in line[at + 6:].split():
                if "=" in pair:
                    k, v = pair.split("=", 1)
                    fields[k] = v
            out.append(fields)
        return out


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def quiet_env(store_dir):
    """Headless, silent, and pointed at a throwaway store."""
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_RENDER_DRIVER"] = "software"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["XDG_DATA_HOME"] = store_dir
    env["HOME"] = store_dir
    env["APPDATA"] = store_dir
    # And the one the game reads itself, because on macOS none of the three
    # above moves the preferences directory: SDL asks the platform, and the
    # platform answers from the password database rather than the environment.
    env["GEARSTICK_PREF_DIR"] = os.path.join(store_dir, "prefs")
    return env


def driven_ticks(reader):
    """How many ticks of *racing* have been seen - the countdown does not
    count. Zero until the lights go green."""
    rows = [r for r in reader.traces()
            if r.get("screen") == "race" and r.get("held") == "0"]
    if len(rows) < 2:
        return 0
    return int(rows[-1]["tick"]) - int(rows[0]["tick"])


def driven_enough(reader):
    """Enough racing has been watched to judge it. Used to stop the client as
    soon as there is, so a fast build is not made to sit through a deadline."""
    return driven_ticks(reader) >= DRIVE_TICKS


def run(argv, env, seconds, until=None):
    """Run a client until `until` says it has seen enough, and at the outside
    for `seconds`, then hand back everything it said.

    Waiting for what is being checked rather than for a stopwatch is what makes
    this the same check on a fast machine and a slow one. The stopwatch is only
    there so a client that never gets going is a failure rather than a hang."""
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, env=env)
    reader = Reader(proc.stdout)
    deadline = time.monotonic() + seconds
    try:
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                break
            if until is not None and until(reader):
                break
            time.sleep(0.1)
    finally:
        # Killed rather than asked to leave, on every platform: terminate() is
        # a signal where signals exist and TerminateProcess where they do not,
        # and a harness that behaves differently on one platform is how a check
        # comes to pass for a reason it does not have everywhere.
        proc.kill()
        proc.wait(timeout=5)
    return reader


def check_race(where, reader):
    """The rules a person checks in the first five seconds."""
    rows = [r for r in reader.traces() if r.get("screen") == "race"]
    log = reader.text()

    if len(rows) < 4:
        print(f"play_check: {where}: the race never got going - "
              f"{len(rows)} trace line(s) from a race\n" + log)
        return False

    # The clock advances, and keeps advancing. A race that freezes reports the
    # same tick for the rest of the run, which is exactly what a rollback with
    # nobody to confirm its ticks did.
    ticks = [int(r["tick"]) for r in rows]
    if ticks[-1] <= ticks[0]:
        print(f"play_check: {where}: the clock did not advance "
              f"({ticks[0]} to {ticks[-1]})\n" + log)
        return False
    if ticks[-1] == ticks[-2]:
        print(f"play_check: {where}: the race stopped at tick {ticks[-1]}\n"
              + log)
        return False

    # The car this machine drives is on this machine's screen. Every time: a
    # camera that is right for the first second and wrong afterwards is a
    # camera that is wrong.
    off = [r for r in rows if r.get("onscreen") != "1"]
    if off:
        first = off[0]
        print(f"play_check: {where}: the car was off the screen on "
              f"{len(off)} of {len(rows)} looks - at tick {first['tick']} it "
              f"was at {first['x']},{first['y']} drawn at "
              f"{first['sx']},{first['sy']} with the camera on "
              f"{first['cam']}\n" + log)
        return False

    # **The race begins on the line, and then it is let go.** Both halves are
    # checked: a countdown that never ends is a race nobody can drive, and no
    # countdown at all is everybody moving before the person watching the lights
    # has reacted. Splitting the run here is also what makes the rules below
    # mean what they say - before the flag the simulation holds every car's
    # input at nothing, so a car that has not moved yet is obeying the rules.
    if "held" not in rows[0]:
        print(f"play_check: {where}: the trace does not say whether the race "
              f"is held - a client too old to check\n" + log)
        return False

    waiting = [r for r in rows if r["held"] == "1"]
    driving = [r for r in rows if r["held"] == "0"]

    if not waiting:
        print(f"play_check: {where}: the race was never held on the line - "
              f"the first thing seen at tick {rows[0]['tick']} was already "
              f"running\n" + log)
        return False
    if len(driving) < 2:
        print(f"play_check: {where}: the lights never went green - still "
              f"counting down at tick {rows[-1]['tick']}\n" + log)
        return False

    drove = int(driving[-1]["tick"]) - int(driving[0]["tick"])
    if drove < DRIVE_TICKS:
        print(f"play_check: {where}: only {drove} ticks of racing before the "
              f"clock ran out, and {DRIVE_TICKS} were asked for\n" + log)
        return False

    # It moves, and gets somewhere. This is the one that says the input path
    # reaches the car at all - and it is asked of the car once it is allowed to
    # drive, because a car being held on a slope slides gently down it and that
    # is not the input path working.
    x0, y0 = float(driving[0]["x"]), float(driving[0]["y"])
    far = max(abs(float(r["x"]) - x0) + abs(float(r["y"]) - y0)
              for r in driving)
    if far < MOVED_TILES:
        print(f"play_check: {where}: the car went {far:.1f} tiles in the "
              f"{drove} ticks after the flag, which is nowhere\n" + log)
        return False

    if any(float(r["speed"]) > 0.5 for r in driving) is False:
        print(f"play_check: {where}: the car never got up any speed\n" + log)
        return False

    # And nothing waited on anybody. A stall is the rollback saying the other
    # machine has gone quiet; in a race with one car in it there is no other
    # machine, and it used to say it anyway.
    stalled = [r for r in rows if r.get("stalls", "0") != "0"]
    if stalled:
        print(f"play_check: {where}: the race stalled "
              f"({stalled[0]['stalls']} times by tick {stalled[0]['tick']})\n"
              + log)
        return False

    print(f"play_check: {where}: raced {ticks[-1]} ticks, held for "
          f"{int(driving[0]['tick'])} of them, then {drove} ticks and "
          f"{far:.0f} tiles of driving, on screen every time, correct")
    return True


def main():
    if len(sys.argv) < 3:
        print("usage: play_check.py <gearstick_server> <gearstick> [assets]")
        return 2
    server_bin, game_bin = sys.argv[1], sys.argv[2]
    assets = sys.argv[3] if len(sys.argv) > 3 else None

    # **On ground that is not at height zero.** The camera fault that started
    # all this was invisible on the flat, and the first track in the library is
    # flat where the grid is - so the local race is pointed at one that is not,
    # and finds the same fault the server's track found.
    local = [game_bin, "--screen", "race", "--autodrive", "--trace"]
    if assets is not None:
        # A hand-authored track rather than a generated one: generated names
        # come from their seeds, so a change to the generator renames them all
        # and a check that hard-codes one silently stops checking anything. This
        # one is written out by name in tools/make_tracks.c and stays.
        local += ["--track", os.path.join(assets, "tracks", "first-light.gstrack")]

    with tempfile.TemporaryDirectory() as tmp:
        store = os.path.join(tmp, "store")
        os.makedirs(store, exist_ok=True)
        env = quiet_env(store)

        # --- a race on this machine ---------------------------------------
        here = run(local, env, LOCAL_SECONDS, driven_enough)
        if not check_race("on this machine", here):
            return 1

        # --- and the same race against a real server -----------------------
        port = free_udp_port()
        server = subprocess.Popen(
            [server_bin, "--port", str(port), "--players", "1", "--plain",
             "--store", os.path.join(tmp, "server.db")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env=env)
        watching = Reader(server.stdout)
        try:
            key = None
            deadline = time.monotonic() + 30.0
            while key is None and time.monotonic() < deadline:
                found = re.search(r"key ([0-9a-f]{64})", watching.text())
                if found:
                    key = found.group(1)
                    break
                if server.poll() is not None:
                    break
                time.sleep(0.05)
            if key is None:
                print("play_check: the server never announced a key\n"
                      + watching.text())
                return 1

            there = run([game_bin, "--server", "127.0.0.1", str(port),
                         "--server-key", key, "--name", "tester",
                         "--screen", "lobby", "--autodrive", "--trace"],
                        env, SERVER_SECONDS, driven_enough)
            if not check_race("at a server", there):
                print("--- and what the server said ---\n" + watching.text())
                return 1
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()

    return 0


if __name__ == "__main__":
    sys.exit(main())
