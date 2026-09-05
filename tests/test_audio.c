// test_audio.c - what the race actually sounds like, measured.
//
// "Listened to on three platforms" is the verification in the plan and it is
// the right one, but it is not a thing CI can do, and a synthesiser that has
// gone silent or gone to a constant tone will pass a listening test that nobody
// remembers to run. So the signal itself is measured: level, and pitch by
// counting zero crossings. Both are properties of the samples that come out of
// the same function the device callback calls, so what is checked here is what
// comes out of the speakers rather than a description of it.
#include "audio/gs_audio.h"
#include "audio/gs_music.h"
#include "core/gs_track.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include "gs_sandbox.h"

static int gs_failures = 0;
static const char *gs_current = "";

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s\n    %s:%d: %s\n", gs_current, __FILE__,         \
                   __LINE__, #cond);                                           \
            gs_failures++;                                                     \
        }                                                                      \
    } while (0)

#define TEST(name)                                                             \
    static void name(void);                                                    \
    static void run_##name(void) { gs_current = #name; name(); }               \
    static void name(void)

#define FRAMES 16384
static float gs_buf[FRAMES * GS_AUDIO_CHANNELS];

static float gs_rms(const float *b, int frames) {
    double sum = 0.0;
    for (int i = 0; i < frames * GS_AUDIO_CHANNELS; i++) sum += (double)b[i] * (double)b[i];
    return (float)SDL_sqrt(sum / (double)(frames * GS_AUDIO_CHANNELS));
}

static inline float gs_f(gs_fix v) { return (float)v / (float)GS_ONE; }

// Zero crossings on the left channel. Not a pitch meter - there is broadband
// tyre noise in this signal and noise crosses zero constantly - but it is a
// perfectly good *brightness* meter, which is what tells a rumble from a hiss.
static int gs_crossings(const float *b, int frames) {
    int n = 0;
    for (int i = 1; i < frames; i++) {
        float a = b[(i - 1) * 2], c = b[i * 2];
        if ((a <= 0.0f && c > 0.0f) || (a > 0.0f && c <= 0.0f)) n++;
    }
    return n;
}

// The fundamental, by *normalised* autocorrelation - the standard one, because
// the unnormalised kind does not work here and the two ways it fails are worth
// recording. Raw correlation is biased by how much energy happens to sit under
// each lag, so it reported the sixth multiple of the true period at speed and
// an octave down at a crawl; and a signal that repeats every N samples also
// repeats every 2N, so even an unbiased peak-pick lands on a subharmonic
// roughly half the time.
//
// Normalising by the energy in both windows puts every lag on the same [-1, 1]
// scale, and taking the *first* peak that clears a fraction of the best one
// takes the shortest period that explains the signal - which is the
// fundamental, since a stack of harmonics does not repeat at half its period.
static float gs_pitch(const float *b, int frames) {
    const int lo = 24, hi = 2400;      // 2 kHz down to 20 Hz at 48 kHz
    int window = frames - hi - 1;
    if (window < 4000) return 0.0f;

    static float r[2401];
    float best = 0.0f;

    for (int lag = lo; lag <= hi; lag++) {
        double num = 0.0, ea = 0.0, eb = 0.0;
        for (int i = 0; i < window; i += 2) {
            double x = (double)b[i * 2];
            double y = (double)b[(i + lag) * 2];
            num += x * y;
            ea += x * x;
            eb += y * y;
        }
        double den = SDL_sqrt(ea * eb);
        r[lag] = den > 1e-12 ? (float)(num / den) : 0.0f;
        if (r[lag] > best) best = r[lag];
    }
    if (best < 0.2f) return 0.0f;      // nothing periodic in here

    // The first peak worth believing.
    for (int lag = lo + 1; lag < hi; lag++) {
        if (r[lag] < best * 0.85f) continue;
        if (r[lag] < r[lag - 1] || r[lag] < r[lag + 1]) continue;   // a peak
        return (float)GS_AUDIO_RATE / (float)lag;
    }
    return 0.0f;
}

static gs_track gs_t;

// Settle the chased values - the master fade, the engine's inertia - so what is
// measured is the steady state rather than the ramp into it.
// **In the gear the simulation would have put it in.** The engine note is
// derived from the car's gear, and the gear is simulation state - so a scene
// that pokes a velocity in and never steps the world leaves every car in
// first whatever speed it is doing, and the gearbox cannot be heard at all.
// Stepping instead would drag wear and collision into scenes that are about
// neither, so the gear is set the way an automatic sets it, through the same
// function the simulation uses.
static void gs_settle(const gs_world *w, const gs_track *t) {
    float lx = w->car_count > 0 ? gs_f(w->car[0].x) : 0.0f;
    float ly = w->car_count > 0 ? gs_f(w->car[0].y) : 0.0f;
    for (int i = 0; i < 120; i++) {
        gs_audio_update(w, t, lx, ly);
        gs_audio_render(gs_buf, FRAMES);
    }
}

// The speed a scene is about, with the gearbox told about it. The gear comes
// from the forward component, which is what a gearbox is geared to.
static void gs_at_speed(gs_world *w, gs_fix vx, gs_fix vy) {
    if (w->car_count == 0) return;
    w->car[0].vx = vx;
    w->car[0].vy = vy;
    w->car[0].gear = gs_gear_auto(gs_vehicle(w->car[0].vehicle), vx);
}

static void gs_scene(gs_world *w, gs_surface surface) {
    gs_track_init(&gs_t, 32, 16, surface);
    for (uint8_t y = 0; y <= gs_t.h; y++)
        for (uint8_t x = 0; x <= gs_t.w; x++) gs_track_set_corner(&gs_t, x, y, 0);

    gs_world_init(w, GS_ONE);
    gs_world_add_car(w, &gs_t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(16), GS_INT(8), 0);
}

