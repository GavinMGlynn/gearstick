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
  - the clock advances, and keeps advancing
  - the car this machine drives is on this machine's screen, every time
  - it moves, and gets somewhere
  - nothing stalls

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

# Long enough to be past a rollback window (256 ticks, 2.13s) several times
# over, which is where the online race used to freeze, and past the first
# corner, which is where a camera fault shows.
RACE_SECONDS = 14.0

# How far the car has to get from where it started, in tiles. A car that is
# being driven goes somewhere; a car whose controls are not connected sits on
# the line looking exactly like a car that is stationary on purpose.
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
    return env


def run(argv, env, seconds):
    """Run a client for `seconds` and hand back everything it said."""
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, env=env)
    reader = Reader(proc.stdout)
    deadline = time.monotonic() + seconds
    try:
        while time.monotonic() < deadline:
            if proc.poll() is not None:
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

    # It moves, and gets somewhere. This is the one that says the input path
    # reaches the car at all.
    x0, y0 = float(rows[0]["x"]), float(rows[0]["y"])
    far = max(abs(float(r["x"]) - x0) + abs(float(r["y"]) - y0) for r in rows)
    if far < MOVED_TILES:
        print(f"play_check: {where}: the car went {far:.1f} tiles in "
              f"{RACE_SECONDS:.0f}s, which is nowhere\n" + log)
        return False

    if any(float(r["speed"]) > 0.5 for r in rows) is False:
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

    print(f"play_check: {where}: raced {ticks[-1]} ticks, "
          f"{far:.0f} tiles, on screen every time, correct")
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
        here = run(local, env, RACE_SECONDS)
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
                        env, RACE_SECONDS + 6.0)
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
