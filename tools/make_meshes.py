#!/usr/bin/env python3
"""Generate the vehicle meshes src/gfx/gs_meshes.c, and their attribution.

**The vehicles are generated, not drawn.** Each one is a handful of boxes with
positions and sizes given here in tiles, welded into one low-poly mesh and
written out as C. Nothing is modelled by hand, nothing is downloaded, nothing
third-party is redistributed, and there is no licence question to answer,
because there is no third-party art in the game at all.

That is not a compromise at this fidelity. The renderer draws cars as geometry
under a fixed isometric camera, and what reads at that size is the silhouette:
a boxy 1985 shape at low poly with flat colours. Six vehicles that must be
distinguishable at a glance and consistent with each other is exactly the job a
parameter table does better than a person with a mouse. Changing every car's
ride height is a number here, not an afternoon.

Colours are not baked. Every triangle carries a *role* - body, trim, glass,
tyre, metal, light - and the renderer decides what a role looks like, which is
what lets four players share one mesh and be told apart by paint.

Re-run after changing anything below and commit the result. CI re-generates and
diffs, so the committed file cannot drift from what this script produces.

    python3 tools/make_meshes.py
"""
import datetime
import pathlib

# The roles a triangle can have. Kept in step with gs_paint in gs_meshes.h.
BODY, TRIM, GLASS, TYRE, METAL, LIGHT = range(6)
ROLE_NAMES = ["BODY", "TRIM", "GLASS", "TYRE", "METAL", "LIGHT"]

# Model space, in tiles: +x is forward, +y is left, +z is up, and the origin is
# on the ground between the wheels. One tile is four metres, and these are
# deliberately not four metres - see gs_render.c on why a car is drawn at about
# 1.3 tiles rather than at the 0.67 an honest scale would give.


def box(cx, cy, cz, sx, sy, sz, role):
    """A box centred at (cx, cy, cz), sized (sx, sy, sz), all six faces one role."""
    return {"c": (cx, cy, cz), "s": (sx, sy, sz), "role": role}


def wheel(cx, cy, r, w):
    """A wheel: a box, because at this size a cylinder is the same silhouette
    and eight more triangles."""
    return box(cx, cy, r, r * 2.0, w, r * 2.0, TYRE)


def wheels(front, rear, track, r, w):
    return [wheel(front, track, r, w), wheel(front, -track, r, w),
            wheel(rear, track, r, w), wheel(rear, -track, r, w)]


# --- the roster -------------------------------------------------------------
#
# Each entry is a list of boxes. The numbers are chosen so that the six read
# apart at a glance from directly above the isometric camera: a silhouette test,
# not an accuracy one.

# Proportion, taken from real cars and scaled to the 1.3 tiles a car is drawn
# at. A saloon is 4.5 m long, 1.8 wide and 1.4 tall on 0.65 m wheels, which is
# length : width : height : wheel of 1 : 0.40 : 0.31 : 0.145. Getting this wrong
# is the single most visible thing about a low-poly vehicle - the first version
# of these had wheels three quarters of the car's height, and every one of them
# read as a monster truck no matter what else was on it.

