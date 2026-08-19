#!/usr/bin/env python3
"""Bake the C bindings for Dear ImGui into src/ui/dcimgui/.

Dear ImGui is C++. `ext/dear_bindings` generates a C API over it, and this
script runs that generator for the two backends gearstick actually uses - SDL3
and SDL_Renderer3 - and nothing else. The upstream BuildAllBindings.sh emits
nineteen backends, eighteen of which would be dead weight in the tree.

**The output is committed**, for the same reason the trigonometric tables are:
so that a clone builds with a C compiler and nothing else. Generating at build
time would put Python and `ply` between every contributor and a working build,
and this file changes when the ImGui pin changes - a few times a year at most.

The generator also writes .json metadata describing the API, which exists so
that *other* languages can build their own bindings. It is 8.5 MB and nothing
here reads it, so it is deleted rather than committed.

Needs `ply` (pip install ply==3.11) and the submodules:

    git submodule update --init --depth 1 ext/imgui ext/dear_bindings
    python3 tools/make_imgui_bindings.py

CI re-bakes and diffs, so the committed bindings cannot drift from the pins.
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMGUI = ROOT / "ext" / "imgui"
BINDINGS = ROOT / "ext" / "dear_bindings"
OUT = ROOT / "src" / "ui" / "dcimgui"

# The only two backends this game has any use for: SDL3 for the platform layer
# and SDL_Renderer3 for drawing, matching the renderer the game already uses.
BACKENDS = ["sdl3", "sdlrenderer3"]


def die(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def run(args):
    result = subprocess.run(args, cwd=BINDINGS, capture_output=True, text=True)
    if result.returncode != 0:
        die("dear_bindings failed:\n" + result.stdout + result.stderr)


def main():
    if not (IMGUI / "imgui.h").exists():
        die("ext/imgui is empty - run:\n"
            "  git submodule update --init --depth 1 ext/imgui ext/dear_bindings")
    if not (BINDINGS / "dear_bindings.py").exists():
        die("ext/dear_bindings is empty - run:\n"
            "  git submodule update --init --depth 1 ext/imgui ext/dear_bindings")
    try:
        import ply  # noqa: F401
    except ImportError:
        die("dear_bindings needs ply:  python3 -m pip install --user ply==3.11")

    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "backends").mkdir(parents=True)

    print("generating dcimgui from ext/imgui")
    run([sys.executable, "dear_bindings.py", "-o", str(OUT / "dcimgui"),
         str(IMGUI / "imgui.h")])

    for backend in BACKENDS:
        print("generating backend " + backend)
        run([sys.executable, "dear_bindings.py", "--backend",
             "--include", str(IMGUI / "imgui.h"),
             "--imconfig-path", str(IMGUI / "imconfig.h"),
             "-o", str(OUT / "backends" / ("dcimgui_impl_" + backend)),
             str(IMGUI / "backends" / ("imgui_impl_" + backend + ".h"))])

    # The metadata is for other languages' generators; nothing here reads it.
    removed = 0
    for junk in OUT.rglob("*.json"):
        junk.unlink()
        removed += 1

    kept = sorted(p.relative_to(ROOT) for p in OUT.rglob("*") if p.is_file())
    total = sum((ROOT / p).stat().st_size for p in kept)
    print("\nwrote %d files, %.0f KB (dropped %d .json)" % (len(kept), total / 1024, removed))
    for p in kept:
        print("  %s" % p)


if __name__ == "__main__":
    main()
