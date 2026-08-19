#include "audio/gs_music.h"

#include "audio/gs_audio.h"

#include <SDL3/SDL.h>

// --- the composer -----------------------------------------------------------
//
// Everything below is decided once, from the seed, when a track is loaded. What
// runs per sample is a sequencer reading tables, which is the same division a
// tracker made in 1985 and for the same reason: composition is cheap and
// oscillators are not.

#define GS_BARS       8       // the piece, before it repeats
#define GS_STEPS      16      // sixteenth notes in a bar
#define GS_VOICES     4

// A minor and a major scale, in semitones. Two moods and nothing in between,
// because a generative score that can reach every mode reaches most of them by
// accident.
static const int8_t gs_minor_scale[7] = { 0, 2, 3, 5, 7, 8, 10 };
static const int8_t gs_major_scale[7] = { 0, 2, 4, 5, 7, 9, 11 };

// Chord progressions that go somewhere and come back. Degrees of the scale.
static const int8_t gs_progression[4][4] = {
    { 0, 5, 3, 4 },   // i - VI - IV - V
    { 0, 3, 4, 0 },
    { 0, 4, 5, 4 },
    { 0, 2, 3, 4 },
};

typedef struct gs_note {
    uint8_t degree;     // scale degree, 0 for a rest
    uint8_t octave;
    uint8_t length;     // in steps
} gs_note;

static struct {
    bool     playing;
    uint64_t seed;
    uint32_t rng;

    uint8_t  root;
    bool     minor;
    uint16_t bpm;
    uint8_t  progression;

    // The written-out piece.
    gs_note  lead[GS_BARS][GS_STEPS];
    uint8_t  bass[GS_BARS][GS_STEPS];
    uint8_t  drum[GS_BARS][GS_STEPS];   // 0 none, 1 kick, 2 snare, 3 hat

    // Where the sequencer is.
    double   step_samples;
    double   sample_in_step;
    uint32_t step;
    uint32_t bar;

    // The oscillators.
    float    phase[GS_VOICES];
    float    freq[GS_VOICES];
    float    env[GS_VOICES];
    float    duty[GS_VOICES];
    uint32_t noise;
    float    noise_env;
    float    noise_lp;

    // Arpeggio: a chiptune's chords are one voice playing three notes very
    // fast, because one voice is what there was.
    uint8_t  arp[3];
    uint8_t  arp_at;

    float    volume;
    float    gain, gain_want;
} gs_m;

static uint32_t gs_rand(void) {
    gs_m.rng ^= gs_m.rng << 13;
    gs_m.rng ^= gs_m.rng >> 17;
    gs_m.rng ^= gs_m.rng << 5;
    return gs_m.rng;
}

static uint32_t gs_pick(uint32_t n) { return gs_rand() % n; }

// Equal temperament, A above middle C at 440. A table would be more in period
// and this is not the simulation - it may use libm, and a note that is a
// fraction of a cent out on one platform is a note nobody can hear the
// difference in.
static float gs_note_hz(int semitones_above_a) {
    return 440.0f * SDL_powf(2.0f, (float)semitones_above_a / 12.0f);
}

static int gs_degree_semitone(uint8_t degree, uint8_t octave) {
    const int8_t *scale = gs_m.minor ? gs_minor_scale : gs_major_scale;
    int d = degree % 7;
    int extra = degree / 7;
    return gs_m.root + scale[d] + 12 * ((int)octave + extra) - 24;
}

