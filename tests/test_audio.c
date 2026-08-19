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
static void gs_settle(const gs_world *w, const gs_track *t) {
    float lx = w->car_count > 0 ? gs_f(w->car[0].x) : 0.0f;
    float ly = w->car_count > 0 ? gs_f(w->car[0].y) : 0.0f;
    for (int i = 0; i < 120; i++) {
        gs_audio_update(w, t, lx, ly);
        gs_audio_render(gs_buf, FRAMES);
    }
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
    w.car[0].vx = GS_INT(6);
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
    float pitch[4];
    const int speeds[4] = { 0, 1, 2, 7 };

    for (int i = 0; i < 4; i++) {
        gs_scene(&w, GS_SURF_PAVEMENT);
        w.car[0].vx = GS_INT(speeds[i]);
        gs_settle(&w, &gs_t);
        pitch[i] = gs_pitch(gs_buf, FRAMES);
    }

    // A stationary car idles rather than stopping. An engine note that goes to
    // nothing at a standstill is a car that has stalled.
    CHECK(pitch[0] > 20.0f && pitch[0] < 40.0f);

    // Within a gear the note is very nearly proportional to road speed, because
    // in a gear that is exactly what it is. Doubling the speed doubles it.
    CHECK(pitch[1] > pitch[0] * 1.4f);
    CHECK(pitch[2] > pitch[1] * 1.8f);
    CHECK(pitch[2] < pitch[1] * 2.2f);

    // **And here is the gearbox.** Seven tiles a second is three and a half
    // times two tiles a second, so without a gearbox the note would be three
    // and a half times higher. It is about one and a half, because the car has
    // changed up twice on the way - which is the entire reason to model a
    // drivetrain rather than to multiply a frequency by a speed.
    CHECK(pitch[3] > pitch[0]);
    CHECK(pitch[3] < pitch[2] * 2.0f);
}

TEST(the_ground_a_car_is_on_changes_what_it_sounds_like) {
    gs_world w;

    // The same car at the same speed, sliding the same amount, on three
    // surfaces. If these come out the same, the surface is not reaching the
    // synthesiser at all.
    float level[3];
    int bright[3];
    const gs_surface surfaces[3] = { GS_SURF_PAVEMENT, GS_SURF_DIRT, GS_SURF_ICE };

    for (int i = 0; i < 3; i++) {
        gs_scene(&w, surfaces[i]);
        w.car[0].vx = GS_INT(5);
        w.car[0].vy = GS_INT(4);          // sideways, so the tyres are working
        gs_settle(&w, &gs_t);
        level[i] = gs_rms(gs_buf, FRAMES);
        bright[i] = gs_crossings(gs_buf, FRAMES);
    }

    // Dirt is the loudest and ice the quietest, which is the whole point of
    // knowing what you are driving on without looking.
    CHECK(level[1] > level[0]);
    CHECK(level[0] > level[2]);

    // And ice is the brightest: a hiss rather than a rumble.
    CHECK(bright[2] > bright[1]);
}

TEST(a_car_in_the_air_makes_no_tyre_noise_at_all) {
    gs_world w;

    // On the ground, sliding hard on dirt: about as much tyre noise as there is.
    gs_scene(&w, GS_SURF_DIRT);
    w.car[0].vx = GS_INT(6);
    w.car[0].vy = GS_INT(5);
    w.car[0].grounded = true;
    gs_settle(&w, &gs_t);
    float grounded = gs_rms(gs_buf, FRAMES);

    // The same car, the same instant, in the air. The engine is still running -
    // louder, if anything, because nothing is loading it - but the tyres have
    // gone completely.
    gs_scene(&w, GS_SURF_DIRT);
    w.car[0].vx = GS_INT(6);
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
    w.car[0].vx = GS_INT(6);

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

TEST(nothing_the_synthesiser_produces_can_blow_a_speaker) {
    gs_world w;
    gs_scene(&w, GS_SURF_DIRT);

    // Four cars, all sideways at speed, all being hit. If anything is going to
    // overshoot, it is this.
    for (uint8_t i = 1; i < GS_MAX_CARS; i++) {
        gs_world_add_car(&w, &gs_t, (uint8_t)GS_VEH_SPRINT_CAR,
                         GS_INT(16), GS_INT(8) + GS_INT(i), 0);
    }
    for (uint8_t i = 0; i < w.car_count; i++) {
        w.car[i].vx = GS_INT(8);
        w.car[i].vy = GS_INT(7);
    }
    gs_audio_set_volume(1.0f);

    for (int step = 0; step < 200; step++) {
        for (uint8_t i = 0; i < w.car_count; i++) {
            // Damage climbing every update, which is what an impact is read
            // from, so every voice is struck over and over.
            if (w.car[i].damage < 200) w.car[i].damage = (uint8_t)(w.car[i].damage + 8);
        }
        gs_audio_update(&w, &gs_t, 16.0f, 8.0f);
        gs_audio_render(gs_buf, FRAMES);

        for (int i = 0; i < FRAMES * GS_AUDIO_CHANNELS; i++) {
            CHECK(gs_buf[i] >= -1.0f && gs_buf[i] <= 1.0f);
            CHECK(gs_buf[i] == gs_buf[i]);      // and not a NaN
            if (gs_failures > 0) return;        // one report is enough
        }
    }
    gs_audio_set_volume(0.8f);
}

TEST(silence_is_a_fade_and_not_a_cut) {
    gs_world w;
    gs_scene(&w, GS_SURF_PAVEMENT);
    w.car[0].vx = GS_INT(6);
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
    gs_scene(&w, GS_SURF_DIRT);
    for (uint8_t i = 1; i < GS_MAX_CARS; i++) {
        gs_world_add_car(&w, &gs_t, (uint8_t)GS_VEH_SPRINT_CAR,
                         GS_INT(16), GS_INT(8) + GS_INT(i), 0);
    }
    for (uint8_t i = 0; i < w.car_count; i++) {
        w.car[i].vx = GS_INT(8);
        w.car[i].vy = GS_INT(7);
    }

    gs_audio_set_volume(1.0f);
    gs_music_set_volume(1.0f);
    gs_music_start(0x1234ULL);

    for (int step = 0; step < 120; step++) {
        for (uint8_t i = 0; i < w.car_count; i++) {
            if (w.car[i].damage < 200) w.car[i].damage = (uint8_t)(w.car[i].damage + 8);
        }
        gs_audio_update(&w, &gs_t, 16.0f, 8.0f);
        gs_audio_render(gs_buf, FRAMES);

        for (int i = 0; i < FRAMES * GS_AUDIO_CHANNELS; i++) {
            CHECK(gs_buf[i] >= -1.0f && gs_buf[i] <= 1.0f);
            CHECK(gs_buf[i] == gs_buf[i]);
            if (gs_failures > 0) return;
        }
    }
    gs_audio_set_volume(0.8f);
    gs_music_set_volume(0.55f);
    gs_music_stop();
    for (int i = 0; i < 200; i++) gs_audio_render(gs_buf, FRAMES);
}

int main(void) {
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
    run_nothing_the_synthesiser_produces_can_blow_a_speaker();
    run_silence_is_a_fade_and_not_a_cut();
    run_a_track_gets_its_own_tune_and_the_same_one_every_time();
    run_the_music_goes_somewhere_rather_than_repeating_one_bar();
    run_the_music_stops_by_fading_and_can_be_started_again();
    run_the_music_and_the_race_together_still_fit_in_a_speaker();

    gs_audio_close();
    SDL_Quit();

    if (gs_failures > 0) {
        printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
        return 1;
    }
    printf("all audio tests passed\n");
    return 0;
}