TEST(a_race_makes_a_noise_and_an_empty_world_does_not) {
    gs_world w;
    gs_scene(&w, GS_SURF_PAVEMENT);

    // Nothing in the world at all.
    w.car_count = 0;
    gs_settle(&w, &gs_t);
    CHECK(gs_rms(gs_buf, FRAMES) < 0.0001f);

    // One car, moving.
    gs_scene(&w, GS_SURF_PAVEMENT);
    gs_at_speed(&w, GS_INT(6), 0);
    gs_settle(&w, &gs_t);
    CHECK(gs_rms(gs_buf, FRAMES) > 0.01f);

    // And it is not a constant: a signal that never crosses zero is a DC
    // offset, which is silence that also breaks speakers.
    CHECK(gs_crossings(gs_buf, FRAMES) > 20);
}

TEST(the_engine_note_follows_the_drivetrain_up_and_down) {
    gs_world w;

    // Idling, then walking pace, then quick. The note has to rise with the
    // first two - and then, because there is a gearbox, *not* simply keep
    // rising: a change drops it back.
    // **Two of these share a gear and two do not**, which is the whole shape
    // of the claim. A stock car holds first gear to about one and a third
    // tiles a second, and the engine idles below about six tenths - so three
    // quarters and five quarters are both inside first and both above idle,
    // where the note is proportional to speed, while seven tiles a second is
    // three gears further up.
    float pitch[4];
    const gs_fix speeds[4] = { 0, GS_RATIO(3, 4), GS_RATIO(5, 4), GS_INT(7) };

    for (int i = 0; i < 4; i++) {
        gs_scene(&w, GS_SURF_PAVEMENT);
        gs_at_speed(&w, speeds[i], 0);
        gs_settle(&w, &gs_t);
        pitch[i] = gs_pitch(gs_buf, FRAMES);
    }

    // A stationary car idles rather than stopping. An engine note that goes to
    // nothing at a standstill is a car that has stalled.
    CHECK(pitch[0] > 20.0f && pitch[0] < 40.0f);

    // Within a gear the note is very nearly proportional to road speed, because
    // in a gear that is exactly what it is. Doubling the speed doubles it.
    CHECK(pitch[1] > pitch[0] * 1.2f);
    CHECK(pitch[2] > pitch[1] * 1.5f);      // five thirds of the speed
    CHECK(pitch[2] < pitch[1] * 1.85f);

    // **And here is the gearbox.** Seven tiles a second is seven times one
    // tile a second, so without a gearbox the note would be seven times
    // higher. It is between two and three times, because the car has changed
    // up three times on the way - which is the entire reason to model a
    // drivetrain rather than to multiply a frequency by a speed.
    CHECK(pitch[3] > pitch[2]);
    CHECK(pitch[3] < pitch[2] * 3.0f);
}

TEST(the_ground_a_car_is_on_changes_what_it_sounds_like) {
    gs_world w;

    // **Every surface there is, not three of them.**
    //
    // This walked pavement, dirt and ice, which were all there were when it was
    // written, and it went on passing when six more went in: sand, gravel,
    // rock, dust, slush and grass all fell through one `default` in
    // gs_surface_voice and **sounded exactly like pavement**. Two thirds of the
    // grounds in a game whose editor lets you paint all nine, and this test -
    // the one whose whole point is knowing what you are driving on without
    // looking - could not see it, because it never asked about them.
    //
    // Walked from GS_SURF_COUNT rather than a list here, so a tenth surface is
    // in this test the day it exists.
    float level[GS_SURF_COUNT];
    int bright[GS_SURF_COUNT];

    for (int i = 0; i < GS_SURF_COUNT; i++) {
        gs_scene(&w, (gs_surface)i);
        // Sideways as well as forwards, so the tyres are working - and in the
        // gear the simulation would have chosen, or every car sits pinned on
        // the limiter in first and the engine drowns the ground.
        gs_at_speed(&w, GS_INT(5), GS_INT(4));
        gs_settle(&w, &gs_t);
        level[i] = gs_rms(gs_buf, FRAMES);
        bright[i] = gs_crossings(gs_buf, FRAMES);

        // Every one of them is something you can hear, before any of them is
        // compared with any other.
        CHECK(level[i] > 0.01f);
    }

    // **No two grounds sound the same.** This is the claim that matters and the
    // one that was false: if two surfaces come out alike, the second of them is
    // a colour in the editor's palette rather than something to drive on.
    int alike = 0;
    for (int a = 0; a < GS_SURF_COUNT; a++) {
        for (int b = a + 1; b < GS_SURF_COUNT; b++) {
            const float apart = level[a] > level[b] ? level[a] - level[b]
                                                    : level[b] - level[a];
            const int sharper = bright[a] > bright[b] ? bright[a] - bright[b]
                                                      : bright[b] - bright[a];
            // Different in loudness, or different in how bright it is, or both.
            // A tenth of the quieter one, and ten zero crossings in a buffer
            // this long, are both well past what a person could argue about.
            if (apart > level[a] * 0.10f || sharper > 10) continue;

            alike++;
            printf("  ALIKE %s and %s: %.4f against %.4f, %d crossings against "
                   "%d\n", gs_surfaces[a].name, gs_surfaces[b].name,
                   (double)level[a], (double)level[b], bright[a], bright[b]);
        }
    }
    for (int i = 0; i < GS_SURF_COUNT; i++) {
        printf("  SURF %-9s rms %.4f  crossings %d\n", gs_surfaces[i].name,
               (double)level[i], bright[i]);
    }
    printf("  SURFACES %d grounds, %d pairs, all of them told apart\n",
           (int)GS_SURF_COUNT, GS_SURF_COUNT * (GS_SURF_COUNT - 1) / 2);
    CHECK(alike == 0);

    // Dirt is the loudest and ice the quietest, which is the whole point of
    // knowing what you are driving on without looking.
    CHECK(level[GS_SURF_DIRT] > level[GS_SURF_PAVEMENT]);
    CHECK(level[GS_SURF_PAVEMENT] > level[GS_SURF_ICE]);
    for (int i = 0; i < GS_SURF_COUNT; i++) {
        CHECK(level[GS_SURF_DIRT] >= level[i]);      // the loudest of all nine
        CHECK(level[GS_SURF_ICE] <= level[i]);       // and the quietest
    }

    // And ice is a hiss where dirt is a rumble, which is the pair this was
    // always about.
    CHECK(bright[GS_SURF_ICE] > bright[GS_SURF_DIRT]);

    // Basalt is the bottom of the range: all rumble and no top end at all.
    for (int i = 0; i < GS_SURF_COUNT; i++) {
        CHECK(bright[GS_SURF_ROCK] <= bright[i]);
    }

    // **The nine are deliberately not ranked by brightness.** Loud and bright
    // are two axes rather than one, and what makes a surface itself is its own
    // pair of them - sand and gravel are thrown against the underside and read
    // sharper than ice does, because there is far more of them. Ice is the
    // quietest thing here and quiet cannot out-hiss loud. Asserting an order
    // across all nine would be inventing a design decision to fit whatever the
    // numbers happened to be; what is asserted instead is that no two of them
    // land in the same place, which is the thing that was actually wrong.
}

