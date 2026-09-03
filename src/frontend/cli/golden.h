// golden.h - the state hash the selftest race is required to end on.
//
// **This number changing is not a test being annoying.** It means the physics
// moved, and every ghost time, every shared replay and every track anyone has
// tuned against is now wrong. Updating it is a deliberate act with a note in
// docs/PROJECT_STATUS.md saying what changed and why it was worth it.
//
// Print the current value with:  gearstick_cli selftest
#ifndef GS_GOLDEN_H
#define GS_GOLDEN_H

// Moved once, deliberately, when gates were added: a track's identity now
// includes its route, because the same ground driven the other way round is a
// different track and its times are not comparable. The *world* hash below did
// not move, which is the point - the physics was untouched, and that is exactly
// what these two numbers being separate is for.
//
// **And a second time, for the other half of that same sentence.** The hash
// covered the gates and not whether they make a loop or a path - so a circuit
// and a sprint over exactly the same ground were one track. The library is
// content addressed, so saving one beside the other renamed the first entry and
// threw the second away: somebody's work, gone, silently. Records keyed on this
// pooled a lap of a loop with a run from end to end, which is two times that
// cannot be put next to each other.
//
// The world hash did not move again. Nothing about the physics changed; what
// changed is what counts as the same track.
//
// A share code carries this number so a damaged code fails loudly, and one went
// out with v0.1.0-beta1, so those codes would have stopped opening. They do not:
// the reader accepts the answer this used to give as well. See
// gs_track_hash_before_route_kind.
#define GS_SELFTEST_TRACK_HASH 0x483dd875662890aeULL
// Moved twice more since, both times deliberately and both times because the
// race really did change: the grip circle, and then the roster retune that
// followed from it. A vehicle's numbers are inputs to the physics, so changing
// them changes every recorded replay - which is why this number is allowed to
// move during tuning and is not allowed to move by accident.
//
// And again when laps gained a clock: a best lap is what a track is judged by,
// so it is simulation state rather than something a front end watches for.
//
// And when a race gained a finish line: a lap target on the world and a finish
// tick on each car, both hashed, because a results screen that shows times
// needs the times to be part of the state two machines have to agree about.
//
// And when ground too steep to climb became a wall rather than a catapult -
// found by the analyser calling an impassable track completable.
//
// And again for car-to-car collision, which cars in the selftest race now have
// with each other.
//
// And once more for surface wear, which is new state that changes the race:
// dirt churns and loses grip on the line everyone takes, ice polishes into
// something faster and looser, pavement is unmoved.
//
// The grip circle, in detail: engine force is now
// capped by the traction available, so tyres matter off the line and not only
// in corners, and low gravity takes your acceleration along with your weight.
// Every replay and ghost time recorded before that is invalid, which is what
// this number failing is for. See docs/PROJECT_STATUS.md.
// And when a race gained weapons. Four things a driver can leave behind rather
// than one, a count of each on every car, which one a tap would drop, and the
// loadout the race arms everybody with - all of it state that decides a race,
// so all of it hashed. A race with the weapons turned off is every car carrying
// zero of everything and behaves exactly as every race did before, which is why
// the *track* hash did not move and the cars in the selftest race end up in the
// same places: what moved is the number that describes them, because there is
// more of them to describe.
//
// It moved twice while that landed - once for what a car carries and once for
// what the race arms it with - which is one feature and should have been one
// move. The second is here rather than hidden because a number like this one is
// worth more than a tidy history.
//
// And once more when the world stopped going on forever. There is now a run-off
// outside a track and a drop past it, so the ground a car finds off the edge is
// sand at the edge's height for ten tiles and then falling away - where before
// it was the edge tile's own surface, level, without end. The selftest race has
// a car that leaves the track, so its hash moves. See docs/PROJECT_STATUS.md.
// **And the world hash moved, once, when the world got bigger.**
//
// GS_TRACK_MAX went from 64 tiles to 192, because no route folded into a 64 by
// 64 field is longer than about five hundred tiles and every default track was
// therefore a twenty-seven second drive. The physics is untouched; what changed
// is the size of the arrays the physics indexes. Surface wear is stored per
// tile in an array of GS_TRACK_TILES, and a point off the edge of the track is
// clamped into it by GS_TRACK_MAX rather than by the track's own width - so a
// car that leaves the ground the selftest race puts it on (it ends at y = -1.08,
// which is off the track by a tile) now reads a different wear cell than it did.
// Same physics, different cupboard.
//
// The track hash above did not move: a track's identity is its own contents and
// has never depended on how big a track is allowed to be.
// **And the world hash moved, deliberately, when the cars got quicker.**
//
// The game felt sluggish against the one it is after, so power, top speed and
// grip are half as much again on every machine, and toughness with them so that
// half as much speed again does not simply break everything on the first
// landing. That is the physics of every car in every race, so every replay,
// ghost and shared time recorded before it is a race nobody can drive now -
// which is what this number failing is for. See docs/PROJECT_STATUS.md.
#define GS_SELFTEST_WORLD_HASH 0x0550a8a6ccd255d5ULL

