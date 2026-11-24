/* SPDX-License-Identifier: GPL-3.0-only */
/* M-3rd-VAVE's engine: a wavetable polysynth after the Groove Synthesis 3rd Wave, on the PPG Wave 2.2's own waves
 * (w3_tables.h, from its EPROMs). 24 voices (12 on one core), up to 4 a key in unison, each:
 *   three oscillators   a table (00 RESONANT .. 27 FMNTVOCAL) and a position in it (0..60 the table, 61-63 a
 *                       triangle, square and saw), moved by the wave envelope and the LFO in the PPG's steps; the
 *                       waves in 8 bits, read linearly from a copy band-limited for the note (no harmonic past ~18 kHz)
 *   noise
 *   a 4-pole low-pass   one-pole stages with the resonance through a tanh; its envelope, key and velocity
 *   three envelopes     amp, filter, wave (ADSR, one step every 32 frames; the amp ramped across it)
 * then one LFO for all (vibrato, the wave position, the cutoff), the voices spread across the stereo, a ping-pong
 * delay, a soft clip.
 * Floats (fastmath.h): build with -ffp-contract=off, on the chip and on the host alike (eng/test_engine.c). */
#include <stdint.h>
#include "fastmath.h"
#include "w3_tables.h"

#define W3_FS 44100.0f
#define W3_NV 24u                                /* voices with the second core */
#define W3_NV1 12u                               /* voices on one core (the host keeps core 1 back: a crash, its first 0.75 s) */
#define W3_CTL 32u                               /* frames between control steps */
#define W3_CTL_DT ((float)W3_CTL / W3_FS)
#define W3_DLY 24000u                            /* delay line a side: 0.54 s */

/* a patch: W3_NP small numbers, each in its W3_PARAM range */
enum { O_TAB, O_POS, O_ENV, O_LFO, O_SEMI, O_FINE, O_LVL, O_N };
#define P_OSC(i, f) ((i) * O_N + (f))
enum {
    P_NOISE = 3 * O_N, P_CUT, P_RES, P_FENV, P_KTRK, P_FVEL,
    P_AA, P_AD, P_AS, P_AR, P_FA, P_FD, P_FS, P_FR, P_WA, P_WD, P_WS, P_WR,
    P_LRATE, P_LPITCH, P_LCUT, P_AVEL, P_SPREAD, P_DTIME, P_DFB, P_DMIX, P_UNI, P_UDET, P_LAMT, W3_NP
};
/* the LFO's rate: a beat division at the tempo (w3_bpm: the module's), 8 bars .. 1/32 */
#define W3_NDIV 13u
static const char *const W3_DIV_NAME[W3_NDIV] = {"8 BARS", "4 BARS", "2 BARS", "1 BAR", "1/2", "1/2T", "1/4", "1/4T",
                                                 "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const float W3_DIV_BEATS[W3_NDIV] = {32.0f, 16.0f, 8.0f, 4.0f, 2.0f, 4.0f / 3.0f, 1.0f, 2.0f / 3.0f, 0.5f,
                                            1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};   /* beats a cycle */
static volatile float w3_bpm = 120.0f;
typedef struct { const char *name; int8_t lo, hi; } w3_param_t;
#define W3_OSC_PARAMS(n) {n " TABLE", 0, W3_NTAB - 1}, {n " WAVE", 0, 63}, {n " WENV", -63, 63}, {n " WLFO", 0, 63}, \
                         {n " SEMI", -24, 24}, {n " FINE", -50, 50}, {n " LEVEL", 0, 100}
static const w3_param_t W3_PARAM[W3_NP] = {
    W3_OSC_PARAMS("OSC1"), W3_OSC_PARAMS("OSC2"), W3_OSC_PARAMS("OSC3"),
    {"NOISE", 0, 100}, {"CUTOFF", 0, 127}, {"RESO", 0, 100}, {"FILT ENV", -100, 100}, {"KEYTRACK", 0, 100},
    {"FILT VEL", 0, 100},
    {"AMP ATK", 0, 127}, {"AMP DEC", 0, 127}, {"AMP SUS", 0, 100}, {"AMP REL", 0, 127},
    {"FLT ATK", 0, 127}, {"FLT DEC", 0, 127}, {"FLT SUS", 0, 100}, {"FLT REL", 0, 127},
    {"WAV ATK", 0, 127}, {"WAV DEC", 0, 127}, {"WAV SUS", 0, 100}, {"WAV REL", 0, 127},
    {"LFO RATE", 0, W3_NDIV - 1}, {"VIBRATO", 0, 100}, {"LFO>CUT", 0, 100}, {"AMP VEL", 0, 100}, {"SPREAD", 0, 100},
    {"DLY TIME", 0, 127}, {"DLY FDBK", 0, 100}, {"DLY MIX", 0, 100},
    {"UNISON", 1, 4}, {"UNI DETUNE", 0, 50}, {"LFO AMT", 0, 100},   /* voices a key; cents between the outer two,
                                                                         * either side; added movement (0: the preset as made) */
};

enum { W3_PAD, W3_KEYS, W3_BASS, W3_LEAD, W3_BELLS, W3_PLUCK, W3_FX, W3_INIT, W3_NTYPE };
static const char *const W3_TYPE_NAME[W3_NTYPE] = {"PAD", "KEYS", "BASS", "LEAD", "BELLS", "PLUCK", "FX", "INIT"};
typedef struct { const char *name; uint8_t type; int8_t p[W3_NP]; } w3_preset_t;
#define OSC(i, tab, pos, env, lfo, semi, fine, lvl) [P_OSC(i, O_TAB)] = tab, [P_OSC(i, O_POS)] = pos, \
    [P_OSC(i, O_ENV)] = env, [P_OSC(i, O_LFO)] = lfo, [P_OSC(i, O_SEMI)] = semi, [P_OSC(i, O_FINE)] = fine, \
    [P_OSC(i, O_LVL)] = lvl
