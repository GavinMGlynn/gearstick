// gs_ai.h - somebody to race against.
//
// **It plans, it does not follow.** There is no baked line and no recorded lap:
// every tick it looks at where the next gate is, works out how fast it could go
// round the corner that implies given the grip it actually has *here and now* -
// this surface, this gravity, these tyres, this much wear - and drives
// accordingly.
//
// That matters because gravity is a race parameter and a paintable one. A
// speed profile computed when the track was authored would be wrong the moment
// somebody moved the dial, and wrong in the most dangerous possible way: it
// would look right.
//
// Integer throughout and a pure function of the world, so an AI car is
// deterministic exactly as a human one is, and a replay of a race against it
// re-races to the bit.
#ifndef GS_AI_H
#define GS_AI_H

#include "core/gs_sim.h"
#include "core/gs_track.h"

// **Skill is a dial, not three names.**
//
// What separates two drivers here is how near the limit they are willing to
// run: the fraction of the speed the grip would actually bear that they will
// ask for. Under one for everybody, because the estimate is a chord
// approximation and a driver exactly at the limit is a driver about to be over
// it.
//
// That is the whole of "difficulty". Not rubber-banding, not extra power, not
// cheating on grip - the same thing that separates two people. And it is
// continuous for the same reason gravity is: three names is a dropdown, and a
// dropdown is somebody else deciding which three points on a line you are
// allowed to stand on.
#define GS_AI_SKILL_STEPS   20      // 0 to 20 inclusive, so twenty-one settings
#define GS_AI_SKILL_DEFAULT 12

// **What the dial actually changes, which is not one number.**
//
// A dial that only scales the top speed is a handicap, not a driver. What
// separates two people over the same corner is where they lift, how much of the
// grip they ask for once they are in it, and how precisely they hold the line -
// and all three move together, because they are the same confidence.
typedef struct gs_ai_style {
    // How much of the speed the grip would bear they will ask for. Under one
    // for everybody: the estimate is a chord approximation, and a driver exactly
    // at the limit is a driver about to be over it.
    gs_fix   margin;

    // **Where they lift.** The physics says how much road it takes to shed the
    // difference; this is how much more than that they want to see before they
    // believe it. A cautious driver brakes a third early and arrives at the
    // corner having already finished slowing down; a quick one brakes at the
    // last moment the sum allows.
    gs_fix   brake_early;

    // **How straight they hold it.** Below this much error the wheel stays
    // where it is. Wide is a driver who lets the car wander and corrects late,
    // which costs distance on a straight and lines on a jump; narrow is one who
    // keeps it pointed. Too narrow and it hunts, which is why nobody gets zero.
    gs_angle deadband;
} gs_ai_style;

// What a driver at this setting drives like. Integer arithmetic on the way
// through, like everything else in here.
gs_ai_style gs_ai_skill_style(int skill);

// Just the grip fraction of it, for the places that only want the number.
gs_fix gs_ai_skill_margin(int skill);

// What this car would press, this tick, driving at the middle of the dial.
gs_input gs_ai_drive(const gs_world *w, const gs_track *t, uint8_t car);

// The same, driving in a stated style.
gs_input gs_ai_drive_style(const gs_world *w, const gs_track *t, uint8_t car,
                           gs_ai_style style);

// The same, at a stated pace and otherwise driving like the middle of the dial -
// for the sweeps that vary the grip fraction and nothing else.
gs_input gs_ai_drive_at(const gs_world *w, const gs_track *t, uint8_t car,
                        gs_fix margin);

#endif // GS_AI_H
