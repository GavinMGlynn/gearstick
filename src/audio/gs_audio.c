#include "audio/gs_audio.h"

#include "audio/gs_music.h"
#include "core/gs_vehicle.h"

#include <SDL3/SDL.h>

// Q16.16 to float. Defined here rather than borrowed from src/gfx/, because the
// audio has no business depending on the renderer, and core cannot hand it over
// - core has no floating point in it at all.
static inline float gs_to_f(gs_fix v) { return (float)v / (float)GS_ONE; }

// --- The gearbox ------------------------------------------------------------
//
// Five ratios and a final drive, which is what makes an engine note *mean*
// something: without a gearbox the pitch is a straight function of speed and
// the car sounds like a vacuum cleaner accelerating. With one, the note climbs
// to the limiter, drops, and climbs again, and a player can hear how fast they
// are going without looking.

#define GS_GEARS 5

static const float gs_ratio[GS_GEARS] = { 3.40f, 2.10f, 1.48f, 1.12f, 0.88f };

#define GS_FINAL_DRIVE 3.6f
#define GS_IDLE_RPM    850.0f
#define GS_REDLINE_RPM 6800.0f
#define GS_SHIFT_UP    6100.0f
#define GS_SHIFT_DOWN  2600.0f

// Revolutions per minute for a road speed in tiles per second, in a given gear.
// One tile is four metres and a wheel turns about 2.0 m per revolution, so this
// is metres per second to wheel revolutions to crankshaft revolutions.
static float gs_rpm_for(float speed_tiles, int gear) {
    float wheel_rps = speed_tiles * 4.0f / 2.0f;
    return wheel_rps * gs_ratio[gear] * GS_FINAL_DRIVE * 60.0f;
}

// --- What the audio thread reads --------------------------------------------

typedef struct gs_voice {
    bool  active;

    // Engine
    float rpm;
    float engine_phase;
    float engine_gain;
    int   gear;

    // Tyres
    float tyre_gain;
    float tyre_bright;    // 0 gritty (dirt), 1 sharp (ice)
    float tyre_lp, tyre_lp2;   // two cascaded poles - see the render loop

    // Impact, decaying
    float hit;
    float hit_phase;

    float pan;            // -1 left, +1 right
    float distance_gain;
} gs_voice;

// **A noise something on the ground makes**, as against the noise a car makes.
// Hazards are not cars: there are up to thirty-two of them, they do not have
// engines, and the sound one makes is a single event rather than a thing that
// goes on for the whole race. So they get a small bank of struck voices between
// them, and the bank being small is on purpose - eight things going off at once
// is already more than anybody can pick apart, and a hundred is mud.
#define GS_AUDIO_EVENTS 8

typedef struct gs_event {
    float gain;      // rings down to nothing; zero means the slot is free
    float phase;
    float tone;      // where its body sits, in hertz
    float noisy;     // how much of it is hiss rather than tone
    float decay;     // per sample
    float pan;
} gs_event;

static struct {
    SDL_AudioStream *stream;
    SDL_Mutex       *lock;
    bool             open;
    float            volume;

    gs_voice voice[GS_MAX_CARS];

    // Remembered from the last update, so an impact can be noticed without the
    // simulation having to report one. Damage only ever goes up, so a jump in
    // it *is* a collision or a bad landing.
    uint8_t last_damage[GS_MAX_CARS];
    bool    last_grounded[GS_MAX_CARS];
    // And how long each was in the air, so coming down can be told from a
    // hop; and how much of the countdown was left, so a lamp lighting and
    // the lights going green can be heard.
    uint32_t last_air[GS_MAX_CARS];
    uint32_t last_left;
    bool    seeded;

    gs_event event[GS_AUDIO_EVENTS];

    // **Fire, which is the one that is not an event.** It burns for seconds
    // rather than cracking once, so it is a level that follows how much fire is
    // near the listener rather than a struck voice - and it fades in and out
    // instead of switching, because a crackle that starts and stops dead reads
    // as a fault in the game rather than a fire going out.
    float burn, burn_want;
    float burn_lp;