#define ENV(x, a, d, s, r) [P_##x##A] = a, [P_##x##D] = d, [P_##x##S] = s, [P_##x##R] = r
/* times 0..127: 1 ms .. 10 s, exponentially (64: 100 ms) */
static const w3_preset_t W3_PRESET[] = {
    {"SAW SWEEP", W3_PAD, {OSC(0, 6, 60, -60, 0, 0, -6, 70), OSC(1, 6, 60, -60, 0, 0, 6, 70), ENV(A, 70, 90, 80, 85),
                   ENV(F, 60, 95, 40, 85), ENV(W, 0, 110, 0, 90), [P_CUT] = 80, [P_RES] = 20, [P_FENV] = 30,
                   [P_KTRK] = 50, [P_LRATE] = 2, [P_LPITCH] = 6, [P_AVEL] = 30, [P_SPREAD] = 70,
                   [P_DTIME] = 90, [P_DFB] = 35, [P_DMIX] = 25}},
    {"VOX PAD", W3_PAD, {OSC(0, 27, 10, 40, 20, 0, -4, 70), OSC(1, 16, 20, 30, 15, 0, 4, 60), OSC(2, 26, 30, 0, 25, 12, 0, 30),
                 ENV(A, 85, 90, 90, 95), ENV(F, 80, 90, 70, 90), ENV(W, 95, 105, 30, 95), [P_CUT] = 92, [P_RES] = 10,
                 [P_FENV] = 15, [P_KTRK] = 40, [P_LRATE] = 2, [P_LPITCH] = 5, [P_AVEL] = 20, [P_SPREAD] = 90,
                 [P_DTIME] = 100, [P_DFB] = 45, [P_DMIX] = 30}},
    {"DIGI BELL", W3_BELLS, {OSC(0, 4, 0, 40, 0, 0, 0, 80), OSC(1, 2, 20, 30, 0, 12, 3, 40), ENV(A, 0, 100, 0, 100),
                   ENV(F, 0, 95, 20, 95), ENV(W, 0, 85, 0, 85), [P_CUT] = 105, [P_RES] = 5, [P_FENV] = 20,
                   [P_KTRK] = 70, [P_AVEL] = 60, [P_FVEL] = 40, [P_SPREAD] = 60, [P_DTIME] = 80, [P_DFB] = 30,
                   [P_DMIX] = 25, [P_LRATE] = 3}},
    {"RESO BASS", W3_BASS, {OSC(0, 0, 50, -40, 0, -12, 0, 85), OSC(1, 7, 10, 0, 0, -12, 4, 50), ENV(A, 0, 75, 70, 50),
                   ENV(F, 0, 70, 10, 50), ENV(W, 0, 65, 0, 50), [P_CUT] = 52, [P_RES] = 25, [P_FENV] = 35,
                   [P_KTRK] = 40, [P_AVEL] = 40, [P_FVEL] = 30, [P_SPREAD] = 0, [P_LRATE] = 3}},
    {"E.PIANO", W3_KEYS, {OSC(0, 19, 0, 30, 0, 0, -3, 75), OSC(1, 19, 8, 30, 0, 0, 3, 55), ENV(A, 0, 105, 0, 85),
                 ENV(F, 0, 95, 30, 80), ENV(W, 0, 90, 0, 80), [P_CUT] = 95, [P_RES] = 0, [P_FENV] = 15,
                 [P_KTRK] = 60, [P_LRATE] = 3, [P_LCUT] = 0, [P_AVEL] = 60, [P_FVEL] = 50, [P_SPREAD] = 50,
                 [P_DTIME] = 75, [P_DFB] = 20, [P_DMIX] = 15}},
    {"ORGAN SYN", W3_KEYS, {OSC(0, 14, 20, 0, 10, 0, 0, 70), OSC(1, 22, 30, 0, 0, 12, 2, 40), ENV(A, 5, 60, 100, 40),
                   ENV(F, 0, 60, 100, 40), ENV(W, 0, 60, 100, 40), [P_CUT] = 100, [P_KTRK] = 30, [P_LRATE] = 5,
                   [P_LPITCH] = 4, [P_SPREAD] = 40, [P_DTIME] = 60, [P_DFB] = 15, [P_DMIX] = 12}},
    {"WIND", W3_PAD, {OSC(0, 11, 5, 30, 30, 0, 0, 60), OSC(1, 11, 40, -20, 20, 7, 5, 40), ENV(A, 95, 95, 85, 100),
              ENV(F, 95, 95, 60, 100), ENV(W, 100, 100, 50, 100), [P_NOISE] = 25, [P_CUT] = 75, [P_RES] = 35,
              [P_FENV] = 25, [P_KTRK] = 40, [P_LRATE] = 1, [P_LCUT] = 25, [P_SPREAD] = 100, [P_DTIME] = 110,
              [P_DFB] = 55, [P_DMIX] = 35}},
    {"TRANSIENT", W3_PLUCK, {OSC(0, 18, 0, 60, 0, 0, 0, 80), OSC(1, 25, 10, 40, 0, 0, 7, 40), ENV(A, 0, 85, 20, 75),
                   ENV(F, 0, 80, 20, 70), ENV(W, 0, 70, 0, 70), [P_CUT] = 85, [P_RES] = 15, [P_FENV] = 30,
                   [P_KTRK] = 60, [P_AVEL] = 50, [P_FVEL] = 40, [P_SPREAD] = 60, [P_DTIME] = 70, [P_DFB] = 40,
                   [P_DMIX] = 30, [P_LRATE] = 3}},
    {"ROBOTIC", W3_FX, {OSC(0, 20, 0, 0, 63, 0, 0, 75), OSC(1, 20, 30, 0, 50, -12, 0, 45), ENV(A, 10, 80, 80, 70),
                 ENV(F, 10, 80, 60, 70), ENV(W, 0, 80, 100, 70), [P_CUT] = 88, [P_RES] = 30, [P_FENV] = 10,
                 [P_KTRK] = 50, [P_LRATE] = 5, [P_AVEL] = 30, [P_SPREAD] = 50, [P_DTIME] = 64, [P_DFB] = 30,
                 [P_DMIX] = 20}},
    {"CLIP LEAD", W3_LEAD, {OSC(0, 23, 0, 63, 0, 0, -8, 70), OSC(1, 13, 20, 40, 0, 0, 8, 60), ENV(A, 5, 85, 70, 70),
                   ENV(F, 5, 85, 50, 70), ENV(W, 30, 100, 40, 80), [P_CUT] = 82, [P_RES] = 25, [P_FENV] = 25,
                   [P_KTRK] = 60, [P_LRATE] = 5, [P_LPITCH] = 15, [P_AVEL] = 30, [P_SPREAD] = 30,
                   [P_DTIME] = 85, [P_DFB] = 35, [P_DMIX] = 25}},
    {"RESO SWEEP", W3_PAD, {OSC(0, 1, 0, 55, 0, 0, -5, 75), OSC(1, 1, 10, 45, 0, 0, 5, 60), ENV(A, 20, 100, 30, 85),
                    ENV(F, 20, 95, 40, 85), ENV(W, 40, 105, 20, 90), [P_CUT] = 88, [P_RES] = 15, [P_FENV] = 20,
                    [P_KTRK] = 50, [P_AVEL] = 30, [P_SPREAD] = 70, [P_DTIME] = 95, [P_DFB] = 40, [P_DMIX] = 25, [P_LRATE] = 3}},
    {"SQR LEAD", W3_LEAD, {OSC(0, 3, 20, 20, 30, 0, -7, 75), OSC(1, 3, 35, 10, 20, 12, 7, 35), ENV(A, 10, 85, 75, 70),
                  ENV(F, 5, 85, 55, 70), ENV(W, 20, 95, 60, 75), [P_CUT] = 84, [P_RES] = 20, [P_FENV] = 25,
                  [P_KTRK] = 60, [P_LRATE] = 4, [P_LPITCH] = 12, [P_AVEL] = 30, [P_SPREAD] = 20, [P_DTIME] = 88,
                  [P_DFB] = 35, [P_DMIX] = 22}},
    {"PULSE PAD", W3_PAD, {OSC(0, 5, 20, 0, 40, 0, -6, 70), OSC(1, 5, 40, 0, 35, 0, 6, 70), ENV(A, 88, 95, 85, 95),
                   ENV(F, 85, 95, 70, 95), ENV(W, 0, 60, 100, 90), [P_CUT] = 86, [P_RES] = 5, [P_FENV] = 12,
                   [P_KTRK] = 40, [P_LRATE] = 2, [P_LPITCH] = 4, [P_SPREAD] = 90, [P_DTIME] = 100, [P_DFB] = 40,
                   [P_DMIX] = 28}},
    {"FEEDBACK", W3_PLUCK, {OSC(0, 8, 0, 50, 0, 0, 0, 80), OSC(1, 8, 20, 30, 0, -12, 4, 45), ENV(A, 0, 80, 25, 70),
                  ENV(F, 0, 75, 20, 70), ENV(W, 0, 75, 0, 70), [P_CUT] = 80, [P_RES] = 25, [P_FENV] = 30,
                  [P_KTRK] = 60, [P_AVEL] = 50, [P_FVEL] = 40, [P_SPREAD] = 50, [P_DTIME] = 72, [P_DFB] = 35,
                  [P_DMIX] = 22, [P_LRATE] = 3}},
    {"ADD HARM", W3_KEYS, {OSC(0, 9, 0, 50, 0, 0, 0, 75), OSC(1, 9, 30, -20, 0, 12, 2, 35), ENV(A, 0, 100, 40, 90),
                  ENV(F, 0, 95, 60, 90), ENV(W, 10, 105, 30, 95), [P_CUT] = 100, [P_KTRK] = 50, [P_AVEL] = 40,
                  [P_SPREAD] = 60, [P_DTIME] = 90, [P_DFB] = 35, [P_DMIX] = 25, [P_LRATE] = 3}},
    {"HP RESO", W3_FX, {OSC(0, 10, 0, 63, 0, 0, -4, 70), OSC(1, 10, 15, 50, 0, 0, 4, 55), ENV(A, 30, 95, 50, 85),
                 ENV(F, 30, 95, 50, 85), ENV(W, 60, 110, 30, 90), [P_CUT] = 96, [P_RES] = 10, [P_FENV] = 10,
                 [P_KTRK] = 50, [P_LRATE] = 2, [P_LCUT] = 10, [P_SPREAD] = 80, [P_DTIME] = 100, [P_DFB] = 45,
                 [P_DMIX] = 30}},
    {"GLASS PAD", W3_PAD, {OSC(0, 12, 10, 30, 15, 0, -3, 65), OSC(1, 12, 30, 20, 10, 12, 4, 40), OSC(2, 4, 0, 0, 0, 19, 0, 20),
                   ENV(A, 80, 95, 80, 100), ENV(F, 80, 95, 70, 100), ENV(W, 90, 105, 40, 100), [P_CUT] = 98,
                   [P_KTRK] = 40, [P_LRATE] = 2, [P_LPITCH] = 3, [P_SPREAD] = 100, [P_DTIME] = 110, [P_DFB] = 50,
                   [P_DMIX] = 32}},
    {"SQRSAW BRASS", W3_LEAD, {OSC(0, 15, 10, 30, 0, 0, -6, 70), OSC(1, 15, 30, 20, 0, 0, 6, 70), ENV(A, 40, 85, 75, 70),
                      ENV(F, 45, 85, 45, 70), ENV(W, 45, 90, 50, 70), [P_CUT] = 70, [P_RES] = 10, [P_FENV] = 40,
                      [P_KTRK] = 50, [P_AVEL] = 30, [P_FVEL] = 40, [P_LRATE] = 4, [P_LPITCH] = 5, [P_SPREAD] = 50,
                      [P_DTIME] = 80, [P_DFB] = 25, [P_DMIX] = 15}},
    {"POLATED", W3_PAD, {OSC(0, 17, 0, 63, 0, 0, -5, 70), OSC(1, 17, 63, -63, 0, 0, 5, 60), ENV(A, 90, 95, 85, 100),
                 ENV(F, 85, 95, 70, 100), ENV(W, 110, 115, 50, 105), [P_CUT] = 90, [P_RES] = 15, [P_FENV] = 10,
                 [P_KTRK] = 40, [P_LRATE] = 1, [P_LPITCH] = 3, [P_SPREAD] = 100, [P_DTIME] = 105, [P_DFB] = 50,
                 [P_DMIX] = 30}},
    {"STRONG KEYS", W3_KEYS, {OSC(0, 21, 0, 40, 0, 0, -3, 75), OSC(1, 21, 20, 20, 0, 12, 3, 35), ENV(A, 0, 98, 25, 80),
                     ENV(F, 0, 90, 35, 80), ENV(W, 0, 90, 0, 80), [P_CUT] = 92, [P_FENV] = 20, [P_KTRK] = 60,
                     [P_AVEL] = 60, [P_FVEL] = 50, [P_SPREAD] = 50, [P_DTIME] = 76, [P_DFB] = 25, [P_DMIX] = 18, [P_LRATE] = 3}},
    {"RESO PLUCK", W3_PLUCK, {OSC(0, 24, 0, 63, 0, 0, 0, 80), OSC(1, 24, 20, 40, 0, 12, 5, 35), ENV(A, 0, 78, 0, 70),
                    ENV(F, 0, 72, 0, 65), ENV(W, 0, 68, 0, 65), [P_CUT] = 78, [P_RES] = 30, [P_FENV] = 35,
                    [P_KTRK] = 60, [P_AVEL] = 50, [P_FVEL] = 40, [P_SPREAD] = 70, [P_DTIME] = 70, [P_DFB] = 50,
                    [P_DMIX] = 35, [P_LRATE] = 3}},
    {"SOFT BASS", W3_BASS, {OSC(0, 7, 20, -20, 0, -12, 0, 80), OSC(1, 7, 61, 0, 0, -24, 0, 50), ENV(A, 0, 80, 75, 50),
                   ENV(F, 0, 72, 20, 50), ENV(W, 0, 70, 0, 50), [P_CUT] = 48, [P_RES] = 10, [P_FENV] = 25,
                   [P_KTRK] = 40, [P_AVEL] = 40, [P_FVEL] = 25, [P_LRATE] = 3}},
    {"STRINGS", W3_PAD, {OSC(0, 6, 63, 0, 0, 0, -9, 60), OSC(1, 6, 63, 0, 0, 0, 9, 60), OSC(2, 6, 63, 0, 0, 12, 0, 30),
                 ENV(A, 88, 95, 90, 95), ENV(F, 80, 95, 80, 95), ENV(W, 0, 60, 100, 60), [P_CUT] = 84, [P_RES] = 5,
                 [P_FENV] = 10, [P_KTRK] = 40, [P_LRATE] = 5, [P_LPITCH] = 5, [P_SPREAD] = 100, [P_DTIME] = 100,
                 [P_DFB] = 35, [P_DMIX] = 25}},
    {"GLASS BELLS", W3_BELLS, {OSC(0, 4, 20, 20, 0, 0, 0, 70), OSC(1, 12, 0, 30, 0, 24, 5, 40), OSC(2, 2, 10, 0, 0, 7, 0, 25),
                     ENV(A, 0, 108, 0, 105), ENV(F, 0, 100, 30, 100), ENV(W, 0, 95, 0, 95), [P_CUT] = 108,
                     [P_KTRK] = 60, [P_AVEL] = 60, [P_FVEL] = 30, [P_SPREAD] = 80, [P_DTIME] = 96, [P_DFB] = 45,
                     [P_DMIX] = 32, [P_LRATE] = 3}},
    {"SUPER SAW", W3_LEAD, {OSC(0, 6, 63, 0, 0, 0, 0, 80), OSC(1, 6, 63, 0, 0, 12, 0, 30), ENV(A, 40, 90, 85, 85),
                   ENV(F, 40, 90, 60, 85), ENV(W, 0, 60, 100, 60), [P_CUT] = 84, [P_RES] = 8, [P_FENV] = 15,
                   [P_KTRK] = 50, [P_AVEL] = 25, [P_SPREAD] = 100, [P_DTIME] = 92, [P_DFB] = 35, [P_DMIX] = 22,
                   [P_UNI] = 4, [P_UDET] = 22, [P_LRATE] = 3}},
    {"UNI LEAD", W3_LEAD, {OSC(0, 23, 10, 40, 0, 0, 0, 75), OSC(1, 15, 20, 20, 0, -12, 0, 45), ENV(A, 5, 85, 75, 70),
                  ENV(F, 5, 85, 50, 70), ENV(W, 30, 100, 40, 80), [P_CUT] = 82, [P_RES] = 20, [P_FENV] = 25,
                  [P_KTRK] = 60, [P_LRATE] = 5, [P_LPITCH] = 10, [P_AVEL] = 30, [P_SPREAD] = 80, [P_DTIME] = 85,
                  [P_DFB] = 35, [P_DMIX] = 22, [P_UNI] = 3, [P_UDET] = 14}},
    {"PERC ORGAN", W3_KEYS, {OSC(0, 22, 0, 20, 0, 0, 0, 75), OSC(1, 14, 10, 0, 0, 12, 0, 40), ENV(A, 0, 75, 60, 45),
                             ENV(F, 0, 70, 70, 45), ENV(W, 0, 60, 0, 45), [P_CUT] = 98, [P_KTRK] = 40, [P_AVEL] = 30,
                             [P_SPREAD] = 50, [P_DTIME] = 64, [P_DFB] = 15, [P_DMIX] = 12, [P_LRATE] = 3}},
    {"CLAV WAVE", W3_KEYS, {OSC(0, 13, 0, 40, 0, 0, 0, 100), OSC(1, 13, 10, 30, 0, 12, 5, 45), ENV(A, 0, 86, 25, 55), ENV(F, 0, 70, 10, 50),
                            ENV(W, 0, 65, 0, 50), [P_CUT] = 90, [P_RES] = 15, [P_FENV] = 30, [P_KTRK] = 50,
                            [P_AVEL] = 60, [P_FVEL] = 50, [P_SPREAD] = 40, [P_DTIME] = 60, [P_DFB] = 15, [P_DMIX] = 10, [P_LRATE] = 3}},
    {"SUB WAVE", W3_BASS, {OSC(0, 6, 61, 0, 0, -12, 0, 85), OSC(1, 7, 0, 10, 0, -12, 3, 40), ENV(A, 0, 80, 85, 50),
                           ENV(F, 0, 70, 40, 50), ENV(W, 0, 70, 0, 50), [P_CUT] = 45, [P_RES] = 5, [P_FENV] = 15,
                           [P_KTRK] = 40, [P_AVEL] = 40, [P_LRATE] = 3}},
    {"PLUCK BASS", W3_BASS, {OSC(0, 0, 40, -30, 0, -12, 0, 80), ENV(A, 0, 72, 30, 45), ENV(F, 0, 62, 0, 45),
                             ENV(W, 0, 58, 0, 45), [P_CUT] = 42, [P_RES] = 20, [P_FENV] = 40, [P_KTRK] = 50,
                             [P_AVEL] = 50, [P_FVEL] = 30, [P_LRATE] = 3}},
    {"VOX BASS", W3_BASS, {OSC(0, 27, 0, 30, 0, -12, 0, 75), OSC(1, 16, 10, 0, 0, -24, 0, 40), ENV(A, 5, 80, 80, 55),
                           ENV(F, 5, 75, 50, 55), ENV(W, 10, 85, 40, 60), [P_CUT] = 50, [P_RES] = 15, [P_FENV] = 20,
                           [P_KTRK] = 40, [P_AVEL] = 40, [P_LRATE] = 3}},
    {"MALLET", W3_BELLS, {OSC(0, 2, 0, 50, 0, 0, 0, 80), OSC(1, 9, 10, 20, 0, 12, 4, 35), ENV(A, 0, 92, 0, 90),
                          ENV(F, 0, 85, 10, 85), ENV(W, 0, 75, 0, 75), [P_CUT] = 104, [P_KTRK] = 70, [P_AVEL] = 60,
                          [P_FVEL] = 30, [P_SPREAD] = 70, [P_DTIME] = 84, [P_DFB] = 40, [P_DMIX] = 28, [P_LRATE] = 3}},
    {"TINE BELL", W3_BELLS, {OSC(0, 21, 30, 30, 0, 0, 0, 70), OSC(1, 4, 0, 0, 0, 24, 0, 40), ENV(A, 0, 100, 0, 98),
                             ENV(F, 0, 95, 20, 95), ENV(W, 0, 85, 0, 85), [P_CUT] = 110, [P_KTRK] = 60, [P_AVEL] = 50,
                             [P_SPREAD] = 80, [P_DTIME] = 96, [P_DFB] = 45, [P_DMIX] = 30, [P_LRATE] = 3}},
    {"HARM BELLS", W3_BELLS, {OSC(0, 24, 10, 40, 0, 0, 0, 65), OSC(1, 12, 30, 30, 0, 19, 3, 40),
                              OSC(2, 4, 0, 0, 0, 12, 0, 25), ENV(A, 0, 104, 0, 102), ENV(F, 0, 100, 30, 100),
                              ENV(W, 0, 95, 0, 95), [P_CUT] = 106, [P_KTRK] = 60, [P_AVEL] = 50, [P_SPREAD] = 90,
                              [P_DTIME] = 100, [P_DFB] = 50, [P_DMIX] = 32, [P_LRATE] = 3}},
    {"HARP WAVE", W3_PLUCK, {OSC(0, 9, 0, 50, 0, 0, 0, 75), OSC(1, 6, 63, 0, 0, 12, 3, 30), ENV(A, 0, 88, 0, 85),
                             ENV(F, 0, 80, 0, 80), ENV(W, 0, 75, 0, 75), [P_CUT] = 86, [P_RES] = 10, [P_FENV] = 30,
                             [P_KTRK] = 60, [P_AVEL] = 50, [P_FVEL] = 40, [P_SPREAD] = 80, [P_DTIME] = 88, [P_DFB] = 40,
                             [P_DMIX] = 28, [P_LRATE] = 3}},
    {"SYNC PLUCK", W3_PLUCK, {OSC(0, 10, 0, 63, 0, 0, 0, 95), OSC(1, 1, 20, 40, 0, 7, 0, 55), ENV(A, 0, 82, 10, 70),
                              ENV(F, 0, 72, 10, 66), ENV(W, 0, 66, 0, 64), [P_CUT] = 88, [P_RES] = 25, [P_FENV] = 35,
                              [P_KTRK] = 60, [P_AVEL] = 50, [P_FVEL] = 40, [P_SPREAD] = 60, [P_DTIME] = 70, [P_DFB] = 45,
                              [P_DMIX] = 30, [P_LRATE] = 3}},
    {"SEA SPRAY", W3_FX, {OSC(0, 11, 20, 30, 40, 0, 0, 40), ENV(A, 95, 100, 80, 105), ENV(F, 95, 100, 40, 105),
                          ENV(W, 100, 105, 40, 105), [P_NOISE] = 60, [P_CUT] = 70, [P_RES] = 45, [P_FENV] = 30,
                          [P_LRATE] = 0, [P_LCUT] = 40, [P_SPREAD] = 100, [P_DTIME] = 110, [P_DFB] = 60, [P_DMIX] = 40}},
    {"DATA STREAM", W3_FX, {OSC(0, 20, 0, 0, 63, 24, 0, 60), OSC(1, 17, 0, 0, 63, 31, 0, 40), ENV(A, 0, 80, 80, 70),
                            ENV(F, 0, 80, 80, 70), ENV(W, 0, 80, 100, 70), [P_CUT] = 96, [P_RES] = 20, [P_LRATE] = 10,
                            [P_SPREAD] = 80, [P_DTIME] = 70, [P_DFB] = 50, [P_DMIX] = 30}},
    {"TIDE RISER", W3_FX, {OSC(0, 6, 0, 63, 0, 0, -10, 60), OSC(1, 6, 0, 63, 0, 0, 10, 60), OSC(2, 6, 0, 63, 0, 12, 0, 30),
                           ENV(A, 100, 110, 90, 105), ENV(F, 110, 110, 90, 105), ENV(W, 118, 118, 100, 105),
                           [P_CUT] = 50, [P_RES] = 25, [P_FENV] = 60, [P_KTRK] = 40, [P_SPREAD] = 100, [P_DTIME] = 100,
                           [P_DFB] = 55, [P_DMIX] = 35, [P_LRATE] = 3}},
    {"INIT", W3_INIT, {OSC(0, 6, 63, 0, 0, 0, 0, 80), ENV(A, 0, 64, 100, 50), ENV(F, 0, 64, 100, 50),
                       ENV(W, 0, 64, 100, 50), [P_CUT] = 127, [P_KTRK] = 50, [P_AVEL] = 50, [P_LRATE] = 4,
                       [P_DTIME] = 64}},   /* one saw, open */
};
#define W3_NPRESET (sizeof W3_PRESET / sizeof *W3_PRESET)

