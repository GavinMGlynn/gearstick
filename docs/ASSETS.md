# Assets: where they come from, and how they are pinned

**What this file is.** The provenance rules and the sourcing plan. Nothing here
is built yet — the art pipeline is Phase 10 — but the decisions are made and
written down, because the alternative is discovering at Phase 10 that half the
roster has an unclear licence.

`FEATURES.md` says what the game should be; this says where its raw material
comes from and under what terms.

---

## The rules

1. **Nothing third-party is redistributed by this repository.** A clone gets
   URLs and hashes, not sources. Generated output is committed so that a clone
   builds and plays without fetching any of it.
2. **Anything that is a git repository is a pinned submodule under `ext/`.**
   Build-time only, never linked, never shipped, pinned to a tag or a SHA.
3. **Anything that is not a git repository is pinned by content hash.** A
   manifest of URL plus SHA-256, fetched by a tool into a gitignored directory.
   That gives the same guarantee a submodule does — the exact bytes are
   recorded, and a changed upstream is an error rather than a silent
   difference. Pinning provenance to somebody's unofficial mirror of an asset
   pack would be *worse* than not pinning it, so mirrors are not used.
4. **Attribution is generated in the same run as the art.** A licence condition
   is not documentation. `assets/CREDITS.md` moves with the pipeline or not at
   all.
5. **Licences are recorded per asset, at the point of use**, not assumed from
   the site it came from. CC0, MIT and OFL are fine; anything with a
   share-alike or non-commercial term is not used.

---

## Vehicles — 3D meshes, not sprites

The renderer draws cars as real geometry under the fixed isometric camera. See
`PROJECT_STATUS.md`, *Cars are meshes*, for why: terrain is per-corner heights,
so a car's orientation is continuous in heading, pitch and roll, and a sprite
atlas would have to quantise all three.

That makes the art requirement small. A boxy 1985 silhouette at low poly with
flat vertex colours needs no textures at all.

**Where the models come from, in order of preference:**

| Source | What | Licence | Pinned by |
| --- | --- | --- | --- |
| kenney.nl *Car Kit* (~45 models) | sedans, vans, trucks, wheels, debris | CC0 | manifest — it is a zip, not a repo |
| kenney.nl *Racing Kit* | racing bodies, track dressing | CC0 | manifest |
| `KenneyNL/Starter-Kit-Racing` | motorcycle, four trucks, engine/impact/skid audio | MIT | **submodule** when used |
| Modelled here in Blender | anything the kits do not cover — a baja bug, a lunar rover | ours | in-repo source |

The kits carry the bulk and are not git repositories, which is why rule 3
exists. `KenneyNL/Starter-Kit-Racing` is the one official Kenney *repo* and is a
Godot project holding five vehicles; it earns a submodule if and when its
motorcycle or its audio is actually used, and not before.

Modelling the missing vehicles ourselves is genuinely viable at this fidelity —
a baja bug or a lunar rover is an evening's work as a low-poly silhouette, and
it sidesteps the licence question entirely.

**The pipeline:** model (glTF or OBJ) → `tools/make_mesh.py` → a compact
committed mesh of positions, normals and flat colours → loaded and projected by
`src/gfx/`. No runtime glTF parser, no image decoder, no atlas.

## Ground and surfaces — generated, not sourced

There is no tile atlas and there are no terrain textures. The ground is emitted
as shaded geometry tinted by surface and by slope, which is what lets arbitrary
elevation joins stitch by construction and what makes painted gravity and
progressive surface wear another input to the tint rather than new art.

If a surface ever wants a texture over the shading, CC0 material sources exist
(Poly Haven, ambientCG); both serve over an API rather than git, so both fall
under rule 3. Neither is needed yet.

## Backgrounds — there are none

The camera looks down at a solid ground plane. What is behind it is a clear
colour. A skybox would be an odd fit for a fixed isometric camera and is not
planned.

## Sound — synthesised, not sampled

The engine note tracks the drivetrain, so it wants to be generated rather than
looped: a sample pitched up and down is exactly the thing that stops sounding
like a car. SDL3 loads and converts WAV without help, so there is no audio
library and no sample library to source.

`KenneyNL/Starter-Kit-Racing` carries CC0-adjacent engine, impact and skid
audio under MIT, which is useful as a *reference* for what these should sound
like even if nothing is shipped from it.

## The font — a real dependency, unresolved

The HUD needs one, and it is the one asset class with no answer yet. It wants a
pixel font under OFL or CC0 that is a git repository, so rule 2 applies
cleanly. Several exist; none has been chosen, and the choice is not urgent
until there is a HUD to put it in.

The C64's own font ROM is not an option — it is not ours to ship — but the
VIC-II palette is just sixteen colour values, and the Pepto/Colodore derivation
of them is a reference anyone can reimplement.

## Tracks — ours

The fifty stock tracks that shipped with *Racing Destruction Set* are EA's.
They are worth studying: the manual's designer notes explain the intent behind
tracks like `headon`, built to aim both drivers at each other on pavement so
collisions happen at speed, and `destruct`, the shortest buildable track and
fully elevated so you do not have to go far to find someone to hit. That is
design rationale from the authors and it costs nothing to learn from.

**Calibrate against them; ship our own.** Gearstick's tracks are authored in
gearstick's editor.

For validating the editor and the physics against a large body of real
human-authored tracks, the *Stunts* / *4D Sports Driving* corpus is the better
target anyway: the `.trk` format is fully documented, grid-based with elevation
and three surface types, and decades of community tracks exist.
`duplode/stunts-cartography` is a git repository carrying `.TRK` files and
earns a submodule under rule 2 if the validation work happens. Its licence is
unclear from the repository metadata and must be checked before use — which is
rule 5 doing its job.

---

## Status

**Nothing above is fetched, vendored or committed.** `assets/` is empty and
`ext/` holds SDL alone. This file is the plan, and it is written now so that
Phase 10 is a matter of executing it rather than relitigating it.