    // Remembered from the last update, the same way a car's damage is: the
    // simulation is not asked to report a drop or a detonation, it is noticed
    // by looking at what changed. Sound is downstream of the simulation and
    // never upstream, and this is what that costs.
    uint8_t  last_hazards;
    uint8_t  last_kind[GS_MAX_HAZARDS];
    uint8_t  last_spent[GS_MAX_HAZARDS];

    // Frames the callback has handed to a device - see the note by it. Atomic
    // because the callback runs on its own thread and this is read from the
    // one that opened it.
    SDL_AtomicInt fed;

    uint32_t noise;        // xorshift state for the tyre noise
    float    master;       // ramped, so silence is a fade and not a click
    float    master_want;
} gs_a;

static float gs_noise_next(void) {
    // A cheap deterministic white noise. Nothing here needs cryptography and a
    // race must not stall to fill a buffer.
    gs_a.noise ^= gs_a.noise << 13;
    gs_a.noise ^= gs_a.noise >> 17;
    gs_a.noise ^= gs_a.noise << 5;
    return (float)(int32_t)gs_a.noise / 2147483648.0f;
}

// --- Synthesis --------------------------------------------------------------

void gs_audio_render(float *out, int frames) {
    SDL_memset(out, 0, (size_t)frames * GS_AUDIO_CHANNELS * sizeof *out);

    // The music goes in first and the race on top of it, in the same buffer, so
    // that the soft clip below covers both. Mixing it in afterwards would let
    // it push the total past full scale on exactly the loud moments the clip
    // exists for.
    gs_music_mix(out, frames);

    if (gs_a.lock != nullptr) SDL_LockMutex(gs_a.lock);

    for (int i = 0; i < frames; i++) {
        // The master level chases where it wants to be rather than jumping,
        // because an instant change in level is a click and a click is the one
        // thing an ear notices immediately.
        gs_a.master += (gs_a.master_want - gs_a.master) * 0.0008f;

        float left = 0.0f, right = 0.0f;

        for (int v = 0; v < GS_MAX_CARS; v++) {
            gs_voice *o = &gs_a.voice[v];
            if (!o->active) continue;

            // --- Engine. Firing frequency, not crankshaft frequency: a
            // four-cylinder four-stroke fires twice a revolution, which is why
            // an engine sounds an octave above what its rev counter says.
            float f = o->rpm * 2.0f / 60.0f;
            o->engine_phase += f / (float)GS_AUDIO_RATE;
            if (o->engine_phase >= 1.0f) o->engine_phase -= 1.0f;

            // A stack of harmonics rather than a sine: an engine is a series of
            // bangs, and the upper harmonics are what make it sound like a
            // combustion engine rather than a flute.
            float p = o->engine_phase;
            float engine = 0.0f;
            engine += SDL_sinf(p * 6.2831853f) * 0.62f;
            engine += SDL_sinf(p * 12.566371f) * 0.34f;
            engine += SDL_sinf(p * 18.849556f) * 0.20f;
            engine += SDL_sinf(p * 25.132741f) * 0.12f;
            engine += SDL_sinf(p * 31.415927f) * 0.07f;

            // A trace of noise so it is not a pure tone - and only a trace.
            // The first mix had four times this and the whole race hissed.
            engine += gs_noise_next() * 0.015f;
            engine *= o->engine_gain;

            // --- Tyres. Noise through a one-pole low pass whose cutoff is the
            // surface: dirt is a rumble, ice is a hiss, pavement is between.
            //
            // **Compensated for the filter's own gain**, which is the whole
            // reason this is three lines rather than two. A one-pole low pass
            // fed white noise puts out sqrt(k / (2 - k)) of what went in, so a
            // dark filter is a quiet one - and without the correction, dirt at
            // full tyre gain came out *quieter* than ice at half of it. The
            // surface was reaching the synthesiser perfectly and arriving
            // backwards.
            float n = gs_noise_next();
            float k = 0.03f + o->tyre_bright * 0.32f;
            o->tyre_lp  += (n - o->tyre_lp) * k;
            o->tyre_lp2 += (o->tyre_lp - o->tyre_lp2) * k;
            float tyre = o->tyre_lp2 * o->tyre_gain * ((2.0f - k) / k) * 0.55f;

            // --- Impact. A struck, fast-decaying burst.
            float hit = 0.0f;
            if (o->hit > 0.0001f) {
                o->hit_phase += 90.0f / (float)GS_AUDIO_RATE;
                if (o->hit_phase >= 1.0f) o->hit_phase -= 1.0f;
                hit = (gs_noise_next() * 0.7f +
                       SDL_sinf(o->hit_phase * 6.2831853f) * 0.5f) * o->hit;
                o->hit *= 0.99982f;
            }

            float s = (engine + tyre + hit) * o->distance_gain;

            // Constant-power panning, so a car crossing the screen does not get
            // quieter in the middle.
            float ang = (o->pan * 0.5f + 0.5f) * 1.5707963f;
            left  += s * SDL_cosf(ang);
            right += s * SDL_sinf(ang);
        }

        // --- What is on the ground. Struck voices, panned where they lie.
        for (int e = 0; e < GS_AUDIO_EVENTS; e++) {
            gs_event *ev = &gs_a.event[e];
            if (ev->gain <= 0.0001f) { ev->gain = 0.0f; continue; }

            ev->phase += ev->tone / (float)GS_AUDIO_RATE;
            if (ev->phase >= 1.0f) ev->phase -= 1.0f;

            const float body = SDL_sinf(ev->phase * 6.2831853f) * (1.0f - ev->noisy);
            const float hiss = gs_noise_next() * ev->noisy;
            const float s = (body + hiss) * ev->gain;
            ev->gain *= ev->decay;

            const float ang = (ev->pan * 0.5f + 0.5f) * 1.5707963f;
            left  += s * SDL_cosf(ang);
            right += s * SDL_sinf(ang);
        }

        // --- Fire, which is a level and not an event. Filtered noise, and the
        // level chased rather than set so that a fire going out fades.
        gs_a.burn += (gs_a.burn_want - gs_a.burn) * 0.00015f;
        if (gs_a.burn > 0.0001f) {
            const float n = gs_noise_next();
            gs_a.burn_lp += (n - gs_a.burn_lp) * 0.10f;
            const float s = gs_a.burn_lp * gs_a.burn * 3.0f;
            left  += s;
            right += s;
        }

        // Loud enough to be a game. The first mix peaked at a tenth of full
        // scale, which is a race you have to turn the amplifier up for and
        // then get deafened by the next thing you play.
        float g = gs_a.master * gs_a.volume * 1.35f;
        left = left * g + out[i * 2 + 0];
        right = right * g + out[i * 2 + 1];

        // Soft clip. Four cars landing together will overshoot, and a soft knee
        // is a squash where hard clipping is a tear.
        left  = SDL_clamp(left  - left  * left  * left  / 3.0f, -1.0f, 1.0f);
        right = SDL_clamp(right - right * right * right / 3.0f, -1.0f, 1.0f);

        out[i * 2 + 0] = left;
        out[i * 2 + 1] = right;
    }

    if (gs_a.lock != nullptr) SDL_UnlockMutex(gs_a.lock);
}