/* the panel's: one writer (the main loop), read by the render */
static volatile int8_t w3_p[W3_NP];
static volatile uint8_t w3_p_panic;
static volatile uint16_t w3_q[32];               /* key | velocity << 7 (0: off) */
static volatile uint32_t w3_qw, w3_qr;

typedef struct {
    float ph[3], inc[3], wf[3], n[3];
    const int8_t *wa[3], *wb[3];
    float y1, y2, y3, y4, g, k, amp, env[3], vel, gl, gr, det, upan, ug;   /* det: semitones; upan -1..1; ug: unison gain */
    uint32_t age, rng;
    uint8_t st[3], key, gate;                    /* st: 0 off, 1 attack, 2 decay / sustain, 3 release */
} w3_voice_t;

static w3_voice_t w3_v[W3_NV];
static int16_t w3_dl[W3_DLY], w3_dr[W3_DLY];
static uint32_t w3_dw, w3_age;
static float w3_lph, w3_dlp;

static void w3_load(uint32_t i)
{
    uint32_t k;
    for (k = 0; k < W3_NP; k++)
        w3_p[k] = W3_PRESET[i].p[k];
    if (w3_p[P_UNI] < 1)
        w3_p[P_UNI] = 1;                         /* (a preset that does not say: one voice a key) */
}