VEHICLES = [
    ("stock car", [
        box(0.00, 0.0, 0.19, 1.26, 0.54, 0.14, BODY),      # body
        box(-0.06, 0.0, 0.31, 0.60, 0.48, 0.12, BODY),     # cabin
        box(-0.06, 0.0, 0.32, 0.56, 0.50, 0.07, GLASS),    # windows
        box(0.64, 0.0, 0.19, 0.05, 0.36, 0.06, LIGHT),     # headlights
        box(-0.64, 0.0, 0.19, 0.05, 0.34, 0.05, TRIM),     # tail
        box(0.00, 0.0, 0.12, 1.14, 0.50, 0.04, TRIM),      # sills
    ] + wheels(0.42, -0.42, 0.25, 0.10, 0.07)),

    ("sprint car", [
        box(0.00, 0.0, 0.16, 1.30, 0.40, 0.12, BODY),      # long low body
        box(-0.16, 0.0, 0.26, 0.40, 0.38, 0.10, TRIM),     # cockpit surround
        box(-0.16, 0.0, 0.27, 0.36, 0.32, 0.06, GLASS),
        box(-0.56, 0.0, 0.42, 0.24, 0.62, 0.03, METAL),    # the wing
        box(-0.56, 0.26, 0.34, 0.03, 0.03, 0.16, METAL),   # wing stays
        box(-0.56, -0.26, 0.34, 0.03, 0.03, 0.16, METAL),
        box(0.68, 0.0, 0.13, 0.12, 0.28, 0.05, TRIM),      # nose cone
    ] + [wheel(0.46, 0.24, 0.09, 0.06), wheel(0.46, -0.24, 0.09, 0.06),
         wheel(-0.44, 0.26, 0.13, 0.10), wheel(-0.44, -0.26, 0.13, 0.10)]),

    ("dune buggy", [
        box(0.00, 0.0, 0.22, 1.02, 0.48, 0.12, BODY),      # tub
        box(-0.24, 0.22, 0.38, 0.04, 0.04, 0.24, METAL),   # roll hoop uprights
        box(-0.24, -0.22, 0.38, 0.04, 0.04, 0.24, METAL),
        box(-0.24, 0.0, 0.50, 0.04, 0.48, 0.04, METAL),    # hoop top
        box(0.04, 0.0, 0.32, 0.30, 0.36, 0.10, TRIM),      # seats
        box(0.56, 0.0, 0.22, 0.07, 0.28, 0.07, LIGHT),
    ] + wheels(0.44, -0.44, 0.26, 0.14, 0.09)),

    ("baja bug", [
        box(0.02, 0.0, 0.22, 0.98, 0.52, 0.16, BODY),      # the beetle shell
        box(-0.04, 0.0, 0.36, 0.54, 0.46, 0.14, BODY),     # the hump
        box(-0.04, 0.0, 0.37, 0.50, 0.48, 0.08, GLASS),
        box(0.56, 0.0, 0.22, 0.12, 0.40, 0.13, BODY),      # snub nose
        box(-0.58, 0.0, 0.22, 0.12, 0.36, 0.12, METAL),    # exposed engine
        box(0.62, 0.0, 0.25, 0.04, 0.32, 0.06, LIGHT),
    ] + wheels(0.42, -0.42, 0.26, 0.14, 0.10)),

    ("motorcycle", [
        box(0.04, 0.0, 0.22, 0.72, 0.14, 0.11, BODY),      # tank and frame
        box(-0.20, 0.0, 0.29, 0.28, 0.16, 0.06, TRIM),     # seat
        box(0.36, 0.0, 0.33, 0.05, 0.26, 0.04, METAL),     # bars
        box(0.46, 0.0, 0.26, 0.05, 0.11, 0.09, LIGHT),
        box(-0.04, 0.0, 0.39, 0.14, 0.18, 0.20, TRIM),     # rider
        box(-0.04, 0.0, 0.52, 0.12, 0.13, 0.10, GLASS),    # helmet
    ] + [wheel(0.48, 0.0, 0.13, 0.06), wheel(-0.46, 0.0, 0.13, 0.08)]),

    ("lunar rover", [
        box(0.00, 0.0, 0.18, 1.18, 0.62, 0.06, METAL),     # flat deck
        box(-0.24, 0.0, 0.27, 0.38, 0.48, 0.13, BODY),     # instrument box
        box(0.28, 0.0, 0.26, 0.26, 0.40, 0.10, TRIM),      # seats
        box(-0.52, 0.0, 0.40, 0.03, 0.03, 0.24, METAL),    # antenna mast
        box(-0.52, 0.0, 0.53, 0.20, 0.20, 0.02, METAL),    # dish
        box(0.60, 0.0, 0.21, 0.05, 0.24, 0.06, LIGHT),
    ] + [wheel(0.46, 0.32, 0.11, 0.07), wheel(0.46, -0.32, 0.11, 0.07),
         wheel(0.00, 0.34, 0.11, 0.07), wheel(0.00, -0.34, 0.11, 0.07),
         wheel(-0.46, 0.32, 0.11, 0.07), wheel(-0.46, -0.32, 0.11, 0.07)]),
]

# The eight corners of a unit box, and the twelve triangles over them. Wound
# counter-clockwise seen from outside, so the renderer can cull back faces
# rather than sorting every triangle of every car.
CORNERS = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
           (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]

FACES = [
    (4, 5, 6, 7),   # +z
    (1, 0, 3, 2),   # -z
    (5, 1, 2, 6),   # +x
    (0, 4, 7, 3),   # -x
    (7, 6, 2, 3),   # +y
    (0, 1, 5, 4),   # -y
]