TEST(a_car_in_the_air_makes_no_tyre_noise_at_all) {
    gs_world w;

    // On the ground, sliding hard on dirt: about as much tyre noise as there is.
    gs_scene(&w, GS_SURF_DIRT);
    gs_at_speed(&w, GS_INT(6), 0);
    w.car[0].vy = GS_INT(5);
    w.car[0].grounded = true;
    gs_settle(&w, &gs_t);
    float grounded = gs_rms(gs_buf, FRAMES);

    // The same car, the same instant, in the air. The engine is still running -
    // louder, if anything, because nothing is loading it - but the tyres have
    // gone completely.
    gs_scene(&w, GS_SURF_DIRT);
    gs_at_speed(&w, GS_INT(6), 0);
    w.car[0].vy = GS_INT(5);
    w.car[0].grounded = false;
    w.car[0].z = GS_INT(3);
    gs_settle(&w, &gs_t);
    float airborne = gs_rms(gs_buf, FRAMES);

    CHECK(airborne < grounded);

    // Not silent, though: the engine is still there, and a jump that killed all
    // the sound would be a bug rather than drama.
    CHECK(airborne > 0.005f);
}

TEST(a_car_further_away_is_quieter_than_the_one_being_driven) {
    gs_world w;
    gs_scene(&w, GS_SURF_PAVEMENT);
    gs_at_speed(&w, GS_INT(6), 0);

    for (int i = 0; i < 200; i++) {
        gs_audio_update(&w, &gs_t, 16.0f, 8.0f);      // listener on the car
        gs_audio_render(gs_buf, FRAMES);
    }
    float near_level = gs_rms(gs_buf, FRAMES);

    for (int i = 0; i < 200; i++) {
        gs_audio_update(&w, &gs_t, 40.0f, 30.0f);     // and a long way off
        gs_audio_render(gs_buf, FRAMES);
    }
    float far_level = gs_rms(gs_buf, FRAMES);

    CHECK(far_level < near_level * 0.5f);
}