static void w3_note(uint32_t key, uint32_t vel)  /* main loop: queued for the render */
{
    if (w3_qw - w3_qr < 32u) {
        w3_q[w3_qw & 31u] = (uint16_t)((key & 127u) | (vel & 127u) << 7);
        w3_qw++;
    }
}

static uint32_t w3_poly = W3_NV1;               /* voices in use: W3_NV with the second core, else W3_NV1 (the render's) */

static void w3_key(uint32_t key, uint32_t vel)
{
    static const float UG[4] = {1.0f, 0.71f, 0.58f, 0.5f};   /* unison: about the level of one voice */
    uint32_t k, u, nu = (uint32_t)w3_p[P_UNI], taken = 0;
    if (!vel) {
        for (k = 0; k < W3_NV; k++)
            if (w3_v[k].key == key && w3_v[k].gate)
                w3_v[k].gate = 0;
        return;
    }
    nu = nu < 1u ? 1u : nu > 4u ? 4u : nu;
    for (u = 0; u < nu; u++) {
        uint32_t best = 0, o;
        int32_t score = -1;
        float x = nu > 1u ? (float)u * 2.0f / (float)(nu - 1u) - 1.0f : 0;   /* -1 .. 1 across the stack */
        w3_voice_t *v;
        for (k = 0; k < w3_poly; k++) {          /* the same key again, else a silent voice, else the oldest released,
                                                  * else the oldest */
            int32_t sc;
            v = &w3_v[k];
            if (taken >> k & 1u)
                continue;
            sc = v->key == key && v->st[0] ? 0x40000000 : !v->st[0] ? 0x30000000 : 0;
            sc += (int32_t)(!v->gate) << 28;
            sc += (int32_t)((w3_age - v->age) & 0x0FFFFFFFu);
            if (sc > score) { score = sc; best = k; }
        }
        taken |= 1u << best;
        v = &w3_v[best];
        if (!v->st[0])
            for (o = 0; o < 3u; o++) v->ph[o] = 0.25f * (float)u, v->env[o] = 0;   /* a stack starts out of phase */
        if (!v->st[0])
            v->y1 = v->y2 = v->y3 = v->y4 = 0;   /* (a silent voice: nothing of its last note in the filter) */
        v->key = (uint8_t)key;
        v->vel = (float)vel * (1.0f / 127.0f);
        v->det = x * (float)w3_p[P_UDET] * 0.01f;
        v->upan = nu > 1u ? x : (float)(key * 5u % 12u) * (2.0f / 11.0f) - 1.0f;   /* by the key, not the voice it got:
                                                  * the semitones scattered across the stereo */
        v->ug = UG[nu - 1u];
        v->gate = 1;
        v->age = ++w3_age;
        for (o = 0; o < 3u; o++) v->st[o] = 1;
    }
}

