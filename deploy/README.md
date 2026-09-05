# Running the central server

Gearstick meets at **one** server. The decision and its reasons are in
`docs/COMPLETION_PLAN.md`; this directory is what it takes to run that server
somewhere, and to point the game at it.

The server is `gearstick_server`, one static binary that needs no display and
no assets beyond the track library it ships with. It listens on one UDP port,
keeps drivers, records and tracks in one SQLite file, and re-races every time
it is sent before it keeps it. It was built to run on a machine with no
screen; `--headless` says so explicitly.

## The one-machine way

On any Linux box with the release package unpacked:

```sh
sudo install -m 755 gearstick_server /usr/local/bin/
sudo mkdir -p /var/lib/gearstick
sudo cp -r assets /var/lib/gearstick/assets
sudo install -m 644 deploy/gearstick-server.service /etc/systemd/system/
sudo systemctl enable --now gearstick-server
journalctl -u gearstick-server -f
```

The unit runs it headless on port 47800 with its store at
`/var/lib/gearstick/gearstick.db`, restarts it if it stops, and the first
start mints the server's identity and keeps it in the store. **The line to
copy from the log is the key**:

```
  key 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

Open UDP 47800 on the machine's firewall. That is the whole of hosting it.

## The container way

```sh
docker build -f deploy/Dockerfile -t gearstick-server .
docker volume create gearstick-store
docker run -d --name gearstick-server -p 47800:47800/udp \
    -v gearstick-store:/var/lib/gearstick gearstick-server
docker logs gearstick-server | grep key
```

The image builds the server from this source tree, so it is the server this
tree describes and no other.

## Pointing the game at it

Put the host, the port and the key on one line of `assets/server.txt` and
ship the game; `gearstick --online` then joins it with nothing to type. A
player who wants a different server writes the same line into a `server.txt`
in their own preference directory, which wins.

## Keeping it up

- The store is the only state. Back up `/var/lib/gearstick/gearstick.db`
  (or the `gearstick-store` volume); the server's identity is in it, and a
  server with a new identity is a server nobody's game trusts until
  `server.txt` is updated.
- `gearstick_server --help` lists the dials: how many players, how long a
  silence is a departure, a track to serve, a reversed race.
- The dashboard is drawn only to a terminal, and the log a line a minute
  otherwise, on purpose: a server whose output nobody reads keeps serving.