// The loudest the buffer got. An rms over a long buffer hides a short bang, and
// some of what is measured here - a mine going off - is exactly that.
static float gs_peak(const float *buf, int frames) {
    float peak = 0.0f;
    for (int i = 0; i < frames * GS_AUDIO_CHANNELS; i++) {
        const float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    return peak;
}

TEST(a_race_sounds_the_same_loudness_on_every_platform) {
    // **The claim the sound item rests on, and nothing checked it.**
    // *"The synthesiser is platform-independent and the device path is not"* -
    // and every one of the fourteen tests around this one asserts a *relative*
    // property. Dirt louder than pavement, ice brighter than dirt, a mine
    // louder than laying one. A platform whose whole output came out at a tenth
    // of the level, or ten times it, would pass every single one of them.
    //
    // So one absolute number, pinned, from a fixed race. It runs on all three
    // platforms in CI, which is the only way this gets asked of Windows and
    // macOS at all.
    //
    // **A band rather than a value.** This is float arithmetic - `sinf` and a
    // filter - and the last bits of it are a compiler's business, which is
    // exactly why src/core/ forbids floats and why sound is allowed to use
    // them: nothing downstream of the mixer has to agree with anything. A
    // tenth either way is far tighter than a platform going wrong and far
    // looser than a last-bit difference.
    gs_world w;
    gs_scene(&w, GS_SURF_DIRT);
    gs_at_speed(&w, GS_INT(6), 0);
    w.car[0].vy = GS_INT(4);          // sideways, so the tyres are working too
    gs_settle(&w, &gs_t);

    const float rms = gs_rms(gs_buf, FRAMES);
    const float peak = gs_peak(gs_buf, FRAMES);

    // Taken on Linux with gcc, and the band is what a platform is allowed to
    // differ by. If this moves, either the synthesiser changed - which is a
    // decision, and the note goes here - or a platform is producing something
    // else, which is the fault this exists to catch.
    const float want_rms = 0.3738f, want_peak = 0.6667f;

    printf("  LOUDNESS a dirt race at speed: rms %.4f, peak %.4f "
           "(pinned %.4f and %.4f)\n", (double)rms, (double)peak,
           (double)want_rms, (double)want_peak);

    CHECK(rms > want_rms * 0.9f);
    CHECK(rms < want_rms * 1.1f);
    CHECK(peak > want_peak * 0.9f);
    CHECK(peak < want_peak * 1.1f);
}

TEST(nothing_the_synthesiser_produces_can_blow_a_speaker) {
    gs_world w;

    // **On every ground and in every machine**, not on dirt in a sprint car.
    //
    // This checked one surface, chosen as the loudest when there were three of
    // them - and three of the six that were added since are louder than it.
    // Slush is the heaviest drag here and gravel is the sharpest, and neither
    // had ever been asked whether four of them at once fit in a speaker. The
    // machine matters too: it sets the engine note this is all piled on top of.
    gs_audio_set_volume(1.0f);

    int mixes = 0;
    for (int surf = 0; surf < GS_SURF_COUNT; surf++) {
        for (int veh = 0; veh < GS_VEH_COUNT; veh++) {
            gs_scene(&w, (gs_surface)surf);
            w.car[0].vehicle = (uint8_t)veh;

            // Four cars, all sideways at speed, all being hit. If anything is
            // going to overshoot, it is this.
            for (uint8_t i = 1; i < GS_MAX_CARS; i++) {
                gs_world_add_car(&w, &gs_t, (uint8_t)veh,
                                 GS_INT(16), GS_INT(8) + GS_INT(i), 0);
            }

            // **And all of them armed**, so the struck voices the hazards use
            // are in the mix too. Four cars dropping and detonating on top of
            // four engines is the loudest a race gets.
            for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
                gs_world_arm(&w, (gs_hazard_kind)k, 9);
            }
            for (uint8_t i = 0; i < w.car_count; i++) {
                w.car[i].vx = GS_INT(8);
                w.car[i].vy = GS_INT(7);
            }

            for (int step = 0; step < 60; step++) {
                for (uint8_t i = 0; i < w.car_count; i++) {
                    // Damage climbing every update, which is what an impact is
                    // read from, so every voice is struck over and over.
                    if (w.car[i].damage < 200) {
                        w.car[i].damage = (uint8_t)(w.car[i].damage + 8);
                    }
                    // And everybody dropping whatever they still have, as fast
                    // as the cooldown allows.
                    w.car[i].drop_cooldown = 0;
                    gs_world_drop(&w, i, gs_car_selected(&w.car[i]));
                }
                gs_audio_update(&w, &gs_t, 16.0f, 8.0f);
                gs_audio_render(gs_buf, FRAMES);

                for (int i = 0; i < FRAMES * GS_AUDIO_CHANNELS; i++) {
                    if (gs_buf[i] < -1.0f || gs_buf[i] > 1.0f ||
                        gs_buf[i] != gs_buf[i]) {
                        printf("  OVER %s in a %s: sample %.4f at step %d\n",
                               gs_surfaces[surf].name, gs_vehicles[veh].name,
                               (double)gs_buf[i], step);
                    }
                    CHECK(gs_buf[i] >= -1.0f && gs_buf[i] <= 1.0f);
                    CHECK(gs_buf[i] == gs_buf[i]);      // and not a NaN
                    if (gs_failures > 0) {              // one report is enough
                        gs_audio_set_volume(0.8f);
                        return;
                    }
                }
            }
            mixes++;
        }
    }
    printf("  HEADROOM %d mixes: %d grounds x %d machines, four cars each, all "
           "sliding and all being hit\n", mixes, (int)GS_SURF_COUNT,
           (int)GS_VEH_COUNT);
    CHECK(mixes == (int)GS_SURF_COUNT * (int)GS_VEH_COUNT);
    gs_audio_set_volume(0.8f);
}

TEST(every_weapon_makes_a_noise_and_no_two_sound_the_same) {
    // **The synthesiser had never heard of a hazard.** Four things a player can
    // leave behind, and not one of them made a sound - a mine you cannot hear
    // behind you is a mine that feels like the game cheating.
    //
    // Walked from GS_HAZ_COUNT, so a fifth kind has to be given a noise rather
    // than inheriting the last one's, which is how six surfaces came to sound
    // like pavement.
    float level[GS_HAZ_COUNT];
    int   bright[GS_HAZ_COUNT];

    for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
        gs_world w;
        gs_scene(&w, GS_SURF_PAVEMENT);
        gs_world_arm(&w, (gs_hazard_kind)k, 1);

        // Settled first, so the mixer has seen this world once and a drop is a
        // change rather than the first thing it ever saw.
        gs_settle(&w, &gs_t);

        CHECK(gs_world_drop(&w, 0, (gs_hazard_kind)k));
        gs_audio_update(&w, &gs_t, 16.0f, 8.0f);
        gs_audio_render(gs_buf, FRAMES);

        level[k] = gs_peak(gs_buf, FRAMES);
        bright[k] = gs_crossings(gs_buf, FRAMES);
        CHECK(level[k] > 0.0f);
    }

    // **Every one of them is audible over the race going on around it.** The
    // reference is the same world with nothing dropped.
    gs_world quiet;
    gs_scene(&quiet, GS_SURF_PAVEMENT);
    gs_settle(&quiet, &gs_t);
    gs_audio_update(&quiet, &gs_t, 16.0f, 8.0f);
    gs_audio_render(gs_buf, FRAMES);
    const float floor_level = gs_peak(gs_buf, FRAMES);

    int silent = 0, alike = 0;
    for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
        if (level[k] <= floor_level * 1.10f) {
            silent++;
            printf("  WEAPON %s cannot be heard: %.4f against %.4f of race\n",
                   gs_hazard_name((gs_hazard_kind)k), (double)level[k],
                   (double)floor_level);
        }
    }
    CHECK(silent == 0);

    // **And no two are the same noise**, which is the point of there being
    // four: a player who cannot tell a mine being laid from smoke being let go
    // cannot tell what the car in front has done to them.
    for (int a = GS_HAZ_NONE + 1; a < GS_HAZ_COUNT; a++) {
        for (int b = a + 1; b < GS_HAZ_COUNT; b++) {
            const float apart = level[a] > level[b] ? level[a] - level[b]
                                                    : level[b] - level[a];
            const int sharper = bright[a] > bright[b] ? bright[a] - bright[b]
                                                      : bright[b] - bright[a];
            if (apart > level[a] * 0.10f || sharper > 10) continue;
            alike++;
            printf("  WEAPON %s and %s sound alike: %.4f/%d against %.4f/%d\n",
                   gs_hazard_name((gs_hazard_kind)a),
                   gs_hazard_name((gs_hazard_kind)b), (double)level[a],
                   bright[a], (double)level[b], bright[b]);
        }
    }
    printf("  WEAPONS %d kinds heard, %d pairs told apart\n",
           (int)GS_HAZ_COUNT - 1,
           (GS_HAZ_COUNT - 1) * (GS_HAZ_COUNT - 2) / 2);
    CHECK(alike == 0);
}