static float w3_secs(int32_t t) { return 0.001f * fm_exp2f((float)t * (13.2877f / 127.0f)); }   /* 1 ms .. 10 s */
static float w3_coef(float secs) { return 1.0f - fm_expf(-W3_CTL_DT / secs); }

/* one control step of a voice's three envelopes: c[env][0 attack, 1 decay, 2 release], s[env] the sustain */
static void w3_envs(w3_voice_t *v, const float c[3][3], const float s[3])
{
    uint32_t e;
    for (e = 0; e < 3u; e++) {
        float x = v->env[e];
        if (v->st[e] && v->st[e] < 3u && !v->gate)
            v->st[e] = 3;
        switch (v->st[e]) {
        case 1: x += (1.3f - x) * c[e][0]; if (x >= 1.0f) { x = 1.0f; v->st[e] = 2; } break;
        case 2: x = s[e] + (x - s[e]) * (1.0f - c[e][1]); break;
        case 3: x *= 1.0f - c[e][2]; if (x < 0.0001f) { x = 0; v->st[e] = 0; } break;
        default: break;
        }
        v->env[e] = x;
    }
}

static inline float w3_read(const int8_t *w, float x)
{
    int32_t i = (int32_t)x;
    float a = (float)w[i];
    return a + ((float)w[i + 1] - a) * (x - (float)i);
}

