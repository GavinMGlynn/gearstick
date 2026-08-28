#!/usr/bin/env python3
"""library_refresh_check.py - the shipped tracks reach somebody who has played.

**This exists because of a bug no unit test could have caught.** The rule lives
in the frontend's start-up, which links a window system and is not something the
test executables can reach: the library a returning player sees has to be their
own tracks plus the ones the game ships *now*.

What happened instead was that `gs_load_stock_tracks` ran first and
`gs_store_load` ran a hundred and eighty lines later, and reading a store
replaces the library - it has to, since a saved library is the whole of what
somebody has. So every shipped track loaded at start-up was thrown away before
the menu appeared, for everybody who had ever played before. The generator was
fixed on the 22nd and the tracks it made were still not reaching a player who
started on the 19th, six days and one release later. Both halves of the rule are
checked here, because "the shipped tracks are there" would pass just as well if
nothing was ever withdrawn and a library only grew:

  - a track the game no longer ships is gone
  - a track the game ships now is there, on a machine with a store already

The two runs differ only in which tracks the game ships, which is what
GEARSTICK_ASSETS_DIR is for: staging a different shipped set beats a test that
writes into the repository it is testing.

Everything runs headless and silent, and the store goes into a temporary
directory so a run cannot touch the library somebody plays with.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

# The line the client prints when it reconciles the library with what ships:
#   library: 3 shipped track(s), 1 withdrawn, 3 here now
LIBRARY = re.compile(r"library: (\d+) shipped track\(s\), (\d+) withdrawn, (\d+) here now")

# And what the front end says is on the screen afterwards.
TRACE = re.compile(r"trace screen=title .* tracks=(\d+)")

# Two shipped sets. The first two are in both, so what changes between the runs
# is one track withdrawn and one added - which is the shape of a release, and
# the shape that tells "the store won" apart from "the assets won".
BEFORE = ["first-light", "ice-house", "the-oval"]
AFTER = ["first-light", "the-oval", "grey-mile"]

RUN_SECONDS = 60


def stage_assets(source, into, tracks):
    """A copy of the assets with only these tracks in it. Everything else comes
    along because the client wants an icon and a font, not because this cares."""
    os.makedirs(into, exist_ok=True)
    for name in os.listdir(source):
        if name == "tracks":
            continue
        src = os.path.join(source, name)
        dst = os.path.join(into, name)
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            shutil.copy2(src, dst)

    track_dir = os.path.join(into, "tracks")
    os.makedirs(track_dir, exist_ok=True)
    for track in tracks:
        shutil.copy2(os.path.join(source, "tracks", track + ".gstrack"), track_dir)
    return into


def run_client(client, assets, prefs, shot):
    """One start, to the title screen and out again. `--keep` is the consent a
    machine being told what to draw needs before it writes anybody's store."""
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_RENDER_DRIVER"] = "software"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["GEARSTICK_ASSETS_DIR"] = assets
    env["GEARSTICK_PREF_DIR"] = prefs

    out = subprocess.run(
        [client, "--screen", "title", "--shot", shot, "--keep", "--trace"],
        env=env, capture_output=True, text=True, timeout=RUN_SECONDS)
    text = out.stdout + out.stderr
    if out.returncode != 0:
        print(text)
        raise SystemExit(f"the client exited {out.returncode}")
    return text


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: library_refresh_check.py <gearstick> <assets dir>")
    client, assets = sys.argv[1], sys.argv[2]

    for track in set(BEFORE) | set(AFTER):
        if not os.path.exists(os.path.join(assets, "tracks", track + ".gstrack")):
            print(f"{track}.gstrack does not ship any more - this check needs "
                  f"three tracks that do")
            return 77

    with tempfile.TemporaryDirectory() as tmp:
        prefs = os.path.join(tmp, "prefs")
        os.makedirs(prefs)
        shot = os.path.join(tmp, "title.bmp")

        before = stage_assets(assets, os.path.join(tmp, "before"), BEFORE)
        after = stage_assets(assets, os.path.join(tmp, "after"), AFTER)

        # **A player who has played.** The first run is somebody's first, and it
        # leaves a store behind - which is the thing the second run has to get
        # right and the reason this cannot be done in one.
        first = run_client(client, before, prefs, shot)
        lines = LIBRARY.findall(first)
        if len(lines) != 1:
            print(first)
            raise SystemExit(f"expected one library line on a first run, got "
                             f"{len(lines)}")
        shipped, withdrawn, here = (int(v) for v in lines[0])
        assert shipped == len(BEFORE), f"{shipped} shipped, wanted {len(BEFORE)}"
        assert withdrawn == 0, f"{withdrawn} withdrawn on a first run"
        assert here == len(BEFORE), f"{here} in the library, wanted {len(BEFORE)}"

        seen = TRACE.search(first)
        assert seen is not None, "the client never said what was on the screen"
        assert int(seen.group(1)) == len(BEFORE), \
            f"the title screen says {seen.group(1)} tracks, wanted {len(BEFORE)}"

        # **And now the game ships something else.** The store is read first and
        # then reconciled, so the second line is the one that matters: without
        # it there is no second line at all, and the player is looking at the
        # library they were given the first time they ever started the game.
        second = run_client(client, after, prefs, shot)
        lines = LIBRARY.findall(second)
        if len(lines) != 2:
            print(second)
            raise SystemExit(
                "expected the library to be reconciled twice on a second run - "
                "once before the store is read and once after - and it was "
                f"reconciled {len(lines)} time(s). A store that wins over the "
                "shipped set is the bug this check exists for.")

        shipped, withdrawn, here = (int(v) for v in lines[1])
        assert shipped == len(AFTER), f"{shipped} shipped, wanted {len(AFTER)}"
        assert withdrawn == 1, \
            f"{withdrawn} withdrawn, wanted the one track that stopped shipping"
        assert here == len(AFTER), \
            f"{here} in the library, wanted {len(AFTER)} - a library that only " \
            "grows is one that keeps every track every version ever shipped"

        seen = TRACE.search(second)
        assert seen is not None, "the client never said what was on the screen"
        assert int(seen.group(1)) == len(AFTER), \
            f"the title screen says {seen.group(1)} tracks, wanted {len(AFTER)}"

    print(f"library refresh: {len(BEFORE)} shipped, then a release that "
          f"withdrew one and added one, and the player has {len(AFTER)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