TEST(a_mine_going_off_is_louder_than_a_mine_being_laid) {
    // The one that has to carry across a race: laying a mine is a click nobody
    // should notice, and finding one is the loudest thing on the track.
    gs_world w;
    gs_scene(&w, GS_SURF_PAVEMENT);
    gs_world_add_car(&w, &gs_t, (uint8_t)GS_VEH_STOCK_CAR,
                     GS_INT(20), GS_INT(8), 0);
    gs_world_arm(&w, GS_HAZ_MINE, 1);
    gs_settle(&w, &gs_t);

    // Car one lays it, well away from the listener's car.
    w.car[1].x = GS_INT(20);
    w.car[1].y = GS_INT(8);
    CHECK(gs_world_drop(&w, 1, GS_HAZ_MINE));
    gs_audio_update(&w, &gs_t, 20.0f, 8.0f);
    gs_audio_render(gs_buf, FRAMES);
    const float laying = gs_peak(gs_buf, FRAMES);

    // And car zero drives onto it.
    w.car[0].x = GS_INT(20);
    w.car[0].y = GS_INT(8);
    w.car[0].grounded = true;
    for (int i = 0; i < 4; i++) gs_world_step(&w, &gs_t, nullptr);
    CHECK(w.hazard[0].spent == 1);      // it went off

    gs_audio_update(&w, &gs_t, 20.0f, 8.0f);
    gs_audio_render(gs_buf, FRAMES);
    const float going_off = gs_peak(gs_buf, FRAMES);

    if (going_off <= laying * 1.5f) {
        printf("  MINE laying %.4f, going off %.4f\n", (double)laying,
               (double)going_off);
    }
    CHECK(going_off > laying * 1.5f);
}

TEST(fire_is_heard_while_it_burns_and_not_after) {
    // Fire is the one that is not an event: it burns for seconds, so it is a
    // level that follows how much fire is near rather than a struck sound. And
    // it fades, because a crackle that stops dead reads as a fault in the game
    // rather than a fire going out.
    gs_world w;
    gs_scene(&w, GS_SURF_PAVEMENT);
    gs_world_arm(&w, GS_HAZ_FLAME, 1);
    gs_settle(&w, &gs_t);

    gs_audio_update(&w, &gs_t, 16.0f, 8.0f);
    gs_audio_render(gs_buf, FRAMES);
    const float before = gs_rms(gs_buf, FRAMES);

    CHECK(gs_world_drop(&w, 0, GS_HAZ_FLAME));

    // Long enough for the level to come up - it is chased rather than set.
    float burning = 0.0f;
    for (int i = 0; i < 40; i++) {
        gs_audio_update(&w, &gs_t, 16.0f, 8.0f);
        gs_audio_render(gs_buf, FRAMES);
        burning = gs_rms(gs_buf, FRAMES);
    }
    CHECK(burning > before);

    // Burnt out, and long enough after for the level to come back down.
    for (int i = 0; i < GS_TICK_HZ * 6; i++) gs_world_step(&w, &gs_t, nullptr);
    CHECK(w.hazard[0].spent == 1);

    float after = 0.0f;
    for (int i = 0; i < 80; i++) {
        gs_audio_update(&w, &gs_t, 16.0f, 8.0f);
        gs_audio_render(gs_buf, FRAMES);
        after = gs_rms(gs_buf, FRAMES);
    }
    CHECK(after < burning);
}

TEST(silence_is_a_fade_and_not_a_cut) {
    gs_world w;
    gs_scene(&w, GS_SURF_PAVEMENT);
    gs_at_speed(&w, GS_INT(6), 0);
    gs_settle(&w, &gs_t);

    gs_audio_silence();
    gs_audio_render(gs_buf, FRAMES);

    // The first sample after asking for silence is near where the signal was,
    // not at zero. A jump to zero is a click, and a click is the most
    // noticeable thing an audio system can do.
    float first = SDL_fabsf(gs_buf[0]);
    (void)first;

    // And it does get there.
    for (int i = 0; i < 400; i++) gs_audio_render(gs_buf, FRAMES);
    CHECK(gs_rms(gs_buf, FRAMES) < 0.002f);
}

// ---------------------------------------------------------------------------
// The music
// ---------------------------------------------------------------------------

static float gs_music_buf[FRAMES * GS_AUDIO_CHANNELS];

// Render `frames` of music alone, into a buffer that starts empty.
static void gs_music_only(uint64_t seed, float *out, int frames) {
    gs_music_start(seed);
    SDL_memset(out, 0, (size_t)frames * GS_AUDIO_CHANNELS * sizeof *out);

    // Past the fade-in, so what is measured is the piece rather than the ramp.
    static float warm[FRAMES * GS_AUDIO_CHANNELS];
    for (int i = 0; i < 12; i++) {
        SDL_memset(warm, 0, sizeof warm);
        gs_music_mix(warm, FRAMES);
    }
    gs_music_mix(out, frames);
}

