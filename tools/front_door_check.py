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
# wait for one in the lobby to get in.
#
# The first is a claim: eight seconds of a client sitting on the title screen
# and not being dragged onto the grid is the check. Raising it makes the check
# stricter and slower, and lowering it weakens it, so it is a number with a
# meaning.
#
# **The second is not a claim, it is when to give up**, and it was set when a
# track was 17 KB. A track is 148 KB now - the world went from 64 tiles square
# to 192 - and the lobby it covers is a handshake, that track handed over in 127
# chunks, and the race beginning. Measured on a sanitised build with the machine
# otherwise idle, that takes about nine seconds; with `ctest -j2` running
# something heavy alongside it, thirty had no headroom left and this check
# failed once in a full run and passed alone and on the next one. A deadline
# with no margin is a test that reports the machine's load as a fault in the
# game. Ninety, and the wait still ends the moment the race begins, so a run
# that is going to pass is not made slower by it - see the time it prints.
MENU_SETTLE_SECONDS = 8.0
LOBBY_TIMEOUT_SECONDS = 90.0

RACING = "driving car"

# The line the client prints as a race begins:
#   net: 1 players, driving car 0, 1 car(s) on the grid
# The grid has to be the size the server said, or this machine has built a
# different world from everybody else's - which is the one thing rollback cannot
# recover from. It is checked here because it takes a real server to say how
# many players there are.
GRID = re.compile(r"net: (\d+) players, driving car (\d+), (\d+) car")


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


def run_client(game, port, key, screen, env, seconds, online=False):
    """Run a client on `screen` and say whether it ended up racing.

    With `online`, the client is given no address at all: the server's host,
    port and key are written to a server.txt in the client's own preference
    directory and the client is started with --online, which is the whole of
    joining the server everybody meets at. Both spellings are checked, one
    per half, because the second is a file being read and the first is not."""
    args = [game, "--name", "tester", "--screen", screen]
    env = dict(env)
    if online:
        pref = tempfile.mkdtemp(prefix="gearstick-pref-")
        with open(os.path.join(pref, "server.txt"), "w") as f:
            f.write("# written by the front door check\n")
            f.write(f"127.0.0.1 {port} {key}\n")
        env["GEARSTICK_PREF_DIR"] = pref
        args += ["--online"]
    else:
        args += ["--server", "127.0.0.1", str(port), "--server-key", key]
    proc = subprocess.Popen(
        args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, env=env)

    reader = Reader(proc.stdout)
    try:
        racing = reader.wait_for(RACING, seconds)
    finally:
        # **Killed rather than asked to leave, on every platform.** terminate()
        # is a signal the game can act on where signals exist and is
        # TerminateProcess on Windows, which is not - so a client that says
        # goodbye on one platform and vanishes on another is a difference
        # between platforms baked into the harness. This check has already been
        # bitten once by passing on Linux for a reason it did not have on
        # Windows; the cure is for both to do the harsher thing.
        proc.kill()
        proc.wait(timeout=5)
    return racing, reader.text()


def server_log(watching):
    return "\n--- and what the server said ---\n" + watching.text()


def start_server(server_bin, store_path, env):
    """A server, waited for until it announces the key nobody can connect
    without. Returns it, everything it says, and that key.

    **One each, for the two halves below.** They used to share a server, and
    that made the second half depend on the first client's seat being free -
    which it is on the platforms where terminating a process gets it a chance to
    say goodbye, and is not on Windows, where TerminateProcess does not. A
    one-slot server then held a dead client's seat for its full timeout, the
    second client was told the server was full, and a refused client does not
    knock again by design (see gs_wire_poll). The check failed for a reason that
    had nothing to do with what it was checking."""
    port = free_udp_port()
    proc = subprocess.Popen(
        [server_bin, "--port", str(port), "--players", "1", "--plain",
         "--store", store_path],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    watching = Reader(proc.stdout)

    key = None
    deadline = time.monotonic() + 30.0
    while key is None and time.monotonic() < deadline:
        found = re.search(r"key ([0-9a-f]{64})", watching.text())
        if found:
            key = found.group(1)
            break
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    return proc, watching, port, key


def stop_server(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def main():
    if len(sys.argv) < 3:
        print("usage: front_door_check.py <gearstick_server> <gearstick>")
        return 2
    server_bin, game_bin = sys.argv[1], sys.argv[2]

    with tempfile.TemporaryDirectory() as tmp:
        store_dir = os.path.join(tmp, "store")
        os.makedirs(store_dir, exist_ok=True)
        env = quiet_env(store_dir)

        halves = [
            # A client on the menu is not dragged onto the grid by a server
            # that is ready and waiting.
            ("title", MENU_SETTLE_SECONDS, False,
             "a client on the menu was pulled into the race",
             "the menu did not start a race, correct"),
            # And one in the lobby is - because "the menu did not race" would
            # pass just as well if nothing ever raced at all.
            # Joined by --online and a server.txt rather than an address on
            # the command line: the file being read is the thing checked.
            ("lobby", LOBBY_TIMEOUT_SECONDS, True,
             "a client waiting in the lobby, joined through server.txt, never "
             "got into the race - so the check above proves nothing",
             "the lobby did start a race, joined through server.txt, correct"),
        ]

        for screen, seconds, want_racing, wrong, right in halves:
            server, watching, port, key = start_server(
                server_bin, os.path.join(tmp, screen + ".db"), env)
            try:
                if key is None:
                    print("front_door_check: the server never announced a "
                          "key\n" + watching.text())
                    return 1

                began = time.monotonic()
                racing, log = run_client(game_bin, port, key, screen, env,
                                         seconds, online=want_racing)
                took = time.monotonic() - began

                # **What it built, when it did race.** A grid that is not the
                # size the server said means this machine invented part of the
                # race - it built two cars for a one-player server once, from
                # its own setup screen - and two machines that invent
                # differently are a desync from the first tick.
                if racing:
                    grid = GRID.search(log)
                    if grid is None:
                        print("front_door_check: the client raced without "
                              "saying what grid it built\n" + log)
                        return 1
                    players, local, cars = (int(g) for g in grid.groups())
                    if cars != players or local >= players:
                        print(f"front_door_check: the server said {players} "
                              f"player(s) and this client built {cars} car(s), "
                              f"driving car {local}\n" + log +
                              server_log(watching))
                        return 1
                    print(f"front_door_check: {players} player(s), "
                          f"{cars} car(s) on the grid, correct")

                if racing != want_racing:
                    # **Both sides of the conversation**, because which end
                    # went quiet is the first thing anybody reading this will
                    # want to know, and the client's log alone does not say.
                    print("front_door_check: " + wrong + "\n" + log +
                          server_log(watching))
                    return 1
                # **What it had, against what it was given.** A check that
                # only says "correct" hides the margin it passed by, and this
                # one has failed on the margin rather than on the rule.
                print(f"front_door_check: {right} - {took:.1f}s of {seconds:.0f}")
            finally:
                stop_server(server)

    return 0


if __name__ == "__main__":
    sys.exit(main())