void gs_music_start(uint64_t seed) {
    SDL_zero(gs_m);
    gs_m.seed = seed;

    // Fold the whole hash in, so two tracks differing anywhere differ here.
    gs_m.rng = (uint32_t)(seed ^ (seed >> 32));
    if (gs_m.rng == 0) gs_m.rng = 0x9e3779b9u;
    for (int i = 0; i < 8; i++) gs_rand();

    gs_m.root = (uint8_t)gs_pick(12);
    gs_m.minor = gs_pick(4) != 0;             // mostly minor; it is a racing game
    gs_m.bpm = (uint16_t)(112u + gs_pick(56));
    gs_m.progression = (uint8_t)gs_pick(4);
    gs_m.volume = 0.55f;
    gs_m.gain_want = 1.0f;

    for (uint32_t bar = 0; bar < GS_BARS; bar++) {
        int8_t chord = gs_progression[gs_m.progression][bar % 4];

        // --- The bass: the root of the chord on the beat, with a passing note
        // where a bass player would put one.
        for (uint32_t s = 0; s < GS_STEPS; s++) {
            if (s % 4 == 0) {
                gs_m.bass[bar][s] = (uint8_t)(chord + 1);
            } else if (s % 8 == 6 && gs_pick(3) == 0) {
                gs_m.bass[bar][s] = (uint8_t)(chord + 3);
            } else {
                gs_m.bass[bar][s] = 0;
            }
        }

        // --- The lead: notes from the chord and its neighbours, with rests.
        // The last bar of every four gets more of them, which is what makes a
        // phrase sound like it ends rather than stopping.
        bool turnaround = (bar % 4) == 3;
        for (uint32_t s = 0; s < GS_STEPS; s++) {
            uint32_t density = turnaround ? 2 : 3;
            if (gs_pick(density) == 0) {
                gs_m.lead[bar][s].degree = 0;      // a rest
                continue;
            }
            int8_t offsets[4] = { 0, 2, 4, 6 };
            int8_t pick = offsets[gs_pick(4)];
            gs_m.lead[bar][s].degree = (uint8_t)(chord + pick + 1);
            gs_m.lead[bar][s].octave = (uint8_t)(1 + (gs_pick(5) == 0 ? 1 : 0));
            gs_m.lead[bar][s].length = (uint8_t)(1 + gs_pick(2));
        }

        // --- Percussion. A kick on one and three, a snare on two and four, and
        // hats between - the pattern every driving game has ever had, because
        // it is the one that gets out of the way.
        for (uint32_t s = 0; s < GS_STEPS; s++) {
            if (s % 8 == 0) gs_m.drum[bar][s] = 1;
            else if (s % 8 == 4) gs_m.drum[bar][s] = 2;
            else if (s % 2 == 0) gs_m.drum[bar][s] = 3;
            else gs_m.drum[bar][s] = (uint8_t)(gs_pick(4) == 0 ? 3 : 0);
        }
    }

    // Sixteenth notes: four to the beat.
    gs_m.step_samples = (double)GS_AUDIO_RATE * 60.0 / ((double)gs_m.bpm * 4.0);
    gs_m.noise = 0x2545f491u;
    gs_m.playing = true;
}

void gs_music_stop(void) { gs_m.gain_want = 0.0f; }
bool gs_music_playing(void) { return gs_m.playing && gs_m.gain > 0.0005f; }
void gs_music_set_volume(float v) { gs_m.volume = SDL_clamp(v, 0.0f, 1.0f); }
float gs_music_volume(void) { return gs_m.volume; }

gs_music_state gs_music_now(void) {
    gs_music_state st = { 0 };
    st.root = gs_m.root;
    st.minor = gs_m.minor;
    st.bpm = gs_m.bpm;
    st.bar = gs_m.bar;
    st.chord = (uint8_t)gs_progression[gs_m.progression][(gs_m.bar % GS_BARS) % 4];
    st.step = (uint8_t)gs_m.step;
    return st;
}

// --- the sequencer ----------------------------------------------------------

static void gs_music_step(void) {
    uint32_t bar = gs_m.bar % GS_BARS;
    uint32_t s = gs_m.step;

    // Bass.
    if (gs_m.bass[bar][s] != 0) {
        gs_m.freq[0] = gs_note_hz(gs_degree_semitone((uint8_t)(gs_m.bass[bar][s] - 1), 0));
        gs_m.env[0] = 1.0f;
    }

    // Lead.
    if (gs_m.lead[bar][s].degree != 0) {
        uint8_t d = (uint8_t)(gs_m.lead[bar][s].degree - 1);
        gs_m.freq[1] = gs_note_hz(gs_degree_semitone(d, (uint8_t)(gs_m.lead[bar][s].octave + 1)));
        gs_m.env[1] = 1.0f;

        // The chord under it, as an arpeggio on one voice - a third and a fifth
        // above, cycled per step. This is the sound.
        gs_m.arp[0] = d;
        gs_m.arp[1] = (uint8_t)(d + 2);
        gs_m.arp[2] = (uint8_t)(d + 4);
    }

    // The arpeggio voice moves every step whether or not the lead did.
    gs_m.arp_at = (uint8_t)((gs_m.arp_at + 1u) % 3u);
    gs_m.freq[2] = gs_note_hz(gs_degree_semitone(gs_m.arp[gs_m.arp_at], 1));
    gs_m.env[2] = 0.75f;

    // Percussion.
    switch (gs_m.drum[bar][s]) {
    case 1: gs_m.freq[3] = 62.0f; gs_m.env[3] = 1.0f; gs_m.noise_env = 0.25f; break;
    case 2: gs_m.noise_env = 1.0f; break;
    case 3: gs_m.noise_env = 0.22f; break;
    default: break;
    }

    if (++gs_m.step >= GS_STEPS) {
        gs_m.step = 0;
        gs_m.bar++;
    }
}