static void SDLCALL gs_audio_callback(void *userdata, SDL_AudioStream *stream,
                                      int additional, int total) {
    (void)userdata;
    (void)total;

    static float buf[2048 * GS_AUDIO_CHANNELS];
    const int frame_bytes = (int)sizeof(float) * GS_AUDIO_CHANNELS;

    while (additional > 0) {
        int frames = additional / frame_bytes;
        if (frames > 2048) frames = 2048;
        if (frames <= 0) break;

        gs_audio_render(buf, frames);
        SDL_PutAudioStreamData(stream, buf, frames * frame_bytes);
        additional -= frames * frame_bytes;

        // **How many frames have actually reached a device.** The synthesiser
        // is checked to the sample without one; this is the only evidence that
        // the path *to* a device works at all - opening it, the callback
        // thread, and the stream taking what it is given. That path is the half
        // of the audio that is not platform-independent, and it is the half no
        // test had ever run.
        SDL_AddAtomicInt(&gs_a.fed, frames);
    }
}

// --- Opening and closing ----------------------------------------------------

// The state the mixer starts from, whether or not anything is going to play it.
static void gs_audio_reset(void) {
    gs_a.noise = 0x13579bdfu;
    gs_a.volume = 0.8f;
    gs_a.master_want = 1.0f;
    gs_a.master = 0.0f;

    gs_a.lock = SDL_CreateMutex();
}

