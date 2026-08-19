#include "audio/gs_audio.h"

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
    bool    seeded;

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

        // Loud enough to be a game. The first mix peaked at a tenth of full
        // scale, which is a race you have to turn the amplifier up for and
        // then get deafened by the next thing you play.
        float g = gs_a.master * gs_a.volume * 1.35f;
        left *= g;
        right *= g;

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
    }
}

// --- Opening and closing ----------------------------------------------------

bool gs_audio_open(void) {
    if (gs_a.open) return true;

    gs_a.noise = 0x13579bdfu;
    gs_a.volume = 0.8f;
    gs_a.master_want = 1.0f;
    gs_a.master = 0.0f;

    gs_a.lock = SDL_CreateMutex();

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

void gs_audio_set_volume(float v) { gs_a.volume = SDL_clamp(v, 0.0f, 1.0f); }
float gs_audio_volume(void) { return gs_a.volume; }

void gs_audio_silence(void) {
    if (gs_a.lock != nullptr) SDL_LockMutex(gs_a.lock);
    gs_a.master_want = 0.0f;
    if (gs_a.lock != nullptr) SDL_UnlockMutex(gs_a.lock);
}

// --- What the world sounds like ---------------------------------------------

// How much a surface roughens the tyre note, and how bright it is.
static void gs_surface_voice(gs_surface s, float *level, float *bright) {
    switch (s) {
    case GS_SURF_DIRT:  *level = 1.00f; *bright = 0.15f; break;  // a rumble
    case GS_SURF_ICE:   *level = 0.45f; *bright = 0.90f; break;  // a hiss
    default:            *level = 0.62f; *bright = 0.45f; break;  // pavement
    }
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

        // --- The gearbox. Hysteresis on both changes, or a car sitting at a
        // shift point hunts between two gears and sounds broken.
        if (o->gear < 0 || o->gear >= GS_GEARS) o->gear = 0;
        float rpm = gs_rpm_for(speed, o->gear);
        if (rpm > GS_SHIFT_UP && o->gear < GS_GEARS - 1) {
            o->gear++;
            rpm = gs_rpm_for(speed, o->gear);
        } else if (rpm < GS_SHIFT_DOWN && o->gear > 0) {
            o->gear--;
            rpm = gs_rpm_for(speed, o->gear);
        }

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
        gs_a.last_damage[i] = c->damage;
        gs_a.last_grounded[i] = c->grounded;

        // --- Where it is. Distance from the listener, and which side.
        float dx = gs_to_f(c->x) - lx;
        float dy = gs_to_f(c->y) - ly;
        float d = SDL_sqrtf(dx * dx + dy * dy);
        o->distance_gain = 1.0f / (1.0f + d * d * 0.02f);

        // The isometric screen axis: +x and -y are both to the right.
        o->pan = SDL_clamp((dx - dy) * 0.06f, -1.0f, 1.0f);
    }

    gs_a.seeded = true;
    if (gs_a.lock != nullptr) SDL_UnlockMutex(gs_a.lock);
}