// The track generator, folded over its first two hundred seeds.
//
// **A generated track is identified by its seed**, so two people typing the same
// number have to get the same ground - across compilers and across platforms,
// exactly like the physics. This number caught the reason it needs to exist: two
// RNG draws sitting in one argument list are two draws in an order C does not
// define, and gcc and clang chose differently. Changing the generator's output
// deliberately means moving this number and saying so; changing it by accident
// means somebody's shared seed no longer names the track they meant.
// Moved once, deliberately, when the surfaces went from three to nine: the
// generator picks the ground it builds on, so six more grounds is six times as
// many things it can pick and every seed lands somewhere new. Nobody had shared
// a seed yet. The *world* hash below did not move, because the three original
// surfaces kept their numbers and their physics - which is what appending to
// that enum rather than renumbering it is for.
//
// Moved a second time, deliberately, when the tracks became raceable. The
// generator used to lay two gates on every shape it built - one near the left
// edge, one near the right - and the simulation counted a lap when the last
// gate was crossed, so a "lap" was a one-way trip and lap two meant driving
// back across an open field with nothing marking the way. A player put it
// plainly: the track does not seem to go anywhere.
//
// Every seed now builds either a loop or a path, the route is *carved* into the
// ground rather than dropped on top of it, and the road has its own surface so
// it can be seen. Every seed therefore lands somewhere new, and every seed
// anybody had shared names a different track. That is the cost and it was worth
// paying: what the old seeds named was not raceable.
//
// Moved a third time, in the same breath, because the tracks that came out of
// the second attempt still could not be finished. A gate is finite across its
// line, and the generator was laying gates narrower than the road it had just
// carved - so a car keeping to the outside of its own road went *past* a
// checkpoint without crossing it, and because gates count in order the finish
// line then did nothing when it was reached. A player drove over the finish and
// the game did not notice. Gates are wider than the road now, and
// `every_gate_is_wider_than_the_road_it_crosses` keeps them that way.
//
// The *world* hash below did not move. The physics is untouched by any of this.
//
// **Moved a fourth time, and this one names nothing.** This number is a fold of
// `gs_track_hash` over the first two hundred seeds, and that function changed -
// a track's identity now says whether it is a loop or a path. The generator did
// not change. **Every seed builds exactly the same ground it built before**; the
// number that names that ground is different, which is what the fold measures.
// So unlike the three moves above, nobody's shared seed opens a different track,
// and the warning this failure prints is worth reading with that in mind.
// **The generator fold moved deliberately, with the routes.** It is a fold of
// gs_track_hash over two hundred seeds and names nothing on its own; it moves
// whenever the generator makes different ground, which it now does: every seed
// builds a serpentine of about a thousand tiles on a field of a hundred and
// eighty rather than a fifty-tile arc on a field of sixty. That was the point of
// the change - a default track that takes twenty-seven seconds to drive is not
// a race - and it is what a share code for a generated track will now open as.
// **And it moved once more when the circuits learned to close.**
//
// Every loop the generator had ever built ended with a 157 degree reversal at
// about a tile radius, immediately before the start line - a corner no car can
// take, sitting exactly where the lap is counted. The closing arc was centred
// on the route's *start point* rather than on the midpoint between the route's
// two ends, so it always finished a radius away from where it had to arrive and
// the lap wrapped across the gap left over.
//
// The closure is a single half-circle now, and the sharpest bend on any
// generated circuit is 46.8 degrees at 13.8 tiles.
// `no_generated_route_turns_tighter_than_its_own_hairpin` walks 2,073 corners
// over every shape the generator makes and fails on anything over 90.
//
// So every seed builds different ground again, and a seed anybody shared names
// a different track. The physics is untouched - the world hash above moved for
// the roster, not for this - and what these seeds named before was a circuit
// with an unnavigable corner on it.
// **And once more when the routes stopped all being the same shape.**
//
// There were four `gs_track_shape`s from the start and not one of them reached
// the route planner: they choose the terrain and the surfaces, and every track
// in the game came out as the identical serpentine of horizontal passes.
// Eighteen shipped tracks, 83 to 92 gates each, 188 to 260 seconds each, and
// the same picture on every minimap. A player said it: "every track can't be
// the same shape".
//
// The planner has layouts now and the seed picks one - the serpentine as it
// was, or the same turned a quarter so its passes run north and south. That is
// two silhouettes where there was one, and the set it produces runs from 55 to
// 92 gates and 131 to 250 seconds where it used to run from 83 to 92 and 188 to
// 260. Every seed therefore builds different ground, which is what this number
// is for saying out loud.
//
// **And a circuit stopped being a fold at all.** The longest closed curve that
// fits a field under two hundred tiles across is four hundred tiles, and the
// stock floor is six hundred and thirty - so a circuit could only ever meet it
// by folding, and anything folded six times in a square field is a serpentine.
// That was the whole reason every track looked alike, and it was arithmetic
// rather than an oversight.
//
// A circuit is a closed curve now, drawn in polar form about the middle of the
// field, three hundred-odd tiles round and driven three times. The floor is a
// floor on the *race* rather than on the route - see gs_track_race_length - so
// what it was written to guarantee is unchanged and what it was accidentally
// dictating is gone. Paths still fold, because a path is driven once and all of
// its length has to be in the route.
//
// The layout is drawn from the same generator as everything else about a track,
// so it consumes a draw and every seed's terrain moves with its shape.
// **And it moved for the matrix, which retired the shapes altogether.**
//
// The four terrain shapes and the layout planner are gone. A track is now a
// draw from ten dials - class, length, curviness, straightness, jumps,
// relief, relief range, gravity, dress, road width - and the route is grown
// by a biased self-avoiding walk over a cell grid rather than laid from a
// template, so how a track folds is an outcome of the dice. Every seed
// therefore builds entirely different ground, and every seed anybody shared
// names a different track: asked for in so many words - "you need to have two
// classes of tracks... the path needs to be reasonably randomized - the
// tracks should be organic" - and worth exactly this cost. The dials are
// asserted one by one in the suite, each measured on the track that comes
// out. The world hash above did not move; the physics is untouched.
#define GS_SELFTEST_GENERATOR_HASH 0xea105006e4f627b5ULL