void gs_audio_open_silent(void) {
    if (gs_a.open) return;
    gs_audio_reset();
    gs_a.stream = nullptr;
    gs_a.open = true;
}

bool gs_audio_open(void) {
    if (gs_a.open) return true;

    gs_audio_reset();

    SDL_AudioSpec want = { SDL_AUDIO_F32, GS_AUDIO_CHANNELS, GS_AUDIO_RATE };
    gs_a.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &want, gs_audio_callback, nullptr);
    if (gs_a.stream == nullptr) {
        // No device is not a failure. A race in silence is the same race, and
        // stopping for this would mean a machine with no sound card cannot play
        // a driving game.
        SDL_Log("audio: no device (%s) - racing in silence", SDL_GetError());
        gs_a.open = true;
        return false;
    }

    SDL_ResumeAudioStreamDevice(gs_a.stream);
    gs_a.open = true;
    return true;
}

void gs_audio_close(void) {
    if (!gs_a.open) return;
    if (gs_a.stream != nullptr) SDL_DestroyAudioStream(gs_a.stream);
    if (gs_a.lock != nullptr) SDL_DestroyMutex(gs_a.lock);
    SDL_zero(gs_a);
}

bool gs_audio_active(void) { return gs_a.open && gs_a.stream != nullptr; }

int gs_audio_fed(void) { return SDL_GetAtomicInt(&gs_a.fed); }

void gs_audio_set_volume(float v) { gs_a.volume = SDL_clamp(v, 0.0f, 1.0f); }
float gs_audio_volume(void) { return gs_a.volume; }

void gs_audio_silence(void) {
    if (gs_a.lock != nullptr) SDL_LockMutex(gs_a.lock);
    gs_a.master_want = 0.0f;
    if (gs_a.lock != nullptr) SDL_UnlockMutex(gs_a.lock);
}

// --- What the world sounds like ---------------------------------------------

// **How much a surface roughens the tyre note, and how bright it is - for
// every surface there is.**
//
// This had three cases and a `default`, written when there were three surfaces.
// Six more went in - sand, gravel, rock, dust, slush, grass, each with its own
// grip, its own rolling resistance and its own way of wearing - and every one
// of them fell through the default and **sounded exactly like pavement**. Two
// thirds of the grounds in a game whose editor lets you paint all nine, and the
// test next door says the point of this function is "knowing what you are
// driving on without looking".
//
// So: no `default`. A tenth surface now fails to compile here rather than
// quietly sounding like the first, which is the only part of this that a test
// cannot check for itself.
//
// The numbers are a judgement and not a derivation - what grass sounds like is
// not in its rolling resistance - but they are read off the character each
// surface is given in gs_track.c, and no two are alike in either column.
static void gs_surface_voice(gs_surface s, float *level, float *bright) {
    switch (s) {
    // Made ground, and the one everything else is heard against.
    case GS_SURF_PAVEMENT: *level = 0.62f; *bright = 0.45f; break;
    // A rumble. Loose enough to roar, heavy enough to have no top end.
    case GS_SURF_DIRT:     *level = 1.00f; *bright = 0.15f; break;
    // A hiss, and almost nothing under it.
    case GS_SURF_ICE:      *level = 0.45f; *bright = 0.90f; break;
    // Thrown rather than rolled over: loud, and high with it.
    case GS_SURF_SAND:     *level = 0.85f; *bright = 0.70f; break;
    // Stones off the underside. The sharpest thing here that is not ice.
    case GS_SURF_GRAVEL:   *level = 0.92f; *bright = 0.60f; break;
    // Basalt: hard, loud and entirely bottom end.
    case GS_SURF_ROCK:     *level = 0.78f; *bright = 0.05f; break;
    // Regolith, never weathered. Fine, quiet and nearly all hiss.
    case GS_SURF_DUST:     *level = 0.55f; *bright = 0.80f; break;
    // Wet and heavy: the loudest drag here, and dull with it.
    case GS_SURF_SLUSH:    *level = 0.95f; *bright = 0.35f; break;
    // A swish, sitting between pavement and the loose stuff.
    case GS_SURF_GRASS:    *level = 0.70f; *bright = 0.55f; break;
    // Not a surface. Here so the switch is exhaustive and stays that way.
    case GS_SURF_COUNT:    *level = 0.62f; *bright = 0.45f; break;
    }
}

