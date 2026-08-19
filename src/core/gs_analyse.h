// gs_analyse.h - answering questions about a track without racing it yourself.
//
// **Nobody in 1985 could tell you whether their track was any good.** They could
// drive it, and that was all. A deterministic simulation that links nothing can
// be run a few hundred times in the time it takes to drive once, so this asks
// the questions a designer actually has:
//
//   - can it be got round at all?
//   - between which gravities? by which machines?
//   - and where does everybody actually go?
//
// The first two are the envelope. The third is the heatmap, and it is the one
// that changes how a track gets built: the racing line stops being a thing you
// guess at and becomes a thing you can see.
#ifndef GS_ANALYSE_H
#define GS_ANALYSE_H

#include "core/gs_ai.h"
#include "core/gs_sim.h"
#include "core/gs_track.h"

// Gravities tried, from very light to heavier than Jupiter.
#define GS_ANALYSIS_STEPS 9

typedef struct gs_analysis {
    gs_fix  gravity[GS_ANALYSIS_STEPS];
    uint8_t completed[GS_ANALYSIS_STEPS];   // how many vehicles got a lap in

    bool    completable;    // anybody, anywhere in the range
    gs_fix  lightest;       // the envelope: the range of gravity that works
    gs_fix  heaviest;

    // How often a car was on each tile, across every run. Big numbers are the
    // line everybody takes.
    uint16_t visits[GS_TRACK_TILES];
    uint16_t busiest;
} gs_analysis;

// Race every vehicle at every gravity for `seconds` each. Deterministic, so the
// same track always gives the same answer - a design tool that disagreed with
// itself between runs would be worse than none.
void gs_analyse(const gs_track *t, uint32_t seconds, gs_analysis *out);

// How busy a tile was, 0 to GS_ONE, for drawing.
gs_fix gs_analysis_heat(const gs_analysis *a, uint8_t x, uint8_t y);

#endif // GS_ANALYSE_H