// **A race with nobody at the keyboard**, four opponents spread across the
// skill dial, on a circuit none of them has seen.
//
// This one moves for two reasons rather than one: the physics, like the number
// above it, and *the driver*. An opponent is a pure function of the world, so a
// change to how it decides where to lift or which way round a wall to go is a
// change to every race anybody has recorded against it - and unlike a scripted
// input log, there is nothing else holding the race in place. That is exactly
// why it is pinned.
//
// Moved with the world hash above when weapons went in, and for the same
// reason: a car carries more state than it did. No opponent has ever dropped
// anything - gs_ai_drive does not press the button - so the race itself is
// unchanged, and that is a gap written down in docs/PROJECT_STATUS.md rather
// than a thing this number is hiding.
// Moved with the one above, and for the same reason: a bigger world means a
// different wear cell for a car that has left the track, and the opponents race
// puts one there too. The driver and the physics are both untouched.
//
// **And moved once more when the grid became an echelon.**
//
// Four cars used to start abreast and level with each other. Two seconds after
// the flag they are all steering for the same racing line, still level, and a
// car in the middle can be struck from both sides inside one tick: the two
// horizontal impulses partly cancel, the lift each one adds cannot, and the car
// is thrown nearly five tiles up and ten tiles backwards - off the top of the
// field, wrecked, having driven no laps. Four AI cars over the shipped set lost
// **7 of 72** that way, on six of the eighteen tracks, and it took all four
// slots filled, which is the only grid anybody races.
//
// So each slot now starts a tile and a half further back than the one beside
// it, and no two cars are level to be squeezed between. The physics is
// untouched - not one constant of it moved - and so is the driver. **What moved
// is where the cars are put**, and the opponents race is four cars put on a
// grid, so its hash moves with them. See docs/PROJECT_STATUS.md.
// And with it when the roster got quicker, for the plainest reason of all: the
// cars in this race are half as fast again as the ones the old number was taken
// from.
#define GS_OPPONENTS_WORLD_HASH 0x9d61c6f26883878eULL

#endif // GS_GOLDEN_H