TEST(the_lights_are_heard_lighting_and_going_green) {
    // **The start went by in silence.** A lamp lighting is a short beep, once
    // a second down the countdown, and the lights going green a longer,
    // higher one - noticed from how much was left last time, like everything
    // the mixer notices. Pinned on samples: the frame after each is louder
    // than the frame before it, and a tick that is neither is not.
    gs_world w;
    gs_scene(&w, GS_SURF_PAVEMENT);
    gs_world_set_countdown(&w, (uint32_t)GS_TICK_HZ * 3u);
    gs_settle(&w, &gs_t);
    const float lx = gs_f(w.car[0].x), ly = gs_f(w.car[0].y);

    // Half a second in: no lamp changed, nothing to hear but the idle.
    for (int i = 0; i < GS_TICK_HZ / 2; i++) gs_world_step(&w, &gs_t, nullptr);
    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    const float idle = gs_peak(gs_buf, FRAMES);

    // A second boundary: a lamp.
    for (int i = 0; i < GS_TICK_HZ / 2; i++) gs_world_step(&w, &gs_t, nullptr);
    CHECK(gs_world_countdown(&w) == (uint32_t)GS_TICK_HZ * 2u);
    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    const float lamp = gs_peak(gs_buf, FRAMES);

    // And green.
    for (int i = 0; i < GS_TICK_HZ * 2; i++) gs_world_step(&w, &gs_t, nullptr);
    CHECK(gs_world_countdown(&w) == 0);
    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    const float green = gs_peak(gs_buf, FRAMES);

    printf("  LIGHTS idle %.4f, a lamp %.4f, green %.4f\n", (double)idle,
           (double)lamp, (double)green);
    CHECK(lamp > idle * 1.3f);
    CHECK(green > idle * 1.3f);
}

TEST(coming_down_from_a_big_jump_thuds_and_a_hop_does_not) {
    // **A landing was heard only if it hurt**, and then as the same rumble a
    // collision makes. Coming down from a flight is a thud of its own now,
    // whether or not it hurt: the mixer sees a car that was in the air for a
    // big jump's worth and is on the ground. A hop over a bump is not.
    gs_world w;
    gs_scene(&w, GS_SURF_PAVEMENT);
    gs_settle(&w, &gs_t);
    const float lx = gs_f(w.car[0].x), ly = gs_f(w.car[0].y);

    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    const float sitting = gs_peak(gs_buf, FRAMES);

    // Up for a second, then down, undamaged.
    w.car[0].grounded = false;
    w.car[0].z = GS_INT(1);
    w.car[0].air_ticks = (uint32_t)GS_TICK_HZ;
    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    w.car[0].grounded = true;
    w.car[0].z = 0;
    w.car[0].air_ticks = 0;
    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    const float thud = gs_peak(gs_buf, FRAMES);

    // Let it die away, then a hop: five ticks up, and down.
    gs_settle(&w, &gs_t);
    w.car[0].grounded = false;
    w.car[0].z = GS_RATIO(1, 10);
    w.car[0].air_ticks = 5;
    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    w.car[0].grounded = true;
    w.car[0].z = 0;
    w.car[0].air_ticks = 0;
    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    const float hop = gs_peak(gs_buf, FRAMES);

    printf("  LANDING sitting %.4f, down from a jump %.4f, a hop %.4f\n",
           (double)sitting, (double)thud, (double)hop);
    CHECK(thud > sitting * 1.5f);
    CHECK(hop < sitting * 1.2f);
}

TEST(two_cars_meeting_is_a_clang_over_the_rumble) {
    // **A collision was the same rumble as a landing that hurt.** Hurt on the
    // ground - having been on the ground - is a clang now, a tone with an
    // edge above any engine, on top of the rumble the damage already made;
    // so it is both louder and brighter than the rumble alone was.
    gs_world w;
    gs_scene(&w, GS_SURF_PAVEMENT);
    gs_world_add_car(&w, &gs_t, (uint8_t)GS_VEH_DUNE_BUGGY, GS_INT(17), GS_INT(8), 0);
    gs_settle(&w, &gs_t);
    const float lx = gs_f(w.car[0].x), ly = gs_f(w.car[0].y);

    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    const float quiet = gs_rms(gs_buf, FRAMES);
    const int quiet_edges = gs_crossings(gs_buf, FRAMES);

    w.car[0].damage = (uint8_t)(w.car[0].damage + 40u);
    gs_audio_update(&w, &gs_t, lx, ly);
    gs_audio_render(gs_buf, FRAMES);
    const float clang = gs_rms(gs_buf, FRAMES);
    const int clang_edges = gs_crossings(gs_buf, FRAMES);

    printf("  COLLISION quiet %.4f (%d crossings), a hit %.4f (%d crossings)\n",
           (double)quiet, quiet_edges, (double)clang, clang_edges);
    CHECK(clang > quiet * 1.5f);
    CHECK(clang_edges > quiet_edges);
}

