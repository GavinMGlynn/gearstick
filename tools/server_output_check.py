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
        print("usage: server_output_check.py <gearstick_server>")
        return 2
    server_bin = sys.argv[1]

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
             "--seconds", str(RUN_SECONDS),
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
