#!/usr/bin/env python3
"""two_machines_check.py - two real game processes race to the flag through one
server, and the worlds they end in are compared.

Everything under this is proved at the rollback layer: tests/test_wire.c puts
two and four gs_net peers over real sockets, confirms every tick and lands them
on one world. What that cannot say is that the *programs* do - that the client
somebody runs, with its lobby, its track handed over in chunks, its countdown,
the AI at its wheel and its results screen, raced against a second copy of
itself through the server somebody else runs, ends where the other copy ends.
This is that, on the real binaries:

  the CLI writes the short circuit its AI is proved on; the server serves it
  two clients join through the lobby and drive themselves to the flag
  each prints the hash of the race everybody agreed on, and hands it in
  the server re-races each recording before it believes a word of it

Three witnesses have to agree: the two clients, on one hash at one tick, and
the server, which built the same world again from the inputs alone. A client
that ended somewhere else, a recording that does not re-race to its own
ending, or a race that never reached the flag, is a red check with all three
logs attached.
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

# How long to give the whole thing in wall-clock seconds. A lobby, a track in
# chunks, a ten-second countdown, three laps of a circuit the AI takes about
# twenty-four seconds round with another car in the way, and the dozen ticks of
# settling - about two and a half minutes at full speed, twice that with the
# sanitisers on. The check stops the moment all three witnesses have spoken,
# so on a fast machine this is a bound rather than a wait.
SECONDS = 300.0

AGREED = re.compile(
    r"net: the race is agreed at tick (\d+) with hash ([0-9a-f]{16}), and submitted")
VERIFIED = re.compile(r"(\S+): time verified by re-racing it")
REJECTED = re.compile(r"(\S+): time rejected - (.*)")
NAMES = ("north", "south")


class Reader:
    """Everything a child writes, read as it is written, on a thread."""

    def __init__(self, stream):
        self.lines = []
        self.lock = threading.Lock()
        threading.Thread(target=self._pump, args=(stream,), daemon=True).start()

    def _pump(self, stream):
        for line in stream:
            with self.lock:
                self.lines.append(line)

    def text(self):
        with self.lock:
            return "".join(self.lines)

    def traces(self):
        """The trace lines so far, each as a dict of what it said."""
        out = []
        for line in self.text().splitlines():
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

    def agreed(self):
        m = AGREED.search(self.text())
        return (int(m.group(1)), m.group(2)) if m else None


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def quiet_env(store_dir):
    """Headless, silent, and pointed at a throwaway store of its own - two
    clients on one machine must not share a preferences directory, or they
    are one player twice."""
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_RENDER_DRIVER"] = "software"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["XDG_DATA_HOME"] = store_dir
    env["HOME"] = store_dir
    env["APPDATA"] = store_dir
    env["GEARSTICK_PREF_DIR"] = os.path.join(store_dir, "prefs")
    return env


def stop(proc):
    """Killed rather than asked to leave, on every platform."""
    if proc.poll() is None:
        proc.kill()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


def fail(why, server, clients):
    print("two_machines_check: " + why)
    for name, reader in zip(NAMES, clients):
        print(f"--- what {name} said ---\n" + reader.text())
    print("--- what the server said ---\n" + server.text())
    return 1


def main():
    if len(sys.argv) < 4:
        print("usage: two_machines_check.py <gearstick_server> <gearstick> "
              "<gearstick_cli>")
        return 2
    server_bin, game_bin, cli_bin = sys.argv[1], sys.argv[2], sys.argv[3]

    with tempfile.TemporaryDirectory() as tmp:
        # --- a track short enough to race to the flag inside a test ---------
        # The shipped set is deliberately long - four to five minutes a lap for
        # the AI - and the server races three laps. The CLI writes the circuit
        # its own AI is proved on, four gates and about twenty-four seconds a
        # lap. (Not `track`: that writes the selftest fixture, which has no
        # gates, and two cars sat on it for five minutes with nothing to
        # follow before this said `circuit`.)
        circuit = os.path.join(tmp, "circuit.gstrack")
        wrote = subprocess.run([cli_bin, "circuit", circuit],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
        if wrote.returncode != 0 or not os.path.exists(circuit):
            print("two_machines_check: the CLI could not write the circuit\n"
                  + wrote.stdout)
            return 1

        # --- the server, waiting for two ------------------------------------
        port = free_udp_port()
        server_env = quiet_env(os.path.join(tmp, "server"))
        os.makedirs(server_env["GEARSTICK_PREF_DIR"], exist_ok=True)
        server_proc = subprocess.Popen(
            [server_bin, "--port", str(port), "--players", "2", "--plain",
             "--headless", "--track", circuit,
             "--store", os.path.join(tmp, "server.db")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env=server_env)
        server = Reader(server_proc.stdout)
        clients, procs = [], []
        try:
            key = None
            deadline = time.monotonic() + 30.0
            while key is None and time.monotonic() < deadline:
                found = re.search(r"key ([0-9a-f]{64})", server.text())
                if found:
                    key = found.group(1)
                    break
                if server_proc.poll() is not None:
                    break
                time.sleep(0.05)
            if key is None:
                return fail("the server never announced a key", server, clients)

            # --- two clients, each its own machine as far as the game knows --
            for name in NAMES:
                env = quiet_env(os.path.join(tmp, name))
                os.makedirs(env["GEARSTICK_PREF_DIR"], exist_ok=True)
                proc = subprocess.Popen(
                    [game_bin, "--server", "127.0.0.1", str(port),
                     "--server-key", key, "--name", name, "--screen", "lobby",
                     "--autodrive", "--trace"],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    env=env)
                procs.append(proc)
                clients.append(Reader(proc.stdout))

            # --- until all three witnesses have spoken -----------------------
            deadline = time.monotonic() + SECONDS
            while time.monotonic() < deadline:
                if REJECTED.search(server.text()):
                    break
                if (all(r.agreed() is not None for r in clients)
                        and len(VERIFIED.findall(server.text())) >= 2):
                    break
                if any(p.poll() is not None for p in procs):
                    break
                time.sleep(0.1)
        finally:
            for p in procs:
                stop(p)
            server_proc.terminate()
            try:
                server_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server_proc.kill()

        # --- the rules -------------------------------------------------------
        for name, reader, proc in zip(NAMES, clients, procs):
            rows = [r for r in reader.traces() if r.get("screen") == "race"]
            if len(rows) < 2:
                return fail(f"{name} never got into the race", server, clients)
            # Not "did a trace line say over=1": the flag falls on a single
            # tick and the trace samples about once a second, so the finishing
            # tick is usually never the one photographed. Whether the machine
            # finished is whether it agreed the race and handed it in - which is
            # the thing being tested, and the line the whole check turns on.
            if reader.agreed() is None:
                last = rows[-1]
                return fail(f"{name} never agreed and submitted a race - last "
                            f"seen at tick {last.get('tick')} on lap "
                            f"{last.get('lap')}", server, clients)

        rejected = REJECTED.search(server.text())
        if rejected:
            return fail(f"the server rejected {rejected.group(1)}'s race - "
                        f"{rejected.group(2)}", server, clients)

        (tick_a, hash_a), (tick_b, hash_b) = (r.agreed() for r in clients)
        if hash_a != hash_b or tick_a != tick_b:
            return fail(f"the two machines ended in different worlds - "
                        f"{NAMES[0]} agreed {hash_a} at tick {tick_a}, "
                        f"{NAMES[1]} agreed {hash_b} at tick {tick_b}",
                        server, clients)

        verified = set(VERIFIED.findall(server.text()))
        missing = [n for n in NAMES if n not in verified]
        if missing:
            return fail(f"the server never re-raced and verified "
                        f"{' and '.join(missing)}", server, clients)

        # And say what was seen, for the next person reading the log.
        stalls = [max((int(r.get("stalls", "0")) for r in reader.traces()
                       if r.get("screen") == "race"), default=0)
                  for reader in clients]
        print(f"two_machines_check: {NAMES[0]} and {NAMES[1]} raced to the flag "
              f"through one server, agreed on {hash_a} at tick {tick_a}, and the "
              f"server re-raced both recordings to it (stalls {stalls[0]} and "
              f"{stalls[1]}), correct")
    return 0


if __name__ == "__main__":
    sys.exit(main())