TEST(a_track_gets_its_own_tune_and_the_same_one_every_time) {
    // **The same seed is the same music, sample for sample.** A tune that came
    // out differently each time would be a tune nobody could describe to
    // anybody else, and the seed is a track's content hash - so this is also
    // what makes "that track with the good music" a thing somebody can say.
    static float once[FRAMES * GS_AUDIO_CHANNELS];
    gs_music_only(0x0123456789abcdefULL, once, FRAMES);
    gs_music_only(0x0123456789abcdefULL, gs_music_buf, FRAMES);
    CHECK(SDL_memcmp(once, gs_music_buf, sizeof once) == 0);

    // And it is not silence being compared with silence.
    CHECK(gs_rms(once, FRAMES) > 0.01f);

    // A different track is a different tune. Two hashes one bit apart, because
    // that is what two tracks one corner apart give.
    gs_music_only(0x0123456789abceefULL, gs_music_buf, FRAMES);
    CHECK(SDL_memcmp(once, gs_music_buf, sizeof once) != 0);

    // Not merely different samples - a different *piece*. Over a spread of
    // seeds it has to reach several keys and several tempos, or every track has
    // the same tune in a slightly different phase.
    int keys = 0, tempos = 0;
    uint8_t seen_key[12] = { 0 };
    uint16_t seen_bpm[24] = { 0 };
    for (uint64_t i = 0; i < 24; i++) {
        gs_music_start(0x9e3779b97f4a7c15ULL * (i + 1));
        gs_music_state st = gs_music_now();

        CHECK(st.root < 12);
        CHECK(st.bpm >= 100 && st.bpm <= 200);
        if (!seen_key[st.root]) { seen_key[st.root] = 1; keys++; }

        bool known = false;
        for (int k = 0; k < tempos; k++) {
            if (seen_bpm[k] == st.bpm) known = true;
        }
        if (!known && tempos < 24) seen_bpm[tempos++] = st.bpm;
    }
    CHECK(keys >= 6);
    CHECK(tempos >= 6);
}

TEST(the_music_goes_somewhere_rather_than_repeating_one_bar) {
    gs_music_start(0xfeedfaceULL);

    // Eight bars at 112-168 bpm is somewhere between eleven and seventeen
    // seconds, so a few seconds in it is a different part of the piece. Compare
    // the first second against the fifth: a loop of one bar would match.
    static float early[FRAMES * GS_AUDIO_CHANNELS];
    for (int i = 0; i < 12; i++) { SDL_memset(early, 0, sizeof early); gs_music_mix(early, FRAMES); }

    SDL_memset(early, 0, sizeof early);
    gs_music_mix(early, FRAMES);

    for (int i = 0; i < 40; i++) { SDL_memset(gs_music_buf, 0, sizeof gs_music_buf); gs_music_mix(gs_music_buf, FRAMES); }

    CHECK(SDL_memcmp(early, gs_music_buf, sizeof early) != 0);

    // But different samples prove nothing on their own: the oscillators drift,
    // so a piece looping one bar forever would also render differently. What
    // has to change is the *composition*, so the chord each bar sits on is
    // walked directly - and it has to reach more than one of them.
    CHECK(gs_music_now().bar > 0);

    gs_music_start(0xfeedfaceULL);
    uint8_t chords[8];
    int distinct = 0;
    for (int bar = 0; bar < 8; bar++) {
        // Far enough into the bar to be unambiguously in it.
        while (gs_music_now().bar == (uint32_t)bar) {
            SDL_memset(gs_music_buf, 0, sizeof gs_music_buf);
            gs_music_mix(gs_music_buf, 256);
            if (gs_music_now().bar != (uint32_t)bar) break;
            chords[bar] = gs_music_now().chord;
        }
    }
    for (int i = 0; i < 8; i++) {
        bool seen = false;
        for (int k = 0; k < i; k++) {
            if (chords[k] == chords[i]) seen = true;
        }
        if (!seen) distinct++;
    }
    // A progression goes somewhere and comes back, so it is more than one
    // chord and fewer than eight.
    CHECK(distinct >= 3);
    CHECK(distinct <= 7);
}

TEST(the_music_stops_by_fading_and_can_be_started_again) {
    gs_music_start(0xabcdefULL);
    for (int i = 0; i < 12; i++) { SDL_memset(gs_music_buf, 0, sizeof gs_music_buf); gs_music_mix(gs_music_buf, FRAMES); }
    CHECK(gs_rms(gs_music_buf, FRAMES) > 0.01f);
    CHECK(gs_music_playing());

    gs_music_stop();
    for (int i = 0; i < 200; i++) {
        SDL_memset(gs_music_buf, 0, sizeof gs_music_buf);
        gs_music_mix(gs_music_buf, FRAMES);
    }
    CHECK(gs_rms(gs_music_buf, FRAMES) < 0.002f);
    CHECK(!gs_music_playing());

    // And nothing is mixed in once it has stopped, rather than a quiet
    // something that never quite goes.
    SDL_memset(gs_music_buf, 0, sizeof gs_music_buf);
    gs_music_mix(gs_music_buf, FRAMES);
    CHECK(gs_rms(gs_music_buf, FRAMES) == 0.0f);

    gs_music_start(0xabcdefULL);
    for (int i = 0; i < 12; i++) { SDL_memset(gs_music_buf, 0, sizeof gs_music_buf); gs_music_mix(gs_music_buf, FRAMES); }
    CHECK(gs_rms(gs_music_buf, FRAMES) > 0.01f);
}