void gs_music_mix(float *out, int frames) {
    if (!gs_m.playing) return;

    for (int i = 0; i < frames; i++) {
        gs_m.gain += (gs_m.gain_want - gs_m.gain) * 0.00005f;

        if (gs_m.sample_in_step <= 0.0) {
            gs_music_step();
            gs_m.sample_in_step += gs_m.step_samples;
        }
        gs_m.sample_in_step -= 1.0;

        float mix = 0.0f;

        // Bass: a pulse an octave down, long decay.
        gs_m.phase[0] += gs_m.freq[0] / (float)GS_AUDIO_RATE;
        if (gs_m.phase[0] >= 1.0f) gs_m.phase[0] -= 1.0f;
        mix += (gs_m.phase[0] < 0.5f ? 0.55f : -0.55f) * gs_m.env[0];
        gs_m.env[0] *= 0.99993f;

        // Lead: a pulse whose duty moves, which is the sound a SID made when
        // somebody wanted a note to feel alive.
        gs_m.duty[1] += 0.0000021f;
        if (gs_m.duty[1] > 0.42f) gs_m.duty[1] -= 0.34f;
        gs_m.phase[1] += gs_m.freq[1] / (float)GS_AUDIO_RATE;
        if (gs_m.phase[1] >= 1.0f) gs_m.phase[1] -= 1.0f;
        mix += (gs_m.phase[1] < 0.20f + gs_m.duty[1] ? 0.30f : -0.30f) * gs_m.env[1];
        gs_m.env[1] *= 0.99988f;

        // Arpeggio: a triangle, quieter, sitting under the lead.
        gs_m.phase[2] += gs_m.freq[2] / (float)GS_AUDIO_RATE;
        if (gs_m.phase[2] >= 1.0f) gs_m.phase[2] -= 1.0f;
        float tri = gs_m.phase[2] < 0.5f ? gs_m.phase[2] * 4.0f - 1.0f
                                         : 3.0f - gs_m.phase[2] * 4.0f;
        mix += tri * 0.20f * gs_m.env[2];
        gs_m.env[2] *= 0.9997f;

        // Kick: a sine that drops in pitch, which is a kick drum.
        gs_m.freq[3] *= 0.99993f;
        gs_m.phase[3] += gs_m.freq[3] / (float)GS_AUDIO_RATE;
        if (gs_m.phase[3] >= 1.0f) gs_m.phase[3] -= 1.0f;
        mix += SDL_sinf(gs_m.phase[3] * 6.2831853f) * 0.70f * gs_m.env[3];
        gs_m.env[3] *= 0.9995f;

        // Snare and hats: noise through a one-pole, because that is all a snare
        // needs to be next to everything else going on.
        gs_m.noise ^= gs_m.noise << 13;
        gs_m.noise ^= gs_m.noise >> 17;
        gs_m.noise ^= gs_m.noise << 5;
        float n = (float)(int32_t)gs_m.noise / 2147483648.0f;
        gs_m.noise_lp += (n - gs_m.noise_lp) * 0.45f;
        mix += gs_m.noise_lp * 0.5f * gs_m.noise_env;
        gs_m.noise_env *= 0.9992f;

        mix *= gs_m.volume * gs_m.gain * 0.30f;

        // Centred: music is not in the world, so it has no side to come from.
        out[i * 2 + 0] += mix;
        out[i * 2 + 1] += mix;
    }

    if (gs_m.gain_want == 0.0f && gs_m.gain < 0.0005f) gs_m.playing = false;
}
