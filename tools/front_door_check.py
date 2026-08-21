#!/usr/bin/env python3
"""front_door_check.py - the front door, checked against the real programs.

**This exists because of a bug that no unit test could have caught.** The rule
it pins lives in the frontend's main loop, which links a window system and is
not something the test executables can reach: a client that has signed in and is
reading the records must not be dragged onto the grid because a server somewhere
decided the race could start. Waiting in the lobby is how somebody says yes.

The first version of the login screen sent an online player straight to the
lobby on signing in, and a one-player server is ready immediately, so signing in
started a race. That is exactly the shape of thing a real client talking to a
real server notices and a unit test does not.

Both halves are checked, because only checking that the menu does not race would
pass just as well if nothing ever raced at all:

  - a client sitting on the menu must still be on the menu a while later
  - a client waiting in the lobby must get into the race

Everything runs headless and silent, and the store is redirected into a
temporary directory so a test run cannot touch the profile somebody plays with.

**The server's output is read the whole way through**, on a thread of its own.
Reading until the key and then walking away is what this used to do, and it cost
an afternoon: a pipe nobody drains fills up, the server blocks inside printf,
and a client sits at a lobby that is never going to answer. The server no longer
writes enough to fill one - see tools/server_output_check.py - but a harness
that leaves a child's pipe unread is a trap for whoever writes the next one.
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

# How long to believe a client on the menu is staying there, and how long to
# wait for one in the lobby to get in. The second is generous because it covers
# a handshake, a track transfer and the race actually beginning.
MENU_SETTLE_SECONDS = 8.0
LOBBY_TIMEOUT_SECONDS = 30.0

RACING = "driving car"


def free_udp_port():
    """A port nothing is on. Bound and released, which is racy in principle and
    fine here - the alternative is a hard-coded port that collides with whatever
    the person running the tests happens to have open."""
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
    # SDL_GetPrefPath reads a different one of these on each platform. Setting
    # all three is how this stays a test rather than a way to lose somebody's
    # drivers and records.
    env["XDG_DATA_HOME"] = store_dir
    env["HOME"] = store_dir
    env["APPDATA"] = store_dir
    return env


class Reader:
    """Everything a child writes, read as it is written, on a thread.

    A blocking readline on a thread rather than a non-blocking one in the loop:
    os.set_blocking only learned about pipes on Windows in Python 3.12, and a
    check that quietly does nothing on an older interpreter is worse than one
    that fails."""

    def __init__(self, stream):
        self.stream = stream
        self.lines = []
        self.done = False
        self.lock = threading.Lock()
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

    def wait_for(self, needle, seconds):
        """Wait for a line containing `needle`, and say whether one came. Ends
        early if the child has gone and everything it wrote has been read -
        waiting the rest of the time out for a process that has exited only
        makes the failure slower to arrive."""
        deadline = time.monotonic() + seconds
        seen = 0
        while time.monotonic() < deadline:
            with self.lock:
                lines, done = self.lines[seen:], self.done
            seen += len(lines)
            for line in lines:
                if needle in line:
                    return True
            if done:
                return False
            time.sleep(0.05)
        return False


def run_client(game, port, key, screen, env, seconds):
    """Run a client on `screen` and say whether it ended up racing."""
    proc = subprocess.Popen(
        [game, "--server", "127.0.0.1", str(port), "--server-key", key,
         "--name", "tester", "--screen", screen],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, env=env)

    reader = Reader(proc.stdout)
    try:
        racing = reader.wait_for(RACING, seconds)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return racing, reader.text()


def server_log(watching):
    return "\n--- and what the server said ---\n" + watching.text()


def main():
    if len(sys.argv) < 3:
        print("usage: front_door_check.py <gearstick_server> <gearstick>")
        return 2
    server_bin, game_bin = sys.argv[1], sys.argv[2]

    with tempfile.TemporaryDirectory() as tmp:
        store_dir = os.path.join(tmp, "store")
        os.makedirs(store_dir, exist_ok=True)
        env = quiet_env(store_dir)
        port = free_udp_port()

        server = subprocess.Popen(
            [server_bin, "--port", str(port), "--players", "1", "--plain",
             "--store", os.path.join(tmp, "server.db")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env=env)

        # Read from here to the end of the run, so the server is never held up
        # by a pipe that has filled behind it.
        watching = Reader(server.stdout)

        try:
            # The server prints its public key on the way up; a client without
            # it refuses to connect, which is the whole point of IK.
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
                print("front_door_check: the server never announced a key\n"
                      + watching.text())
                return 1

            # --- the menu must not be a way into a race --------------------
            racing, log = run_client(game_bin, port, key, "title", env,
                                     MENU_SETTLE_SECONDS)
            if racing:
                print("front_door_check: a client on the menu was pulled into "
                      "the race\n" + log + server_log(watching))
                return 1
            print("front_door_check: the menu did not start a race, correct")

            # --- and the lobby must be ------------------------------------
            racing, log = run_client(game_bin, port, key, "lobby", env,
                                     LOBBY_TIMEOUT_SECONDS)
            if not racing:
                # **Both sides of the conversation**, because which end went
                # quiet is the first thing anybody reading this will want to
                # know, and the client's log alone does not say.
                print("front_door_check: a client waiting in the lobby never "
                      "got into the race - so the check above proves nothing\n"
                      + log + server_log(watching))
                return 1
            print("front_door_check: the lobby did start a race, correct")
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()

    return 0


if __name__ == "__main__":
    sys.exit(main())
