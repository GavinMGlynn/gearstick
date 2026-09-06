#!/usr/bin/env python3
"""records_check.py - a time set in an online race reaches the records.

The rollback layer is proved in tests/test_wire.c: peers over real sockets
confirm every tick and land on one world. What that cannot say is that a
networked race, run by the client somebody actually starts, produces a
*recording the server will keep* - and for a long time it did not, in two ways
that only a race through a real server could show:

  - the confirmed recording was harvested only once the race was over, by which
    time gs_net had kept only the last two seconds of it, so it came up empty
    and nothing submittable was ever built; and
  - the server verified car 0 of whatever arrived, so only a driver who started
    on pole could have a time kept - everyone else was "somebody else drove
    that".

This races one real client, driving itself, to the flag through one real
server on the short circuit the CLI writes, and asserts the whole path a
record travels: the recording keeps up (never "fell behind"), the race is
agreed and submitted, and the server re-races it and keeps it. It is the first
end-to-end cover of online -> server records; before it, that path had none.

(Two machines that both finish is a further step - see
tools/two_machines_check.py and PROJECT_STATUS.md: it needs a settle-time fix
that is not this one.)
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

SECONDS = 120.0
AGREED = re.compile(
    r"net: the race is agreed at tick (\d+) with hash ([0-9a-f]{16}), and submitted")
VERIFIED = re.compile(r"(\S+): time verified by re-racing it")
REJECTED = re.compile(r"(\S+): time rejected - (.*)")
FELL_BEHIND = "the recording fell behind the race"
NAME = "solo"


class Reader:
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


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def quiet_env(store_dir):
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_RENDER_DRIVER"] = "software"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["XDG_DATA_HOME"] = store_dir
    env["HOME"] = store_dir
    env["APPDATA"] = store_dir
    env["GEARSTICK_PREF_DIR"] = os.path.join(store_dir, "prefs")
    os.makedirs(env["GEARSTICK_PREF_DIR"], exist_ok=True)
    return env


def fail(why, client, server):
    print("records_check: " + why)
    print("--- what the client said ---\n" + client.text())
    print("--- what the server said ---\n" + server.text())
    return 1


def main():
    if len(sys.argv) < 4:
        print("usage: records_check.py <gearstick_server> <gearstick> "
              "<gearstick_cli>")
        return 2
    server_bin, game_bin, cli_bin = sys.argv[1], sys.argv[2], sys.argv[3]

    with tempfile.TemporaryDirectory() as tmp:
        circuit = os.path.join(tmp, "circuit.gstrack")
        wrote = subprocess.run([cli_bin, "circuit", circuit],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
        if wrote.returncode != 0 or not os.path.exists(circuit):
            print("records_check: the CLI could not write the circuit\n"
                  + wrote.stdout)
            return 1

        port = free_udp_port()
        server_proc = subprocess.Popen(
            [server_bin, "--port", str(port), "--players", "1", "--plain",
             "--headless", "--track", circuit,
             "--store", os.path.join(tmp, "server.db")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env=quiet_env(os.path.join(tmp, "server")))
        server = Reader(server_proc.stdout)
        client = Reader(subprocess.PIPE)          # replaced below
        client_proc = None
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
                return fail("the server never announced a key", client, server)

            client_proc = subprocess.Popen(
                [game_bin, "--server", "127.0.0.1", str(port), "--server-key",
                 key, "--name", NAME, "--screen", "lobby", "--autodrive",
                 "--trace"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=quiet_env(os.path.join(tmp, "client")))
            client = Reader(client_proc.stdout)

            deadline = time.monotonic() + SECONDS
            while time.monotonic() < deadline:
                if REJECTED.search(server.text()):
                    break
                if AGREED.search(client.text()) and VERIFIED.search(server.text()):
                    break
                if "nobody finished agreeing" in client.text():
                    break
                if client_proc.poll() is not None:
                    break
                time.sleep(0.1)
        finally:
            if client_proc is not None and client_proc.poll() is None:
                client_proc.kill()
                client_proc.wait(timeout=5)
            server_proc.terminate()
            try:
                server_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server_proc.kill()

        # --- the rules -------------------------------------------------------
        if FELL_BEHIND in client.text():
            return fail("the recording fell behind the race - the confirmed "
                        "ticks were harvested too late to reproduce it", client,
                        server)
        agreed = AGREED.search(client.text())
        if agreed is None:
            return fail("the client never agreed and submitted a race", client,
                        server)
        rejected = REJECTED.search(server.text())
        if rejected:
            return fail(f"the server rejected {rejected.group(1)}'s time - "
                        f"{rejected.group(2)}", client, server)
        verified = VERIFIED.search(server.text())
        if verified is None:
            return fail("the server never re-raced and verified the time",
                        client, server)

        print(f"records_check: {NAME} raced online to the flag, agreed the race "
              f"at tick {agreed.group(1)} (hash {agreed.group(2)}), and the "
              f"server re-raced the recording and kept it, correct")
    return 0


if __name__ == "__main__":
    sys.exit(main())
