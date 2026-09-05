#!/usr/bin/env python3
"""server_output_check.py - a server whose output nobody reads keeps serving.

**This exists because a server stopped being a server on account of its own
output.** The dashboard repaints four times a second, and printed into a pipe
that nobody is draining it fills the buffer; the next `printf` blocks, and a
server blocked inside `printf` answers nothing on the network. A pipe on Windows
holds four kilobytes by default - about one second of dashboard - so a Windows
client could never join a server whose output was being captured, while the same
pair worked on Linux for the sixteen seconds it took to fill a larger pipe. The
front door check found it; this pins it.

Two things are checked, because either alone is weak:

  - the server, with its output going into a pipe nobody reads, stops when it
    was told to. Blocked in a write it would never reach the clock at all.
    This reproduces the original fault on its own where pipes are small.
  - and what it wrote in that time is less than a pipe holds, which is what
    makes the first true on a machine whose pipes are bigger. Four kilobytes
    is the smallest buffer any of the three platforms gives us.

Nothing here needs a client: what is being pinned is what the server does with
its own output, and dragging a race into it would only make it slower to run and
harder to believe.
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import time

# Long enough that the old dashboard - four repaints a second, six hundred bytes
# each - would have written five times a small pipe's capacity.
RUN_SECONDS = 5

# What a pipe holds on the tightest of the three platforms: CreatePipe on
# Windows takes a default buffer of 4096 bytes. Anything under this in a run
# this long cannot fill one, whoever is or is not reading.
PIPE_BYTES = 4096

# The server is told to stop after RUN_SECONDS; this is how much longer than
# that it is given before being called stuck. Generous, because a loaded CI
# machine is slow and a blocked server is stuck forever - the two are never
# close enough to confuse.
GRACE_SECONDS = 20


def main():
    if len(sys.argv) < 2:
        print("usage: server_output_check.py <gearstick_server> [<gearstick>]")
        return 2
    server_bin = sys.argv[1]
    game_bin = sys.argv[2] if len(sys.argv) > 2 else None

    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ)
        # A throwaway store, so a check cannot touch the drivers and records
        # somebody plays with.
        env["XDG_DATA_HOME"] = tmp
        env["HOME"] = tmp
        env["APPDATA"] = tmp

        # **Nothing reads this pipe until the process is gone**, which is the
        # whole situation being tested. --plain as well, so this is not passing
        # for the lesser reason that cursor control was switched off.
        server = subprocess.Popen(
            [server_bin, "--port", "0", "--players", "1", "--plain",
             "--headless", "--seconds", str(RUN_SECONDS),
             "--store", os.path.join(tmp, "server.db")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)

        started = time.monotonic()
        try:
            server.wait(timeout=RUN_SECONDS + GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate()
            print("server_output_check: the server was told to stop after "
                  f"{RUN_SECONDS}s and was still running "
                  f"{RUN_SECONDS + GRACE_SECONDS}s later. A server that is "
                  "blocked writing to a pipe nobody is reading never reaches "
                  "its own clock - and never answers a client either.")
            return 1
        took = time.monotonic() - started

        written = server.stdout.read()
        server.stdout.close()

        print(f"server_output_check: stopped after {took:.1f}s, "
              f"having written {len(written)} bytes to a pipe nobody read")

        if len(written) > PIPE_BYTES:
            print("server_output_check: that is more than a pipe holds "
                  f"({PIPE_BYTES} bytes). It fitted this time because this "
                  "machine's pipe is bigger; on a machine whose pipe is not, "
                  "the server would have blocked and stopped serving.\n"
                  + written.decode("utf-8", "replace"))
            return 1

        # And the control: silence would pass the two checks above and mean the
        # server never started. The key is the line a client cannot connect
        # without, so its absence is not a small thing either.
        if b"key " not in written:
            print("server_output_check: the server never announced its key, "
                  "so it never got as far as listening - the checks above "
                  "passed for the wrong reason.\n"
                  + written.decode("utf-8", "replace"))
            return 1

    print("server_output_check: a server nobody is reading keeps going, "
          "correct")

    if not dashboard_check(server_bin):
        return 1
    if not window_check(server_bin, game_bin):
        return 1
    if not no_display_check(server_bin):
        return 1
    if not press_check(server_bin, game_bin, "drop",
                       "dropped by the operator", needs_client=True):
        return 1
    if not press_check(server_bin, game_bin, "take the track down",
                       "taken down", needs_client=False):
        return 1
    return 0


# --- and the other side of that gate -----------------------------------------
#
# **The dashboard had never been drawn by anything.** Everything above pins what
# the server does when its output is *not* a terminal, which is the fault that
# was found in front of a player - and it is the whole reason the dashboard is
# switched off there. So the dashboard itself, fifty-nine lines of the server's
# only user interface, was reached by no test at all: a coverage build put
# `gs_draw` at zero while twenty-six tests hammered the server.
#
# Drawing it needs a terminal, so this gives it one. Where there is no pty -
# Windows - there is no way to ask for the dashboard and this says so rather
# than passing quietly.
def dashboard_check(server_bin):
    try:
        import pty
    except ImportError:
        print("server_output_check: no pty on this platform, so the dashboard "
              "cannot be asked for and is not checked here")
        return True

    primary, secondary = pty.openpty()
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ)
        env["XDG_DATA_HOME"] = tmp
        env["HOME"] = tmp
        env["APPDATA"] = tmp

        # --plain, so what comes back is the table rather than the table with
        # cursor control wrapped round it.
        server = subprocess.Popen(
            [server_bin, "--port", "0", "--players", "2", "--plain",
             "--headless", "--seconds", "3",
             "--store", os.path.join(tmp, "dash.db")],
            stdout=secondary, stderr=subprocess.STDOUT, env=env)
        os.close(secondary)

        seen = b""
        deadline = time.monotonic() + 3 + GRACE_SECONDS
        while time.monotonic() < deadline:
            try:
                chunk = os.read(primary, 65536)
            except OSError:
                break
            if not chunk:
                break
            seen += chunk
        os.close(primary)
        server.wait(timeout=GRACE_SECONDS)

    text = seen.decode("utf-8", "replace")

    # The header, the column titles, and the footer that says how to leave -
    # which together are the dashboard being drawn rather than a log line that
    # happens to mention the server.
    for want in ("gearstick server", "driver", "ping", "port"):
        if want not in text:
            print("server_output_check: given a terminal, the server did not "
                  f"draw its dashboard - no {want!r} in what it wrote.\n"
                  + text[:2000])
            return False

    # And it repaints. Four times a second for three seconds is a dozen; one
    # would mean it drew once and then stopped, which is a dashboard that
    # cannot show anybody arriving.
    drawn = text.count("gearstick server")
    if drawn < 4:
        print("server_output_check: the dashboard was drawn "
              f"{drawn} time(s) in three seconds, so it is not repainting")
        return False

    print(f"server_output_check: given a terminal the dashboard drew "
          f"{drawn} times in three seconds, correct")
    return True


# --- the window ---------------------------------------------------------------
#
# **The window and the terminal report the same numbers from the same race.**
# On a machine with a display the server opens a window as well as drawing its
# dashboard, and the two are drawn from one set of facts. This holds them to it
# on real output: a run on a pty with SDL's dummy video driver - a window nobody
# can see, which is still a window the code draws into - and --window-dump,
# which prints what the window showed in its last frame after the terminal's
# last dashboard. Every line of the dashboard must appear among the window's
# lines, spacing aside, and the window must have said it was open.
#
# With the game to hand, a client joins through the lobby first, so the table
# has a driver on it and the log has an arrival - and the arrival, which the
# terminal scrolled away under its dashboard, must be among the window's lines
# too. Without the game the check runs on an empty server and says so.
def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def window_check(server_bin, game_bin):
    try:
        import pty
    except ImportError:
        print("server_output_check: no pty on this platform, so the window "
              "cannot be held against the dashboard here")
        return True

    primary, secondary = pty.openpty()
    client = None
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ)
        env["XDG_DATA_HOME"] = tmp
        env["HOME"] = tmp
        env["APPDATA"] = tmp
        env["SDL_VIDEODRIVER"] = "dummy"
        env["SDL_RENDER_DRIVER"] = "software"
        env["SDL_AUDIO_DRIVER"] = "dummy"

        port = free_udp_port()
        # Long enough for a client to get through the lobby on a loaded
        # machine; the front door check allows ninety for the same thing.
        seconds = 40 if game_bin else 3
        server = subprocess.Popen(
            [server_bin, "--port", str(port), "--players", "2", "--plain",
             "--seconds", str(seconds), "--window-dump",
             "--store", os.path.join(tmp, "window.db")],
            stdout=secondary, stderr=subprocess.STDOUT, env=env)
        os.close(secondary)

        seen = b""
        deadline = time.monotonic() + seconds + GRACE_SECONDS
        while time.monotonic() < deadline:
            try:
                chunk = os.read(primary, 65536)
            except OSError:
                break
            if not chunk:
                break
            seen += chunk
            # A client, once the server has said the key nobody can connect
            # without. It stays until the server stops, so the last dashboard
            # and the last frame both have it on the table.
            if game_bin and client is None:
                found = re.search(rb"key ([0-9a-f]{64})", seen)
                if found:
                    client = subprocess.Popen(
                        [game_bin, "--server", "127.0.0.1", str(port),
                         "--server-key", found.group(1).decode(),
                         "--name", "tester", "--screen", "lobby"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                        env=env)
        os.close(primary)
        server.wait(timeout=GRACE_SECONDS)
        if client is not None:
            client.kill()
            client.wait(timeout=5)

    text = seen.decode("utf-8", "replace")

    if "a window is open as well" not in text:
        print("server_output_check: with a display on offer the server did "
              "not open a window.\n" + text[:2000])
        return False
    if game_bin and "(tester) joined" not in text:
        print("server_output_check: the client never joined, so the window "
              "was held against an empty table.\n" + text[-3000:])
        return False

    # The window's lines, which come after the terminal's last dashboard -
    # and the dashboard itself, which is the terminal's text with the
    # window's lines taken out of it, from its last header to the line that
    # says how to leave.
    window = [" ".join(l[len("window: "):].split()) for l in text.splitlines()
              if l.startswith("window: ")]
    terminal = "\n".join(l for l in text.splitlines()
                         if not l.startswith("window: "))
    head = terminal.rfind("gearstick server")
    tail = terminal.find("ctrl-c to stop", head)
    if head < 0 or tail < 0:
        print("server_output_check: no final dashboard to hold the window "
              "against.\n" + text[-2000:])
        return False
    dashboard = [" ".join(l.split()) for l in terminal[head:tail].splitlines()]
    dashboard = [l for l in dashboard if l and not l.startswith("---")]
    if not window:
        print("server_output_check: --window-dump printed nothing.\n"
              + text[-2000:])
        return False

    missing = [l for l in dashboard if l not in window]
    if missing:
        print("server_output_check: the window did not show what the "
              "terminal showed. Missing from the window:\n  "
              + "\n  ".join(missing) + "\nthe window showed:\n  "
              + "\n  ".join(window))
        return False

    # The row and the arrival: a driver on the table in both views, and the
    # line the terminal scrolled away kept by the window.
    if game_bin:
        if not any(l.startswith("0 tester ") for l in dashboard):
            print("server_output_check: the client joined but the final "
                  "dashboard has no row for it.\n  " + "\n  ".join(dashboard))
            return False
        if not any("(tester) joined from" in l for l in window):
            print("server_output_check: the window kept no arrival line for "
                  "the client.\n  " + "\n  ".join(window))
            return False

    print(f"server_output_check: the window showed all {len(dashboard)} "
          "lines of the terminal's dashboard"
          + (", the client's row and its arrival" if game_bin else "")
          + ", correct")
    return True


# **And with no display the server runs exactly as it did.** A video driver
# that does not exist is a machine with no screen; the server says so once and
# carries on with its log, which is what it wrote before there was a window.
def no_display_check(server_bin):
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ)
        env["XDG_DATA_HOME"] = tmp
        env["HOME"] = tmp
        env["APPDATA"] = tmp
        env["SDL_VIDEODRIVER"] = "there-is-no-such-driver"

        server = subprocess.Popen(
            [server_bin, "--port", "0", "--players", "1", "--plain",
             "--seconds", "2",
             "--store", os.path.join(tmp, "blind.db")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        try:
            written, _ = server.communicate(timeout=2 + GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate()
            print("server_output_check: with no display the server did not "
                  "stop when told to")
            return False

    text = written.decode("utf-8", "replace")
    for want in ("no display, so no window", "key ",
                 "this output is not a terminal, so there is no dashboard"):
        if want not in text:
            print("server_output_check: with no display the server did not "
                  f"say {want!r}.\n" + text[:2000])
            return False
    print("server_output_check: with no display the server said so once and "
          "ran as it did, correct")
    return True


# **The operator's controls do what they say.** The window has a button beside
# each client that drops it and one beside the track that takes it down. Nobody
# can click a window drawn by the dummy driver, so --window-press names a
# button and the server presses it through Dear ImGui's own item hooks the
# first time it is drawn - the button's real path, not a flag standing in for
# it. What follows must show in the server's log: the client left because the
# operator said so, or the track was taken down.
def press_check(server_bin, game_bin, label, expect, needs_client):
    if needs_client and not game_bin:
        print(f"server_output_check: no game to be the client, so {label!r} "
              "is not pressed here")
        return True
    client = None
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ)
        env["XDG_DATA_HOME"] = tmp
        env["HOME"] = tmp
        env["APPDATA"] = tmp
        env["SDL_VIDEODRIVER"] = "dummy"
        env["SDL_RENDER_DRIVER"] = "software"
        env["SDL_AUDIO_DRIVER"] = "dummy"

        port = free_udp_port()
        seconds = 40 if needs_client else 3
        server = subprocess.Popen(
            [server_bin, "--port", str(port), "--players", "2", "--plain",
             "--seconds", str(seconds), "--window-press", label,
             "--store", os.path.join(tmp, "press.db")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)

        seen = b""
        deadline = time.monotonic() + seconds + GRACE_SECONDS
        while time.monotonic() < deadline:
            chunk = server.stdout.read1(65536) if hasattr(server.stdout, "read1") \
                else server.stdout.read(1)
            if not chunk:
                break
            seen += chunk
            if needs_client and client is None:
                found = re.search(rb"key ([0-9a-f]{64})", seen)
                if found:
                    client = subprocess.Popen(
                        [game_bin, "--server", "127.0.0.1", str(port),
                         "--server-key", found.group(1).decode(),
                         "--name", "tester", "--screen", "lobby"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                        env=env)
        server.stdout.close()
        server.wait(timeout=GRACE_SECONDS)
        if client is not None:
            client.kill()
            client.wait(timeout=5)

    text = seen.decode("utf-8", "replace")
    if needs_client and "(tester) joined" not in text:
        print(f"server_output_check: the client never joined, so {label!r} "
              "had nothing to act on.\n" + text[-3000:])
        return False
    if expect not in text:
        print(f"server_output_check: pressing {label!r} in the window did not "
              f"lead to {expect!r} in the log.\n" + text[-3000:])
        return False
    print(f"server_output_check: pressing {label!r} in the window led to "
          f"{expect!r}, correct")
    return True


if __name__ == "__main__":
    sys.exit(main())