/* ---- a render: core 0 prepares what all voices share (w3_ctl), the voices are split between the two cores (the
 * odd ones to core 1 when the host lends it: w3_c1_run / w3_c1_wait, else all here), then the sum, the delay */
#define W3_MAXN 256u                             /* frames a pass (a longer render takes several) */
#define W3_MAXB (W3_MAXN / W3_CTL)
static struct {
    float c[3][3], s[3], lvl[3], lfo[W3_MAXB], cut0, res, noise, mot;   /* mot: LFO AMT, 0..1 */
    uint32_t n;
} w3_ctl;
static float w3_mix[2][2u * W3_MAXN];            /* each core's voices, L R */
static int (*w3_c1_run)(void (*fn)(uint32_t), uint32_t arg);   /* the module's: the host's second core, or 0 */
static int (*w3_c1_wait)(void);

/* voices first, first + step, ... into w3_mix[first & 1] */
static void w3_voices(uint32_t first, uint32_t step)
{
    const volatile int8_t *p = w3_p;
    float *mix = w3_mix[first & 1u];
    uint32_t i, k, o, b, n = w3_ctl.n;
    for (i = 0; i < 2u * n; i++) mix[i] = 0;
    for (k = first; k < W3_NV; k += step) {
        w3_voice_t *v = &w3_v[k];
        float pan = (float)p[P_SPREAD] * 0.01f * v->upan;
        float *mx = mix;
        if (!v->st[0] && v->amp == 0)
            continue;
        v->gl = 1.0f - fm_maxf(pan, 0);
        v->gr = 1.0f + fm_minf(pan, 0);
        for (b = 0; b * W3_CTL < n; b++) {
            uint32_t m = n - b * W3_CTL < W3_CTL ? n - b * W3_CTL : W3_CTL;
            float lfo = w3_ctl.lfo[b], a0 = v->amp, a1, da, oct, fc, y1, y2, y3, y4, g, kk = w3_ctl.res, comp;
            w3_envs(v, w3_ctl.c, w3_ctl.s);
            for (o = 0; o < 3u; o++) {
                int32_t pos, lv = 0;
                float semi = (float)((int32_t)v->key - 69 + p[P_OSC(o, O_SEMI)]) + (float)p[P_OSC(o, O_FINE)] * 0.01f + v->det +
                             lfo * ((float)p[P_LPITCH] * 0.01f + w3_ctl.mot * 0.12f), f, hmax;
                const w3_pos_t *e;
                if (w3_ctl.lvl[o] == 0) continue;
                f = 440.0f * fm_exp2f(semi * (1.0f / 12.0f));
                v->inc[o] = f * (1.0f / W3_FS);
                hmax = 18000.0f / f;
                while (lv < 5 && (float)W3_LVL_H[lv] > hmax) lv++;
                pos = p[P_OSC(o, O_POS)] + (int32_t)(v->env[2] * (float)p[P_OSC(o, O_ENV)] +
                                                     lfo * (float)p[P_OSC(o, O_LFO)] * 0.5f);
                if (pos <= 60) {                 /* LFO AMT moves a wave within its table (it leaves the triangle,
                                                  * square and saw be, and stops short of them) */
                    pos += (int32_t)(lfo * w3_ctl.mot * 18.0f);
                    pos = pos > 60 ? 60 : pos;
                }
                pos = pos < 0 ? 0 : pos > 63 ? 63 : pos;   /* the PPG's steps: no position between two */
                e = &W3_TAB[p[P_OSC(o, O_TAB)]][pos];
                v->wa[o] = W3_WAVE + e->a * W3_WAVE_BYTES + W3_LVL_AT[lv];
                v->wb[o] = W3_WAVE + e->b * W3_WAVE_BYTES + W3_LVL_AT[lv];
                v->wf[o] = (float)e->f * (1.0f / 256.0f);
                v->n[o] = (float)W3_LVL_N[lv];
            }
            oct = w3_ctl.cut0 + v->env[1] * (float)p[P_FENV] * 0.08f +
                  (float)((int32_t)v->key - 60) * (float)p[P_KTRK] * (1.0f / 1200.0f) +
                  (v->vel - 1.0f) * (float)p[P_FVEL] * 0.04f + lfo * ((float)p[P_LCUT] * 0.03f + w3_ctl.mot * 1.2f);
            fc = fm_exp2f(fm_clampf(oct, 4.0f, 14.0f));
            fc = fc > 16000.0f ? 16000.0f : fc;
            g = 1.0f - fm_expf(-FM_TWO_PI * fc / W3_FS);
            comp = 1.0f + 0.4f * kk;             /* what the resonance takes from the bass, back */
            a1 = v->st[0] ? v->env[0] * v->ug * (1.0f - (float)p[P_AVEL] * 0.01f * (1.0f - v->vel)) : 0;
            da = (a1 - a0) * (1.0f / (float)W3_CTL);
            y1 = v->y1; y2 = v->y2; y3 = v->y3; y4 = v->y4;
            for (i = 0; i < m; i++) {
                float x = 0;
                for (o = 0; o < 3u; o++) {
                    float ph, xa, sm;
                    if (w3_ctl.lvl[o] == 0) continue;
                    ph = v->ph[o] + v->inc[o];
                    if (ph >= 1.0f) ph -= 1.0f;
                    v->ph[o] = ph;
                    xa = ph * v->n[o];
                    sm = w3_read(v->wa[o], xa);
                    if (v->wf[o] != 0) sm += (w3_read(v->wb[o], xa) - sm) * v->wf[o];
                    x += sm * w3_ctl.lvl[o];
                }
                if (w3_ctl.noise != 0) {
                    v->rng = v->rng * 1664525u + 1013904223u;
                    x += (float)(int16_t)(v->rng >> 16) * w3_ctl.noise;
                }
                x = x * comp - kk * fm_tanhf(y4);
                y1 += (x - y1) * g;
                y2 += (y1 - y2) * g;
                y3 += (y2 - y3) * g;
                y4 += (y3 - y4) * g;
                a0 += da;
                mx[2u * i] += y4 * a0 * v->gl;
                mx[2u * i + 1u] += y4 * a0 * v->gr;
            }
            mx += 2u * m;
            v->y1 = fm_flush(y1); v->y2 = fm_flush(y2); v->y3 = fm_flush(y3); v->y4 = fm_flush(y4);
            v->amp = a1 < 0.00001f ? 0 : a1;
        }
    }
}