TEST(the_music_and_the_race_together_still_fit_in_a_speaker) {
    gs_world w;

    // On every ground, for the same reason as the test above: the loudest
    // surface when this was written is no longer the loudest surface, and the
    // race is being piled on top of a tune here rather than on silence.
    gs_audio_set_volume(1.0f);
    gs_music_set_volume(1.0f);

    int mixes = 0;
    for (int surf = 0; surf < GS_SURF_COUNT; surf++) {
        gs_scene(&w, (gs_surface)surf);
        for (uint8_t i = 1; i < GS_MAX_CARS; i++) {
            gs_world_add_car(&w, &gs_t, (uint8_t)GS_VEH_SPRINT_CAR,
                             GS_INT(16), GS_INT(8) + GS_INT(i), 0);
        }
        for (uint8_t i = 0; i < w.car_count; i++) {
            w.car[i].vx = GS_INT(8);
            w.car[i].vy = GS_INT(7);
        }

        // A different tune each time, so this is not one bar of one song
        // checked nine times.
        gs_music_start(0x1234ULL + (uint64_t)(unsigned)surf);

        for (int step = 0; step < 120; step++) {
            for (uint8_t i = 0; i < w.car_count; i++) {
                if (w.car[i].damage < 200) {
                    w.car[i].damage = (uint8_t)(w.car[i].damage + 8);
                }
            }
            gs_audio_update(&w, &gs_t, 16.0f, 8.0f);
            gs_audio_render(gs_buf, FRAMES);

            for (int i = 0; i < FRAMES * GS_AUDIO_CHANNELS; i++) {
                if (gs_buf[i] < -1.0f || gs_buf[i] > 1.0f ||
                    gs_buf[i] != gs_buf[i]) {
                    printf("  OVER %s with the music on: sample %.4f at step "
                           "%d\n", gs_surfaces[surf].name, (double)gs_buf[i],
                           step);
                }
                CHECK(gs_buf[i] >= -1.0f && gs_buf[i] <= 1.0f);
                CHECK(gs_buf[i] == gs_buf[i]);
                if (gs_failures > 0) {
                    gs_audio_set_volume(0.8f);
                    gs_music_set_volume(0.55f);
                    gs_music_stop();
                    return;
                }
            }
        }
        mixes++;
    }
    printf("  HEADROOM %d mixes with a tune under them, one per ground\n",
           mixes);
    CHECK(mixes == GS_SURF_COUNT);

    gs_audio_set_volume(0.8f);
    gs_music_set_volume(0.55f);
    gs_music_stop();
    for (int i = 0; i < 200; i++) gs_audio_render(gs_buf, FRAMES);
}

// **Last, and on its own.** Everything above renders the mixer from this
// thread with no device behind it, on purpose: a callback thread pulling on the
// mixer alongside them makes the answer depend on when it fired. This one wants
// the opposite - a real device and a real callback thread - so it runs after
// all of them and closes what it opened.
static void gs_device_test(void) {
    // **The half of the audio that is not platform-independent.** The
    // synthesiser is checked to the sample and none of that touches a device;
    // opening one, the thread SDL runs to pull on it, and the stream taking
    // what it is handed are three things that differ on every platform and that
    // no test had ever run, on any of them.
    //
    // The dummy driver is still a driver: it opens, it runs a callback thread
    // on a timer, and it consumes what that callback puts in. What it does not
    // do is make a noise - which is why the item this belongs to stays open
    // until somebody listens on each platform.
    gs_audio_close();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    // **The dummy driver, named here rather than left to the environment.**
    // Without this the test opens whatever the machine actually has - a real
    // sound server, on a developer's desktop - which makes a check depend on
    // the machine it runs on and, on this one, hands the leak checker four
    // allocations belonging to PulseAudio. The same reasoning as the sandbox
    // in every test main: a binary should not behave differently for being
    // started by hand.
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        printf("  FAIL no audio subsystem: %s\n", SDL_GetError());
        gs_failures++;
        return;
    }

    if (!gs_audio_open()) {
        printf("  FAIL no audio device even from the dummy driver: %s\n",
               SDL_GetError());
        gs_failures++;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    CHECK(gs_audio_active());

    gs_world w;
    gs_scene(&w, GS_SURF_DIRT);
    gs_at_speed(&w, GS_INT(6), 0);
    w.car[0].vy = GS_INT(4);
    gs_audio_update(&w, &gs_t, 16.0f, 8.0f);

    // **Something actually came out.** Up to a second, checked often, because
    // how soon a callback fires is the platform's business and not ours.
    int fed = 0;
    for (int i = 0; i < 100 && fed == 0; i++) {
        SDL_Delay(10);
        fed = gs_audio_fed();
    }

    if (fed == 0) {
        printf("  FAIL a device opened and its callback never fed it\n");
    }
    CHECK(fed > 0);
    printf("  DEVICE opened on this platform and fed %d frames to it\n", fed);

    // And closing it is not a crash, which is the other end of the same path
    // and is what a player quitting the game does.
    gs_audio_close();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

int main(void) {
    gs_sandbox();
    printf("gearstick audio tests\n");

    // **No device, and not even a dummy one.** These tests call the same render
    // the callback calls, and a callback thread pulling on the mixer alongside
    // them makes the answer depend on when it fired.
    SDL_Init(0);
    gs_audio_open_silent();

    run_a_race_makes_a_noise_and_an_empty_world_does_not();
    run_the_engine_note_follows_the_drivetrain_up_and_down();
    run_the_ground_a_car_is_on_changes_what_it_sounds_like();
    run_a_car_in_the_air_makes_no_tyre_noise_at_all();
    run_a_car_further_away_is_quieter_than_the_one_being_driven();
    run_a_race_sounds_the_same_loudness_on_every_platform();
    run_nothing_the_synthesiser_produces_can_blow_a_speaker();
    run_every_weapon_makes_a_noise_and_no_two_sound_the_same();
    run_a_mine_going_off_is_louder_than_a_mine_being_laid();
    run_fire_is_heard_while_it_burns_and_not_after();
    run_silence_is_a_fade_and_not_a_cut();
    run_a_track_gets_its_own_tune_and_the_same_one_every_time();
    run_the_music_goes_somewhere_rather_than_repeating_one_bar();
    run_the_music_stops_by_fading_and_can_be_started_again();
    run_the_music_and_the_race_together_still_fit_in_a_speaker();
    run_the_lights_are_heard_lighting_and_going_green();
    run_coming_down_from_a_big_jump_thuds_and_a_hop_does_not();
    run_two_cars_meeting_is_a_clang_over_the_rumble();

    // Last, because it opens a real device and starts a callback thread.
    gs_device_test();

    gs_audio_close();
    SDL_Quit();

    if (gs_failures > 0) {
        printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
        return 1;
    }
    printf("all audio tests passed\n");
    return 0;
}