// **What each kind sounds like when it is put down**, and what a mine sounds
// like when it is found. One list, named one by one, so a fifth kind of hazard
// has to be given a noise rather than inheriting the last one's - which is how
// six surfaces came to sound like pavement.
static void gs_event_voice(gs_hazard_kind kind, bool blast, float *tone,
                           float *noisy, float *decay, float *gain) {
    if (blast) {
        // A mine going off: low, loud and long enough to turn round for.
        *tone = 70.0f; *noisy = 0.85f; *decay = 0.99988f; *gain = 1.0f;
        return;
    }
    switch (kind) {
    case GS_HAZ_OIL:
        // Poured: low and wet, and over quickly.
        *tone = 140.0f; *noisy = 0.80f; *decay = 0.9994f; *gain = 0.34f;
        break;
    case GS_HAZ_MINE:
        // Set down: a click, and nothing else. The quietest of the four,
        // because a mine you can hear being laid is a mine nobody drives over.
        *tone = 900.0f; *noisy = 0.25f; *decay = 0.9985f; *gain = 0.26f;
        break;
    case GS_HAZ_SMOKE:
        // A hiss, and it goes on - the canister is still emptying.
        *tone = 300.0f; *noisy = 1.00f; *decay = 0.99975f; *gain = 0.40f;
        break;
    case GS_HAZ_FLAME:
        // A whoosh: the sound of something catching.
        *tone = 200.0f; *noisy = 0.92f; *decay = 0.99965f; *gain = 0.48f;
        break;
    case GS_HAZ_NONE:
    case GS_HAZ_COUNT:
        *tone = 0.0f; *noisy = 0.0f; *decay = 0.0f; *gain = 0.0f;
        break;
    }
}

// Put one in the bank. The quietest slot is taken when they are all busy, so a
// mine going off is never lost to four slicks being poured.
// **A struck voice at a place**: what everything that happens once sounds
// like, a hazard going off or a moment in a race. Panned and quietened by
// where it is against the listener, and taking the quietest slot in the bank
// only if it is louder than what is there.
static void gs_strike(float x, float y, float lx, float ly, float tone,
                      float noisy, float decay, float gain) {
    if (gain <= 0.0f) return;

    const float dx = x - lx, dy = y - ly;
    const float d = SDL_sqrtf(dx * dx + dy * dy);
    gain *= 1.0f / (1.0f + d * d * 0.02f);

    int at = 0;
    for (int i = 1; i < GS_AUDIO_EVENTS; i++) {
        if (gs_a.event[i].gain < gs_a.event[at].gain) at = i;
    }
    if (gs_a.event[at].gain > gain) return;      // busier than this is worth

    gs_a.event[at].gain  = gain;
    gs_a.event[at].phase = 0.0f;
    gs_a.event[at].tone  = tone;
    gs_a.event[at].noisy = noisy;
    gs_a.event[at].decay = decay;
    gs_a.event[at].pan   = SDL_clamp((dx - dy) * 0.06f, -1.0f, 1.0f);
}

static void gs_event_add(gs_hazard_kind kind, bool blast, float x, float y,
                         float lx, float ly) {
    float tone = 0.0f, noisy = 0.0f, decay = 0.0f, gain = 0.0f;
    gs_event_voice(kind, blast, &tone, &noisy, &decay, &gain);
    gs_strike(x, y, lx, ly, tone, noisy, decay, gain);
}