def build(boxes):
    """Weld a list of boxes into one indexed mesh."""
    verts = []
    index = {}
    tris = []

    def vertex(p):
        # Rounded before it is looked up, so two boxes that meet exactly share
        # their vertices and the output does not depend on floating point noise.
        key = tuple(round(v, 5) for v in p)
        if key not in index:
            index[key] = len(verts)
            verts.append(key)
        return index[key]

    for b in boxes:
        cx, cy, cz = b["c"]
        sx, sy, sz = b["s"]
        ids = [vertex((cx + x * sx / 2.0, cy + y * sy / 2.0, cz + z * sz / 2.0))
               for (x, y, z) in CORNERS]
        for (a, bb, c, d) in FACES:
            tris.append((ids[a], ids[bb], ids[c], b["role"]))
            tris.append((ids[a], ids[c], ids[d], b["role"]))

    return verts, tris


def cname(name):
    return name.replace(" ", "_")


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    built = [(name, *build(boxes)) for name, boxes in VEHICLES]

    out = ['// Generated by tools/make_meshes.py. Do not edit.',
           '//',
           '// Six vehicles, every one of them generated from the parameters in that',
           '// script rather than modelled by hand or downloaded from anywhere. See it',
           '// for why, and change it rather than this file.',
           '',
           '#include "gfx/gs_meshes.h"',
           '']

    for name, verts, tris in built:
        c = cname(name)
        out.append(f'static const gs_mesh_vertex gs_{c}_vertex[] = {{')
        for (x, y, z) in verts:
            out.append(f'    {{ {x:8.5f}f, {y:8.5f}f, {z:8.5f}f }},')
        out.append('};')
        out.append('')
        out.append(f'static const gs_mesh_tri gs_{c}_tri[] = {{')
        for (a, b, cc, role) in tris:
            out.append(f'    {{ {a:3d}, {b:3d}, {cc:3d}, GS_PAINT_{ROLE_NAMES[role]} }},')
        out.append('};')
        out.append('')

    out.append('static const gs_mesh gs_meshes[] = {')
    for name, verts, tris in built:
        c = cname(name)
        out.append(f'    {{ "{name}", gs_{c}_vertex, {len(verts)}, '
                   f'gs_{c}_tri, {len(tris)} }},')
    out.append('};')
    out.append('')
    out.append('const gs_mesh *gs_mesh_for(uint8_t vehicle) {')
    out.append('    if (vehicle >= (uint8_t)(sizeof gs_meshes / sizeof gs_meshes[0])) {')
    out.append('        return &gs_meshes[0];')
    out.append('    }')
    out.append('    return &gs_meshes[vehicle];')
    out.append('}')
    out.append('')

    (root / "src/gfx/gs_meshes.c").write_text("\n".join(out))

    # --- The attribution, written in the same run as the art it describes.
    #
    # A licence condition is not documentation: if the art is regenerated and
    # the credits are not, the credits are wrong, and the only way to be sure
    # they never are is for one command to produce both.
    total_v = sum(len(v) for _, v, _ in built)
    total_t = sum(len(t) for _, _, t in built)

    lines = [
        "# Attribution",
        "",
        "Generated by `tools/make_meshes.py` in the same run as the art it",
        "describes. Do not edit: re-run the tool.",
        "",
        "## Vehicle meshes",
        "",
        "**Every vehicle in gearstick is generated from parameters in this",
        "repository.** None is modelled by hand, none is downloaded, and none is",
        "third-party. There is no licence condition to satisfy here because there",
        "is no third-party art in the game.",
        "",
        "| Vehicle | Vertices | Triangles |",
        "| --- | --- | --- |",
    ]
    for name, verts, tris in built:
        lines.append(f"| {name} | {len(verts)} | {len(tris)} |")
    lines += [
        f"| **total** | **{total_v}** | **{total_t}** |",
        "",
        "## Ground, surfaces and terrain",
        "",
        "Generated at run time as shaded geometry, tinted by surface and slope.",
        "There is no tile atlas, no terrain texture and no image file of any kind.",
        "",
        "## Everything else",
        "",
        "The trigonometric tables are baked by `tools/make_tables.py` and the",
        "Dear ImGui C bindings by `tools/make_imgui_bindings.py`. Third-party",
        "*code* is listed in `ext/README.md`, pinned as submodules, and is not",
        "redistributed by this repository.",
        "",
    ]
    (root / "assets/ATTRIBUTION.md").write_text("\n".join(lines))

    print(f"wrote src/gfx/gs_meshes.c: {len(built)} vehicles, "
          f"{total_v} vertices, {total_t} triangles")
    print("wrote assets/ATTRIBUTION.md")


if __name__ == "__main__":
    main()