static void w3_c1_job(uint32_t arg) { (void)arg; w3_voices(1, 2); }

static void w3_pass(int32_t *out, uint32_t n)
{
    const volatile int8_t *p = w3_p;
    uint32_t i, k, b, dtime, split;
    float dfb, dmix;
    for (k = 0; k < 3u; k++) {
        w3_ctl.c[k][0] = w3_coef(w3_secs(p[P_AA + 4 * k]) * 0.68f);   /* the attack aims past 1: there at the time */
        w3_ctl.c[k][1] = w3_coef(w3_secs(p[P_AD + 4 * k]) * 0.33f);   /* decay and release: within 5 % at the time */
        w3_ctl.c[k][2] = w3_coef(w3_secs(p[P_AR + 4 * k]) * 0.33f);
        w3_ctl.s[k] = (float)p[P_AS + 4 * k] * 0.01f;
        w3_ctl.lvl[k] = (float)p[P_OSC(k, O_LVL)] * (0.01f / 127.0f / 3.0f);
    }
    for (b = 0; b * W3_CTL < n; b++) {
        w3_lph += w3_bpm * (1.0f / 60.0f) / W3_DIV_BEATS[p[P_LRATE] < (int8_t)W3_NDIV ? p[P_LRATE] : 0] * W3_CTL_DT;
        if (w3_lph >= 1.0f) w3_lph -= 1.0f;
        w3_ctl.lfo[b] = w3_lph < 0.5f ? 4.0f * w3_lph - 1.0f : 3.0f - 4.0f * w3_lph;
    }
    w3_ctl.cut0 = 4.3219f + (float)p[P_CUT] * (9.9658f / 127.0f);   /* log2 Hz: 20 Hz .. 20 kHz */
    w3_ctl.res = (float)p[P_RES] * 0.039f;
    w3_ctl.mot = (float)p[P_LAMT] * 0.01f;      /* LFO AMT: movement on top of the preset's own, at its LFO's rate: up
                                                 * to 18 steps of every wave, 1.2 octaves of cutoff, 12 cents of pitch */
    w3_ctl.noise = (float)p[P_NOISE] * (0.3f / 100.0f / 32768.0f);
    w3_ctl.n = n;
    split = w3_c1_run && w3_c1_run(w3_c1_job, 0);
    w3_poly = split ? W3_NV : W3_NV1;
    if (!split)                                  /* one core: the voices past W3_NV1 cut (a job is not out: safe) */
        for (k = W3_NV1; k < W3_NV; k++)
            w3_v[k].st[0] = w3_v[k].st[1] = w3_v[k].st[2] = 0, w3_v[k].amp = 0, w3_v[k].gate = 0;
    w3_voices(0, split ? 2u : 1u);
    if (split && w3_c1_wait() != 0)
        w3_voices(1, 2);                         /* core 1 ran late (it is held now): its voices here, from where it left them */
    /* the ping-pong delay: the mono sum into the left line, the left into the right, the right back into the left;
     * a one-pole in its loop, as a tape's */
    dtime = 441u + (uint32_t)p[P_DTIME] * ((W3_DLY - 442u) / 127u);
    dfb = (float)p[P_DFB] * 0.0085f;
    dmix = (float)p[P_DMIX] * 0.01f;
    for (i = 0; i < n; i++) {
        uint32_t r = (w3_dw + W3_DLY - dtime) % W3_DLY;
        float bl = w3_mix[0][2u * i] + (split ? w3_mix[1][2u * i] : 0), br = w3_mix[0][2u * i + 1u] + (split ? w3_mix[1][2u * i + 1u] : 0);
        float dl = (float)w3_dl[r] * (1.0f / 16384.0f), dr = (float)w3_dr[r] * (1.0f / 16384.0f), l, rr;
        w3_dlp += ((bl + br) * 0.5f + dr * dfb - w3_dlp) * 0.45f;
        w3_dl[w3_dw] = (int16_t)(fm_clampf(w3_dlp, -1.99f, 1.99f) * 16384.0f);
        w3_dr[w3_dw] = (int16_t)(fm_clampf(dl * dfb, -1.99f, 1.99f) * 16384.0f);
        if (++w3_dw == W3_DLY) w3_dw = 0;
        l = fm_tanhf((bl + dl * dmix) * 0.9f);
        rr = fm_tanhf((br + dr * dmix) * 0.9f);
        out[2u * i] += (int32_t)(l * 32000.0f);
        out[2u * i + 1u] += (int32_t)(rr * 32000.0f);
    }
}

static void w3_render(int32_t *out, uint32_t n)
{
    uint32_t k;
    if (w3_p_panic) {
        w3_p_panic = 0;
        for (k = 0; k < W3_NV; k++) w3_v[k].gate = 0, w3_v[k].st[0] = w3_v[k].st[1] = w3_v[k].st[2] = 0;
    }
    while (w3_qr != w3_qw) {
        uint32_t e = w3_q[w3_qr & 31u];
        w3_qr++;
        w3_key(e & 127u, e >> 7);
    }
    while (n) {
        uint32_t m = n < W3_MAXN ? n : W3_MAXN;
        w3_pass(out, m);
        out += 2u * m;
        n -= m;
    }
}