void gs_audio_update(const gs_world *w, const gs_track *t, float lx, float ly) {
    if (!gs_a.open) return;
    if (gs_a.lock != nullptr) SDL_LockMutex(gs_a.lock);

    gs_a.master_want = 1.0f;

    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_voice *o = &gs_a.voice[i];

        if (i >= w->car_count || !w->car[i].active) {
            o->active = false;
            continue;
        }
        const gs_car *c = &w->car[i];
        o->active = true;

        float speed = gs_to_f(gs_car_speed(c));

        // --- The gearbox, read from the simulation rather than guessed
        // from speed. The car itself knows which gear it is in - the
        // automatic picked it, or the driver shifted into it - so the note
        // climbs and drops through exactly the gears the wheels are using,
        // and a manual shift is heard the instant it happens. The audio's
        // own ratios still shape the *note* within a gear; which gear it is
        // comes from `c->gear`, 1-based, mapped onto the ratio table.
        const gs_vehicle_def *def = gs_vehicle(c->vehicle);
        const uint8_t gears = def != nullptr && def->gears > 0 ? def->gears : 1;
        int idx = (int)c->gear - 1;
        idx = (idx * GS_GEARS) / (gears > 0 ? gears : 1);
        o->gear = SDL_clamp(idx, 0, GS_GEARS - 1);
        float rpm = gs_rpm_for(speed, o->gear);

        // **In the air the engine goes light.** Nothing is loading it, so it
        // runs up towards the limiter - which, with the tyre noise gone at the
        // same moment, is the sound of a jump.
        if (!c->grounded) rpm = rpm * 0.55f + GS_REDLINE_RPM * 0.45f;

        rpm = SDL_clamp(rpm, GS_IDLE_RPM, GS_REDLINE_RPM);

        // Chased rather than set, so a rollback or a collision does not step the
        // pitch. An engine has inertia and so does this.
        o->rpm += (rpm - o->rpm) * 0.25f;

        o->engine_gain = c->wrecked ? 0.0f : 0.30f + speed * 0.022f;

        // --- Tyres. Loud when the car is sliding, silent when it is not
        // touching anything.
        float level = 0.0f, bright = 0.5f;
        if (c->grounded) {
            gs_surface s = gs_track_surface(t, c->x, c->y);
            gs_surface_voice(s, &level, &bright);

            // Slip: how much of the car's motion is sideways. A car tracking
            // straight makes very little noise; a car sideways makes all of it.
            float vx = gs_to_f(c->vx), vy = gs_to_f(c->vy);
            float ch = gs_to_f(gs_cos(c->heading)), sh = gs_to_f(gs_sin(c->heading));
            float lateral = SDL_fabsf(-vx * sh + vy * ch);

            // **Rolling is nearly silent; sliding is not.** Tyre noise that
            // is always there is just hiss over the top of the engine, and the
            // information a player wants from it is *am I sliding* - so almost
            // all of the level is on the slip and almost none on the speed.
            float roll = SDL_clamp(speed / 8.0f, 0.0f, 1.0f) * 0.07f;
            float slide = SDL_clamp(lateral / 3.0f, 0.0f, 1.0f) * 0.42f;
            level *= roll + slide;
        }
        o->tyre_gain += (level - o->tyre_gain) * 0.2f;
        o->tyre_bright = bright;

        // --- Impacts, noticed rather than reported. Damage only ever goes up,
        // so a jump in it is a collision or a landing that hurt, and the size of
        // the jump is how hard it was.
        if (gs_a.seeded && c->damage > gs_a.last_damage[i]) {
            float hurt = (float)(c->damage - gs_a.last_damage[i]) / 40.0f;
            o->hit = SDL_clamp(o->hit + hurt, 0.0f, 1.0f);
            o->hit_phase = 0.0f;
        }
        // --- **The moments**, noticed the same way. Coming down from a big
        // jump is a thud - low, mostly noise, gone in a third of a second,
        // heavier the longer the flight - whether or not it hurt. Hitting
        // another car is a clang: a tone with an edge, higher than any
        // engine, over the rumble the damage already made. Told apart by
        // whether the car was in the air: a landing that hurt is a landing.
        {
            const float cx = gs_to_f(c->x), cy = gs_to_f(c->y);
            if (gs_a.seeded && !gs_a.last_grounded[i] && c->grounded &&
                gs_a.last_air[i] >= GS_BIG_AIR_TICKS) {
                const float flight = SDL_clamp((float)gs_a.last_air[i] /
                                                   (float)GS_TICK_HZ, 0.25f, 1.5f);
                gs_strike(cx, cy, lx, ly, 55.0f, 0.75f, 0.99985f, 0.7f + flight * 0.5f);
            }
            if (gs_a.seeded && c->damage > gs_a.last_damage[i] && c->grounded &&
                gs_a.last_grounded[i]) {
                const float hurt = (float)(c->damage - gs_a.last_damage[i]) / 40.0f;
                gs_strike(cx, cy, lx, ly, 620.0f, 0.35f, 0.99970f,
                          SDL_clamp(0.35f + hurt * 0.5f, 0.0f, 0.9f));
            }
        }
        gs_a.last_damage[i] = c->damage;
        gs_a.last_grounded[i] = c->grounded;
        gs_a.last_air[i] = c->air_ticks;

        // --- Where it is. Distance from the listener, and which side.
        float dx = gs_to_f(c->x) - lx;
        float dy = gs_to_f(c->y) - ly;
        float d = SDL_sqrtf(dx * dx + dy * dy);
        o->distance_gain = 1.0f / (1.0f + d * d * 0.02f);

        // The isometric screen axis: +x and -y are both to the right.
        o->pan = SDL_clamp((dx - dy) * 0.06f, -1.0f, 1.0f);
    }

    // --- What is on the ground. **Noticed rather than reported**, the same way
    // an impact is: the simulation is not asked to say "a mine went off", it is
    // looked at and something has changed. Sound is downstream of the
    // simulation and never upstream, and this is what that costs.
    float burn = 0.0f;
    for (uint8_t i = 0; i < w->hazard_count && i < GS_MAX_HAZARDS; i++) {
        const gs_hazard *h = &w->hazard[i];
        const float hx = gs_to_f(h->x), hy = gs_to_f(h->y);

        if (gs_a.seeded) {
            // **New here.** Either the slot did not exist last time, or it did
            // and now holds something else - which is what the ring does when
            // thirty-two are already down.
            const bool fresh = i >= gs_a.last_hazards ||
                               gs_a.last_kind[i] != h->kind;
            if (fresh && !h->spent) {
                gs_event_add((gs_hazard_kind)h->kind, false, hx, hy, lx, ly);
            }

            // **Found.** A mine that has just been spent went off; smoke and
            // fire become spent by burning out, which is not a bang.
            if (!fresh && h->spent && !gs_a.last_spent[i] &&
                h->kind == (uint8_t)GS_HAZ_MINE) {
                gs_event_add(GS_HAZ_MINE, true, hx, hy, lx, ly);
            }
        }

        gs_a.last_kind[i]  = h->kind;
        gs_a.last_spent[i] = h->spent;

        // Fire is a level rather than an event: it burns for seconds, so what
        // it wants is to be louder the more of it there is and the nearer it
        // is.
        if (h->kind == (uint8_t)GS_HAZ_FLAME && !h->spent) {
            const float dx = hx - lx, dy = hy - ly;
            burn += 1.0f / (1.0f + (dx * dx + dy * dy) * 0.05f);
        }
    }
    gs_a.last_hazards = w->hazard_count;
    gs_a.burn_want = SDL_clamp(burn, 0.0f, 1.0f) * 0.30f;

    // --- **The lights.** A short beep as each lamp lights - once a second
    // through the countdown - and a longer, higher one on the tick the lights
    // go green, at the listener rather than at a place, since the start is
    // everybody's. Noticed from what was left last time, like everything.
    {
        const uint32_t left = gs_world_countdown(w);
        if (gs_a.seeded && gs_a.last_left > 0) {
            if (left == 0) {
                gs_strike(lx, ly, lx, ly, 880.0f, 0.0f, 0.99993f, 0.55f);
            } else if ((gs_a.last_left - 1u) / (uint32_t)GS_TICK_HZ !=
                       (left - 1u) / (uint32_t)GS_TICK_HZ) {
                gs_strike(lx, ly, lx, ly, 440.0f, 0.0f, 0.99960f, 0.40f);
            }
        }
        gs_a.last_left = left;
    }
    gs_a.seeded = true;
    if (gs_a.lock != nullptr) SDL_UnlockMutex(gs_a.lock);
}
