/* SPDX-License-Identifier: GPL-3.0-only */
/* M-3rd-VAVE: a wavetable polysynth after the Groove Synthesis 3rd Wave, on the PPG Wave 2.2's own wavetables
 * (eng/w3_engine.c; eng/w3_tables.h made by gen_tables.py). 24 voices with the host's second core (12 without), up
 * to 4 a key in unison. Its panel is laid out as PRO-FM-78's (and its players are PRO-FM-78's: w3_play.inc):
 *   keys / MIDI          notes (the keys' force as velocity); OCT- / OCT+ on the top line
 *   ALGORITHM            the type: ALL, PAD, KEYS, BASS, LEAD, BELLS, PLUCK, FX, INIT
 *   PRESETS, SELECT      the type's presets
 *   HOME                 the home page (and at power-on): cutoff, resonance, LFO amount, amp release on
 *                        KNOB 1..4; held 1.5 s the host's module list
 *   FX SEL ENV LFO EDIT  a page of four controls on KNOB 1..4, in their units (again: the section's next page, after
 *                        its last HOME): FX the mixer, delay, voices (unison); SEL the chord generator; ENV the filter,
 *                        amp and wave envelopes; LFO the LFO and its wave modulation; EDIT the filter, oscillators,
 *                        fine tuning
 *   ARP                  the arpeggiator (mode, rate, octaves, gate; again: tempo, swing, latch, velocity)
 *   SEQ                  the loop recorder (tempo, bars, click, transpose); PLAY starts / stops it; REC stopped arms it,
 *                        playing records over it (again: stops), held 1 s clears it; SAVE keeps the loop in flash
 *   CC 74 71 73 72       cutoff, resonance, amp attack, amp release
 * The preset, its edits, the players' settings and the octave are kept over a power-off (the host's settings record,
 * handed over once nothing sounds). The sea above the program rolls on every page, and rises with the sound: three
 * waves. The LFO's rate is a beat division of the tempo (TEMPO's, or a DAW's MIDI clock). */
#include <stdint.h>
#include "hb_module.h"
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, __SIZE_TYPE__ n);
int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);
#include "w3_engine.c"

static const hb_host_t *H;
static void str_cpy(char *d, const char *s, unsigned max)
{
    unsigned i = 0;
    while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    if (max) d[i] = 0;
}
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint32_t noise32(int32_t *st)
{
    uint32_t s = (uint32_t)*st;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *st = (int32_t)s;
    return s;
}
#define CTL 128u                                /* frames a players' step: the render in two pieces a block (each a
                                                 * handoff to core 1: fewer, less of its overhead) */
#include "w3_play.inc"

/* ---- the queue: main loop (note, midi, ui) -> audio interrupt */
enum { Q_KEY, Q_ALL_OFF, Q_CLK, Q_START, Q_CONT, Q_STOP, Q_PLAY, Q_REC, Q_CLEAR };
#define QN 64u
static uint32_t q[QN];
static volatile uint32_t q_w, q_r;
static void put(uint32_t kind, uint32_t a, uint32_t b)
{
    if (q_w - q_r < QN) {                       /* (full: dropped) */
        q[q_w % QN] = kind | a << 8 | b << 16;
        q_w = q_w + 1u;
    }
}
static void take(uint32_t m)                    /* one queued event, in the audio interrupt */
{
    uint32_t a = m >> 8 & 0xFFu, b = m >> 16;
    switch (m & 0xFFu) {
    case Q_KEY: key_in(a, b); break;
    case Q_ALL_OFF: players_all_off(); w3_p_panic = 1; break;
    case Q_CLK: clock_in(); break;
    case Q_START: clock_start(1); break;
    case Q_CONT: clock_start(0); break;
    case Q_STOP: if (playing) seq_stop(); break;
    case Q_PLAY: if (playing) seq_stop(); else seq_start(); break;
    case Q_REC:
        if (rec == R_ON) { rec_close(); rec = R_OFF; }
        else rec = rec == R_ARMED ? R_OFF : playing ? R_ON : R_ARMED;
        break;
    case Q_CLEAR: seq_clear(); break;
    }
}

static volatile int32_t out_peak;               /* the last block's peak: the home page's waves rise with it */
static void render(int32_t *out, uint32_t n)
{
    uint32_t i, at, m;
    int32_t pk = 0;
    while (q_r != q_w) {
        take(q[q_r % QN]);
        q_r = q_r + 1u;
    }
    w3_bpm = TICK_S / spt();                    /* the LFO's beat: TEMPO's, or a MIDI clock's */
    for (at = 0; at < n; at += m) {
        m = n - at < CTL ? n - at : CTL;
        clock_block();
        w3_render(out + 2u * at, m);
        now_s += m;
        for (i = 0; i < m; i++) {
            int32_t s = click_sample(), *o = out + 2u * (at + i), a = o[0] < 0 ? -o[0] : o[0];
            o[0] = clamp(o[0] + s, -32767, 32767);
            o[1] = clamp(o[1] + s, -32767, 32767);
            if (a > pk) pk = a;
        }
    }
    out_peak = pk;
}
static uint32_t sounding(void)                  /* voices still sounding, release tails too */
{
    uint32_t k, n = 0;
    for (k = 0; k < W3_NV; k++) n += w3_v[k].st[0] != 0;
    return n;
}

/* ---- the presets by type: ALGORITHM a type, PRESETS its presets (each type keeps its place) */
#define TN (W3_NTYPE + 1u)                      /* ALL, then W3_TYPE_NAME's (INIT last) */
static uint8_t members[2u * W3_NPRESET], t_start[TN], t_count[TN], t_pos[TN];
static uint32_t type, want;                     /* the type shown, the preset sounding */
static uint32_t edited;                         /* a control turned since the preset was picked */
static const char *type_name(uint32_t t) { return t ? W3_TYPE_NAME[t - 1u] : "ALL"; }
static void types_init(void)
{
    uint32_t t, k, n = 0;
    for (t = 0; t < TN; t++) {
        t_start[t] = (uint8_t)n;
        for (k = 0; k < W3_NPRESET; k++)
            if (t ? W3_PRESET[k].type == t - 1u : W3_PRESET[k].type != W3_INIT)
                members[n++] = (uint8_t)k;
        t_count[t] = (uint8_t)(n - t_start[t]);
    }
}
static void pick(uint32_t k)
{
    want = k;
    edited = 0;
    w3_load(k);
}
static void pick_in_type(void) { pick(members[t_start[type] + t_pos[type]]); }

/* ---- the controls: M-3rd-VAVE's parameters (w3_p) and the players' settings (gen[]), each shown in its unit */
enum { U_NUM, U_PCT, U_CUT, U_TIME, U_TABLE, U_WAVE, U_SEMI, U_CENTS, U_LFO, U_DLY, U_UNI,
       U_CHORD, U_INV, U_VOICING, U_STRUM, U_AMODE, U_RATE, U_AOCT, U_AGATE, U_TEMPO, U_SWING, U_LATCH, U_VELO, U_BARS,
       U_CLICK, U_TRANSP };                    /* (U_CHORD ..: the players', gen[], not the preset's) */
typedef struct { const char *label; uint8_t idx, unit; } ctl_t;
typedef struct { const char *title; uint8_t btn; ctl_t c[4]; } page_t;
#define NO_BTN 0xFFu
#define NONE {0, 0, 0}
static const page_t PAGES[] = {                 /* a section's pages next to each other: a press the next, after its last HOME */
    {"HOME", NO_BTN, {{"CUTOFF", P_CUT, U_CUT}, {"RESO", P_RES, U_PCT}, {"LFO AMT", P_LAMT, U_PCT},
                      {"RELEASE", P_AR, U_TIME}}},
    {"FILTER", HB_BTN_EDIT, {{"CUTOFF", P_CUT, U_CUT}, {"RESO", P_RES, U_PCT}, {"ENV AMT", P_FENV, U_NUM},
                             {"KEYBD", P_KTRK, U_PCT}}},
    {"OSCILLATOR 1", HB_BTN_EDIT, {{"TABLE", P_OSC(0, O_TAB), U_TABLE}, {"WAVE", P_OSC(0, O_POS), U_WAVE},
                                   {"W.ENV", P_OSC(0, O_ENV), U_NUM}, {"SEMI", P_OSC(0, O_SEMI), U_SEMI}}},
    {"OSCILLATOR 2", HB_BTN_EDIT, {{"TABLE", P_OSC(1, O_TAB), U_TABLE}, {"WAVE", P_OSC(1, O_POS), U_WAVE},
                                   {"W.ENV", P_OSC(1, O_ENV), U_NUM}, {"SEMI", P_OSC(1, O_SEMI), U_SEMI}}},
    {"OSCILLATOR 3", HB_BTN_EDIT, {{"TABLE", P_OSC(2, O_TAB), U_TABLE}, {"WAVE", P_OSC(2, O_POS), U_WAVE},
                                   {"W.ENV", P_OSC(2, O_ENV), U_NUM}, {"SEMI", P_OSC(2, O_SEMI), U_SEMI}}},
    {"FINE TUNE", HB_BTN_EDIT, {{"OSC 1", P_OSC(0, O_FINE), U_CENTS}, {"OSC 2", P_OSC(1, O_FINE), U_CENTS},
                                {"OSC 3", P_OSC(2, O_FINE), U_CENTS}, NONE}},
    {"CHORD", HB_BTN_SEL, {{"CHORD", G_CHORD, U_CHORD}, {"INVERT", G_INV, U_INV}, {"VOICING", G_VOICING, U_VOICING},
                           {"STRUM", G_STRUM, U_STRUM}}},
    {"FILTER ENVELOPE", HB_BTN_ENV, {{"ATTACK", P_FA, U_TIME}, {"DECAY", P_FD, U_TIME}, {"SUSTAIN", P_FS, U_PCT},
                                     {"RELEASE", P_FR, U_TIME}}},
    {"AMPLIFIER ENVELOPE", HB_BTN_ENV, {{"ATTACK", P_AA, U_TIME}, {"DECAY", P_AD, U_TIME}, {"SUSTAIN", P_AS, U_PCT},
                                        {"RELEASE", P_AR, U_TIME}}},
    {"WAVE ENVELOPE", HB_BTN_ENV, {{"ATTACK", P_WA, U_TIME}, {"DECAY", P_WD, U_TIME}, {"SUSTAIN", P_WS, U_PCT},
                                   {"RELEASE", P_WR, U_TIME}}},
    {"MIXER", HB_BTN_FX, {{"OSC 1", P_OSC(0, O_LVL), U_PCT}, {"OSC 2", P_OSC(1, O_LVL), U_PCT},
                          {"OSC 3", P_OSC(2, O_LVL), U_PCT}, {"NOISE", P_NOISE, U_PCT}}},
    {"DELAY", HB_BTN_FX, {{"TIME", P_DTIME, U_DLY}, {"FEEDBACK", P_DFB, U_PCT}, {"MIX", P_DMIX, U_PCT},
                          {"SPREAD", P_SPREAD, U_PCT}}},
    {"VOICES", HB_BTN_FX, {{"UNISON", P_UNI, U_UNI}, {"DETUNE", P_UDET, U_CENTS}, {"AMP VEL", P_AVEL, U_PCT},
                           {"FILT VEL", P_FVEL, U_PCT}}},
    {"LFO", HB_BTN_LFO, {{"RATE", P_LRATE, U_LFO}, {"VIBRATO", P_LPITCH, U_PCT}, {">CUTOFF", P_LCUT, U_PCT},
                         {"AMOUNT", P_LAMT, U_PCT}}},
    {"LFO > WAVE", HB_BTN_LFO, {{"OSC 1", P_OSC(0, O_LFO), U_NUM}, {"OSC 2", P_OSC(1, O_LFO), U_NUM},
                                {"OSC 3", P_OSC(2, O_LFO), U_NUM}, NONE}},
    {"ARPEGGIATOR", HB_BTN_ARP, {{"MODE", G_AMODE, U_AMODE}, {"RATE", G_ARATE, U_RATE}, {"OCTAVES", G_AOCT, U_AOCT},
                                 {"GATE", G_AGATE, U_AGATE}}},
    {"ARP OPTIONS", HB_BTN_ARP, {{"TEMPO", G_TEMPO, U_TEMPO}, {"SWING", G_SWING, U_SWING}, {"LATCH", G_LATCH, U_LATCH},
                                 {"VELO", G_VELO, U_VELO}}},
    {"LOOP RECORDER", HB_BTN_SEQ, {{"TEMPO", G_TEMPO, U_TEMPO}, {"BARS", G_BARS, U_BARS}, {"CLICK", G_CLICK, U_CLICK},
                                   {"TRANSP", G_TRANSP, U_TRANSP}}},
};
#define NPAGES (sizeof PAGES / sizeof PAGES[0])
static const uint8_t PAGE_BTN[5] = {HB_BTN_FX, HB_BTN_SEL, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_EDIT};   /* the panel's order */
static const char *const BTN_NAME[5] = {"FX", "SEL", "ENV", "LFO", "EDIT"};
static uint32_t page;                           /* 0: HOME */

static int32_t ctl_min(const ctl_t *c) { return c->unit >= U_CHORD ? 0 : W3_PARAM[c->idx].lo; }
static int32_t ctl_max(const ctl_t *c)
{
    if (c->unit < U_CHORD)
        return W3_PARAM[c->idx].hi;
    switch (c->unit) {
    case U_CHORD: return N_CHORDS - 1;
    case U_INV: return 3;
    case U_VOICING: return 2;
    case U_STRUM: return 20;
    case U_AMODE: case U_RATE: return 5;
    case U_AOCT: case U_BARS: return 3;
    case U_AGATE: return 9;
    case U_TEMPO: return 140;
    case U_SWING: return 25;
    case U_LATCH: return 1;
    case U_CLICK: return 2;
    case U_TRANSP: return 24;
    default: return 127;
    }
}
static int32_t ctl_get(const ctl_t *c) { return c->unit >= U_CHORD ? gen[c->idx] : w3_p[c->idx]; }
static void ctl_set(const ctl_t *c, int32_t v)  /* (main loop: a byte at a time, the render reads them each pass) */
{
    v = clamp(v, ctl_min(c), ctl_max(c));
    if (c->unit >= U_CHORD) {                   /* (the players', not the preset's: no EDITED) */
        gen[c->idx] = (uint8_t)v;
        return;
    }
    if (w3_p[c->idx] != v) {
        w3_p[c->idx] = (int8_t)v;
        edited = 1;
    }
}

/* text: s gets v with d decimals (rounded) and unit; returns s */
static char *fmt(char *s, float v, uint32_t d, const char *unit)
{
    uint32_t i = 0, k, p = d == 2u ? 100u : d ? 10u : 1u, n = (uint32_t)(v * (float)p + 0.5f), w;
    char t[12];
    for (w = 0, k = n / p; ; k /= 10u) { t[w++] = (char)('0' + k % 10u); if (k < 10u) break; }
    while (w) s[i++] = t[--w];
    if (d) {
        s[i++] = '.';
        if (d == 2u) s[i++] = (char)('0' + n / 10u % 10u);
        s[i++] = (char)('0' + n % 10u);
    }
    while (*unit) s[i++] = *unit++;
    s[i] = 0;
    return s;
}
static char *fmt_auto(char *s, float v, const char *unit) { return fmt(s, v, v < 9.995f ? 2u : v < 99.95f ? 1u : 0u, unit); }
static char *fmt_signed(char *s, int32_t v, const char *unit)
{
    s[0] = v < 0 ? '-' : '+';
    fmt(s + 1, (float)(v < 0 ? -v : v), 0, unit);
    return s;
}
static void ctl_text(char *s, const ctl_t *c, int32_t v)
{
    float f;
    switch (c->unit) {
    case U_NUM: if (ctl_min(c) < 0) fmt_signed(s, v, ""); else fmt(s, (float)v, 0, ""); break;
    case U_PCT: fmt(s, (float)v * 100.0f / (float)ctl_max(c), 0, "%"); break;
    case U_CUT:                                 /* the engine's: 20 Hz .. 20 kHz over 0..127 */
        f = fm_exp2f(4.3219f + (float)v * (9.9658f / 127.0f));
        if (f < 999.5f) fmt_auto(s, f, " Hz"); else fmt_auto(s, f * 0.001f, "kHz");
        break;
    case U_TIME:
        f = w3_secs(v) * 1000.0f;
        if (f < 999.5f) fmt_auto(s, f, " ms"); else fmt_auto(s, f * 0.001f, " s");
        break;
    case U_TABLE: str_cpy(s, W3_TAB_NAME[v], 12); break;
    case U_WAVE: {
        static const char *const UP[3] = {"TRI", "SQUARE", "SAW"};
        if (v > 60) str_cpy(s, UP[v - 61], 8); else fmt(s, (float)v, 0, "");
        break;
    }
    case U_SEMI: fmt_signed(s, v, " st"); break;
    case U_CENTS: fmt_signed(s, v, " ct"); break;
    case U_LFO: str_cpy(s, W3_DIV_NAME[v], 8); break;
    case U_DLY: fmt(s, (float)(441u + (uint32_t)v * ((W3_DLY - 442u) / 127u)) * (1000.0f / 44100.0f), 0, " ms"); break;
    case U_UNI: if (v > 1) fmt(s, (float)v, 0, " VOICES"); else str_cpy(s, "OFF", 8); break;
    case U_CHORD: str_cpy(s, CHORDS[v].name, 8); break;
    case U_INV: { static const char *const I[4] = {"ROOT", "1ST", "2ND", "3RD"}; str_cpy(s, I[v], 8); break; }
    case U_VOICING: { static const char *const V[3] = {"CLOSE", "OPEN", "+BASS"}; str_cpy(s, V[v], 8); break; }
    case U_STRUM: if (v) fmt(s, (float)(v * 10), 0, " ms"); else str_cpy(s, "OFF", 8); break;
    case U_AMODE: { static const char *const M[6] = {"OFF", "UP", "DOWN", "UP/DN", "RANDOM", "PLAYED"}; str_cpy(s, M[v], 8); break; }
    case U_RATE: { static const char *const R[6] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"}; str_cpy(s, R[v], 8); break; }
    case U_AOCT: fmt(s, (float)(v + 1), 0, " OCT"); break;
    case U_AGATE: fmt(s, (float)(v * 10 + 10), 0, "%"); break;
    case U_TEMPO:
        if (ext_bpm && ext_on()) { str_cpy(s, "EXT ", 8); fmt(s + 4, (float)ext_bpm, 0, ""); }
        else fmt(s, (float)(v + 60), 0, " BPM");
        break;
    case U_SWING: fmt(s, (float)(v + 50), 0, "%"); break;
    case U_LATCH: str_cpy(s, v ? "ON" : "OFF", 8); break;
    case U_VELO: if (v) fmt(s, (float)v, 0, ""); else str_cpy(s, "PLAYED", 8); break;
    case U_BARS: if (v) fmt(s, (float)(1 << v), 0, " BARS"); else str_cpy(s, "1 BAR", 8); break;
    case U_CLICK: { static const char *const C[3] = {"OFF", "REC", "ON"}; str_cpy(s, C[v], 8); break; }
    case U_TRANSP: fmt_signed(s, v - 12, " st"); break;
    }
}

/* ---- the screen: PRO-FM-78's layout in the sea's colours, the program over the waves */
#define C_LEGEND HB_RGB(206, 236, 230)          /* sea foam */
#define C_VALUE  HB_RGB(255, 255, 255)
#define C_DIM    HB_RGB(70, 104, 120)
#define C_LED    HB_RGB(40, 226, 206)           /* the lit: a teal */
#define C_WARN   HB_RGB(255, 80, 60)
#define C_EDITED HB_RGB(255, 160, 90)           /* coral */
#define C_PANEL  HB_RGB(6, 14, 26)
#define CHEEK 8
#define PANEL_W (240 - 2 * CHEEK)
#define COL_W (PANEL_W / 4)
#define Y_PROG 24
#define Y_HEAD 102
#define Y_KNOBS 126
#define Y_FOOT 224
#define SEA_H (Y_HEAD - Y_PROG)

static uint16_t mix(uint32_t r0, uint32_t g0, uint32_t b0, uint32_t r1, uint32_t g1, uint32_t b1, float t)
{
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return HB_RGB((uint32_t)((float)r0 + ((float)r1 - (float)r0) * t), (uint32_t)((float)g0 + ((float)g1 - (float)g0) * t),
                  (uint32_t)((float)b0 + ((float)b1 - (float)b0) * t));
}
static void cheek(int32_t x0, int32_t inner)    /* a deep-water side: darker downwards, a lit outer edge */
{
    int32_t x, y, h = 240 - Y_PROG;
    H->cv_begin(CHEEK, h, 0);
    for (y = 0; y < h; y++) {
        float t = (float)y / (float)h;
        for (x = 0; x < CHEEK; x++)
            H->cv_pixel(x, y, x == inner ? HB_RGB(2, 6, 12)
                                         : x == (inner ? 0 : CHEEK - 1) ? mix(30, 80, 110, 10, 30, 50, t)
                                                                        : mix(14, 44, 70, 4, 14, 28, t));
    }
    H->cv_blit(x0, Y_PROG);
}

/* the sea: three waves rolling in, the third (the front one) the tallest, foam on its crest; t in seconds, swell
 * 0..1 (the sound's level) lifts them */
static float sea_t, sea_swell;
static void sea(float t, float swell)
{
    static const struct { float y0, amp, k, w, ph; uint8_t r, g, b; } W[3] = {
        {57.0f, 4.0f, 0.045f, 0.9f, 0.0f, 18, 64, 100},
        {64.0f, 5.5f, 0.034f, 1.3f, 1.7f, 16, 96, 128},
        {71.0f, 7.5f, 0.026f, 1.9f, 3.1f, 22, 140, 160}};
    int32_t x, y, l;
    for (y = 0; y < SEA_H; y += 2)              /* the night sky down to the horizon */
        H->cv_rect(0, y, PANEL_W, 2, mix(2, 6, 18, 12, 36, 66, (float)y / 56.0f));
    for (l = 0; l < 3; l++) {
        float a = W[l].amp * (1.0f + 0.8f * swell);
        for (x = 0; x < PANEL_W; x++) {
            float th = (float)x * W[l].k - t * W[l].w + W[l].ph;
            float h = W[l].y0 - a * (fm_sinf(th) + 0.35f * fm_sinf(2.0f * th + 0.8f));   /* (steeper fronts: a sea's) */
            int32_t top = (int32_t)h;
            if (top < 0) top = 0;
            if (top >= SEA_H) continue;
            H->cv_rect(x, top, 1, SEA_H - top, HB_RGB(W[l].r, W[l].g, W[l].b));
            if (l == 2)                         /* foam on the third wave's crest */
                H->cv_pixel(x, top, h < W[l].y0 - a * 0.6f ? HB_RGB(230, 250, 250) : HB_RGB(120, 200, 210));
        }
    }
}
static void draw_program(void)                  /* the program over the sea: its type, place, name, EDITED, chord */
{
    char s[24];
    const char *name = W3_PRESET[want].name;
    uint32_t i = 0;
    H->cv_begin(PANEL_W, SEA_H, 0);
    sea(sea_t, sea_swell);
    str_cpy(s, type_name(type), 12);
    while (s[i]) i++;
    s[i++] = ' ';
    fmt(s + i, (float)(t_pos[type] + 1u), 0, " OF ");
    while (s[i]) i++;
    fmt(s + i, (float)t_count[type], 0, "");
    H->cv_text(6, 2, 0, s, C_DIM);
    if (edited)
        H->cv_text(PANEL_W - 4 - H->text_w(0, "EDITED"), 2, 0, "EDITED", C_EDITED);
    else if (gen[G_CHORD])
        H->cv_text(PANEL_W - 4 - H->text_w(0, CHORDS[gen[G_CHORD]].name), 2, 0, CHORDS[gen[G_CHORD]].name, C_LED);
    if (H->text_w(1, name) <= PANEL_W - 8)
        H->cv_text(6, 18, 1, name, C_VALUE);
    else
        H->cv_text(6, 26, 0, name, C_VALUE);
    H->cv_blit(CHEEK, Y_PROG);
}
static const char *toast;
static uint32_t toast_ms, hold;
static void say(const char *t) { toast = t; toast_ms = H->ms(); }
static void draw_head(void)
{
    const char *t = hold ? "HOLD TO CLEAR" : toast ? toast : PAGES[page].title;
    int32_t w = H->text_w(0, t), x = (PANEL_W - w) / 2;
    H->cv_begin(PANEL_W, 22, C_PANEL);
    H->cv_rect(4, 10, x - 10, 1, C_LEGEND);
    H->cv_rect(x + w + 6, 10, PANEL_W - 4 - (x + w + 6), 1, C_LEGEND);
    H->cv_text(x, 3, 0, t, hold ? C_WARN : toast ? C_EDITED : C_LEGEND);
    if (hold) H->cv_rect(4, 18, (PANEL_W - 8) * (int32_t)hold / 16, 3, C_WARN);
    H->cv_blit(CHEEK, Y_HEAD);
}
static void draw_foot(void)
{
    uint32_t k;
    H->cv_begin(PANEL_W, 240 - Y_FOOT, C_PANEL);
    for (k = 0; k < 5u; k++)
        H->cv_text(4 + (int32_t)k * 43 + (43 - H->text_w(0, BTN_NAME[k])) / 2, 0, 0, BTN_NAME[k],
                   PAGES[page].btn == PAGE_BTN[k] ? C_LEGEND : C_DIM);
    H->cv_blit(CHEEK, Y_FOOT);
}
static int32_t ntext_w(const char *s, int32_t g)   /* the small font set close (g px a glyph, 4 a space) */
{
    int32_t w = 0;
    for (; *s; s++) w += *s == ' ' ? 4 : g;
    return w;
}
static void ntext(int32_t x, const char *s, int32_t y, uint16_t c)   /* centred in a column (closer still if it is wide:
                                                                     * SAW-SWEEP) */
{
    char g[2] = {0, 0};
    int32_t step = ntext_w(s, 7) > COL_W ? 6 : 7;
    for (x += (COL_W - ntext_w(s, step)) / 2; *s; s++) {
        g[0] = *s;
        if (*s != ' ') H->cv_text(x, y, 0, g, c);
        x += *s == ' ' ? 4 : step;
    }
}
static void draw_knob(uint32_t k, int32_t v)    /* KNOB k+1: its name, a knob over its scale, its value */
{
    const ctl_t *c = &PAGES[page].c[k];
    char s[16];
    int32_t cx = COL_W / 2, cy = 44, x, y, i;
    float f, a, sn, cs;
    H->cv_begin(COL_W, Y_FOOT - Y_KNOBS, C_PANEL);
    if (!c->label) {                            /* (an empty place) */
        H->cv_blit(CHEEK + (int32_t)k * COL_W, Y_KNOBS);
        return;
    }
    f = (float)(v - ctl_min(c)) / (float)(ctl_max(c) - ctl_min(c));
    a = (-0.75f + 1.5f * f) * FM_PI;
    sn = fm_sinf(a);
    cs = fm_cosf(a);
    ntext(0, c->label, 0, C_LEGEND);
    for (i = 0; i <= 10; i++) {
        float t = (-0.75f + 0.15f * (float)i) * FM_PI, ts = fm_sinf(t), tc = fm_cosf(t);
        H->cv_line(cx + (int32_t)(21.0f * ts), cy - (int32_t)(21.0f * tc), cx + (int32_t)(24.0f * ts),
                   cy - (int32_t)(24.0f * tc), C_DIM);
    }
    for (y = -18; y <= 18; y++)                 /* the knob: a deep blue, a lighter rim, lit from above */
        for (x = -18; x <= 18; x++) {
            int32_t d = x * x + y * y;
            if (d <= 18 * 18)
                H->cv_pixel(cx + x, cy + y, d > 16 * 16 ? (y < 0 ? HB_RGB(110, 150, 170) : HB_RGB(50, 80, 100))
                                                        : y < -10 ? HB_RGB(30, 50, 70) : HB_RGB(16, 30, 46));
        }
    for (i = -1; i <= 1; i++)
        H->cv_line(cx + i, cy, cx + i + (int32_t)(15.0f * sn), cy - (int32_t)(15.0f * cs), C_VALUE);
    ctl_text(s, c, v);
    ntext(0, s, 74, c->unit == U_TABLE ? C_LED : C_VALUE);
    H->cv_blit(CHEEK + (int32_t)k * COL_W, Y_KNOBS);
}

static uint32_t d_want = ~0u, d_page = ~0u, d_edited, d_type, d_chord, d_full;
static int32_t d_val[4];
static uint32_t knob_ms[4], anim_ms;

/* ---- the panel is the module's (HB_F_PANEL): its keys, OCT- / OCT+, a short HOME (held 1.5 s: the host's list) */
#define KEY0 53                                 /* key 0 = F3, as the host plays them */
static int32_t oct, d_oct = 99;
static uint8_t key_note[27], key_held[27], key_lit[27];
static uint32_t home_ms, rec_ms, rec_down, rec_done;
static char d_top[16] = "?";
static void top_state(char *s)                  /* the players on the top line: ARP, ARMED, REC 2.3, PLAY 1.4 */
{
    uint32_t i = 0, t = loop_t;
    if (gen[G_AMODE]) { str_cpy(s, "ARP ", 8); i = 4; }
    if (rec == R_ARMED) str_cpy(s + i, "ARMED", 8);
    else if (playing) {
        str_cpy(s + i, rec == R_ON ? "REC " : "PLAY ", 8);
        while (s[i]) i++;
        s[i++] = (char)('1' + t / 96u);
        s[i++] = '.';
        s[i++] = (char)('1' + t % 96u / 24u);
        s[i] = 0;
    } else s[i] = 0;
}
static void draw_top(void)
{
    char s[8] = "OCT +0";
    s[4] = oct < 0 ? '-' : '+';
    s[5] = (char)('0' + (oct < 0 ? -oct : oct));
    H->cv_begin(240, 20, 0);
    H->cv_text(4, 2, 0, "M-3RD-VAVE", C_LED);
    H->cv_text(16 + H->text_w(0, "M-3RD-VAVE"), 2, 0, d_top, rec ? C_WARN : C_LEGEND);
    H->cv_text(236 - H->text_w(0, s), 2, 0, s, C_DIM);
    H->cv_rect(0, 19, 240, 1, C_DIM);
    H->cv_blit(0, 0);
}

/* ---- kept over a power-off: the host's settings record (it writes it to flash after a quiet while) */
#define REC_MAGIC 0x33564157u                   /* "WAV3" */
typedef struct { uint32_t magic; uint8_t want, type, edited; int8_t oct; uint8_t t_pos[TN], gen[16]; int8_t p[W3_NP]; } rec_t;
static rec_t rec_saved;
static uint32_t rec_ms_checked;
static void rec_make(rec_t *r)
{
    uint32_t k;
    memset(r, 0, sizeof *r);
    r->magic = REC_MAGIC;
    r->want = (uint8_t)want; r->type = (uint8_t)type; r->edited = (uint8_t)edited; r->oct = (int8_t)oct;
    memcpy(r->t_pos, t_pos, sizeof t_pos);
    memcpy(r->gen, gen, sizeof r->gen);
    for (k = 0; k < W3_NP; k++) r->p[k] = w3_p[k];
}
static int rec_restore(void)
{
    rec_t r;
    uint32_t k;
    if (H->load(&r, sizeof r) != 0 || r.magic != REC_MAGIC || r.want >= W3_NPRESET || r.type >= TN)
        return -1;
    for (k = 0; k < TN; k++)
        if (r.t_pos[k] >= t_count[k]) return -1;
    want = r.want; type = r.type; edited = r.edited; oct = clamp(r.oct, -2, 2);
    memcpy(t_pos, r.t_pos, sizeof t_pos);
    memcpy(gen, r.gen, sizeof gen);
    for (k = 0; k < W3_NP; k++)                 /* (the edits, each kept in its range) */
        w3_p[k] = (int8_t)clamp(r.p[k], W3_PARAM[k].lo, W3_PARAM[k].hi);
    rec_saved = r;
    return 0;
}
static void rec_keep(uint32_t now)              /* (main loop) a changed state to the host, at most twice a second and
                                                 * only while nothing sounds: its flash write stalls the sound */
{
    rec_t r;
    if (now - rec_ms_checked < 500u || playing || rec || an || sounding())
        return;
    rec_ms_checked = now;
    rec_make(&r);
    if (memcmp(&r, &rec_saved, sizeof r)) {
        H->save(&r, sizeof r);
        rec_saved = r;
    }
}

static uint8_t d_led[4] = {9, 9, 9, 9};
static uint32_t d_ext;
static const char *d_toast;
static uint32_t d_hold;
static int str_eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void draw(uint32_t full)
{
    static const uint8_t PBTN[4] = {HB_BTN_ARP, HB_BTN_SEQ, HB_BTN_PLAY, HB_BTN_REC};
    uint32_t k, now = H->ms(), led[4], again = 0;
    char st[16];
    if (full || !d_full) {
        H->fill(0, 0, 240, 240, 0);
        cheek(0, CHEEK - 1);
        cheek(240 - CHEEK, 0);
        d_want = d_page = ~0u;
        d_oct = 99;
        d_full = 1;
    }
    top_state(st);
    if (oct != d_oct || !str_eq(st, d_top)) {
        d_oct = oct;
        str_cpy(d_top, st, sizeof d_top);
        draw_top();
    }
    if (toast && now - toast_ms > 1500u)
        toast = 0;
    if (now - anim_ms >= 80u) {                 /* the sea rolls, ~12 frames a second */
        float lvl = (float)out_peak * (1.0f / 20000.0f);
        sea_t += (float)(now - anim_ms > 400u ? 80u : now - anim_ms) * 0.001f;
        sea_swell += ((lvl > 1.0f ? 1.0f : lvl) - sea_swell) * 0.3f;
        anim_ms = now;
        again = 1;
    }
    if (again || want != d_want || edited != d_edited || type != d_type || gen[G_CHORD] != d_chord) {
        d_chord = gen[G_CHORD];
        d_want = want;
        d_edited = edited;
        d_type = type;
        draw_program();
    }
    if (page != d_page) {
        d_page = page;
        draw_head();
        draw_foot();
        for (k = 0; k < 5u; k++)
            H->led(PAGE_BTN[k], PAGES[page].btn == PAGE_BTN[k] ? 1u : 2u);
        d_val[0] = d_val[1] = d_val[2] = d_val[3] = -999;
    }
    if (rec_down && !rec_done && now - rec_ms >= 1000u) {   /* REC held 1 s: cleared */
        put(Q_CLEAR, 0, 0); say("LOOP CLEARED");
        rec_done = 1;
    }
    hold = rec_down && !rec_done && now - rec_ms >= 250u ? 1u + (now - rec_ms - 250u) * 15u / 750u : 0u;
    if (toast != d_toast || hold != d_hold) {
        d_toast = toast;
        d_hold = hold;
        draw_head();
    }
    if ((ext_on() ? ext_bpm : 0u) != d_ext) {
        d_ext = ext_on() ? ext_bpm : 0u;
        d_val[0] = -999;
    }
    led[0] = gen[G_AMODE] || PAGES[page].btn == HB_BTN_ARP ? 1u : 2u;
    led[1] = playing || PAGES[page].btn == HB_BTN_SEQ ? 1u : 2u;
    led[2] = playing ? 1u : 2u;
    led[3] = rec == R_ON ? 1u : rec == R_ARMED ? (now / 250u & 1u) : 2u;
    for (k = 0; k < 4u; k++)
        if (led[k] != d_led[k]) H->led(PBTN[k], d_led[k] = (uint8_t)led[k]);
    for (k = 0; k < 27u; k++) {
        int32_t n = KEY0 + (int32_t)k + 12 * oct;
        uint32_t on = key_held[k] || (n >= 0 && n < 128 && (lit[n >> 5] >> (n & 31) & 1u));
        if (on != key_lit[k]) H->led(HB_LED_KEY(k), key_lit[k] = (uint8_t)on);
    }
    for (k = 0; k < 4u; k++) {
        int32_t v = PAGES[page].c[k].label ? ctl_get(&PAGES[page].c[k]) : -998;
        if (v != d_val[k]) {
            d_val[k] = v;
            draw_knob(k, v);
        }
    }
    rec_keep(now);
}

/* ---- SAVE: the loop and the players' settings in M-3rd-VAVE's flash bank, loaded at the start */
#define SV_MAGIC 0x31504F57u                    /* "WOP1" */
static struct { uint32_t magic; uint8_t gen[16]; ev_t ev[NEV]; } sv;
static void loop_save(void)
{
    if (playing || rec) { say("STOP THE LOOP FIRST"); return; }   /* (a save stalls the sound ~100 ms) */
    if (sounding() || an) { say("LET NOTES END"); return; }
    if (!HB_HOST_HAS(H, bank_save)) { say("NO SAVE HERE"); return; }
    sv.magic = SV_MAGIC;
    memcpy(sv.gen, gen, sizeof gen);
    memcpy(sv.ev, ev, sizeof ev);
    say(H->bank_save(&sv, sizeof sv) == 0 ? "LOOP SAVED" : "SAVE FAILED");
}
static void loop_load(void)
{
    uint32_t i;
    if (!HB_HOST_HAS(H, bank_load) || H->bank_load(&sv, sizeof sv) != 0 || sv.magic != SV_MAGIC)
        return;
    memcpy(gen, sv.gen, sizeof gen);
    memcpy(ev, sv.ev, sizeof ev);
    for (i = 0; i < NEV; i++)
        ev[i].fl &= EV_USED | EV_CHORD;
}

static void note(uint32_t ch, uint32_t key, uint32_t vel);
static void ui(uint32_t ev_, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev_), k;
    switch (HB_EV_KIND(ev_)) {
    case HB_EV_KEY_DOWN:                        /* arg: a sensed key's velocity, else 0 (100) */
        if (id < 27u) {
            key_note[id] = (uint8_t)clamp(KEY0 + (int32_t)id + 12 * oct, 0, 127);
            note(0, key_note[id], arg > 0 ? (uint32_t)arg : 100u);
            H->led(HB_LED_KEY(id), key_held[id] = key_lit[id] = 1);
        }
        return;
    case HB_EV_KEY_UP:
        if (id < 27u) {
            note(0, key_note[id], 0);
            H->led(HB_LED_KEY(id), key_held[id] = key_lit[id] = 0);
        }
        return;
    case HB_EV_BTN_UP:
        if (id == HB_BTN_HOME && H->ms() - home_ms < 1400u)
            page = 0;
        if (id == HB_BTN_REC && rec_down) {
            rec_down = 0;
            if (!rec_done && H->ms() - rec_ms < 1000u) put(Q_REC, 0, 0);
            else if (!rec_done) { put(Q_CLEAR, 0, 0); say("LOOP CLEARED"); }
        }
        return;
    default: break;
    }
    if (HB_EV_KIND(ev_) == HB_EV_BTN_DOWN) {    /* a section: its first page, again its next, after its last HOME */
        if (id == HB_BTN_HOME) home_ms = H->ms();
        else if (id == HB_BTN_REC) { rec_ms = H->ms(); rec_down = 1; rec_done = 0; }
        else if (id == HB_BTN_PLAY) put(Q_PLAY, 0, 0);
        else if (id == HB_BTN_SAVE) loop_save();
        else if (id == HB_BTN_OCT_DOWN && oct > -2) oct--;
        else if (id == HB_BTN_OCT_UP && oct < 2) oct++;
        else if (PAGES[page].btn == id)
            page = page + 1u < NPAGES && PAGES[page + 1u].btn == id ? page + 1u : 0u;
        else
            for (k = 0; k < NPAGES; k++)
                if (PAGES[k].btn == id) { page = k; break; }
        return;
    }
    if (HB_EV_KIND(ev_) != HB_EV_ENC)
        return;
    if (id == HB_ENC_ALGORITHM) {
        type = (uint32_t)(((int32_t)type + arg % (int32_t)TN + (int32_t)TN) % (int32_t)TN);
        pick_in_type();
    } else if (id == HB_ENC_PRESETS || id == HB_ENC_SELECT) {
        int32_t n = t_count[type];
        t_pos[type] = (uint8_t)(((int32_t)t_pos[type] + arg % n + n) % n);
        pick_in_type();
    } else if (id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB4) {
        const ctl_t *c = &PAGES[page].c[id - HB_ENC_KNOB1];
        uint32_t i = id - HB_ENC_KNOB1, now = H->ms();
        int32_t step = ctl_max(c) - ctl_min(c) > 48 && now - knob_ms[i] < 40u ? 4 : 1;   /* turned fast: four a detent */
        if (!c->label)
            return;
        knob_ms[i] = now;
        ctl_set(c, ctl_get(c) + arg * step);
    }
}

static void note(uint32_t ch, uint32_t key, uint32_t vel)   /* keys and MIDI alike: to the players (the interrupt) */
{
    (void)ch;
    put(Q_KEY, key & 127u, vel & 127u);
}
static void cc(uint32_t ch, uint32_t num, uint32_t val)
{
    static const ctl_t SOUND[4] = {{"", P_CUT, U_CUT}, {"", P_RES, U_PCT}, {"", P_AA, U_TIME}, {"", P_AR, U_TIME}};
    (void)ch;
    if (num == 74u) ctl_set(&SOUND[0], (int32_t)val);
    else if (num == 71u) ctl_set(&SOUND[1], (int32_t)val * 100 / 127);
    else if (num == 73u) ctl_set(&SOUND[2], (int32_t)val);
    else if (num == 72u) ctl_set(&SOUND[3], (int32_t)val);
    else if (num == 120u || num == 123u) put(Q_ALL_OFF, 0, 0);
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (st >= 0xF8u) {                          /* a DAW's clock and transport: the arp and the loop follow */
        if (st == 0xF8u) put(Q_CLK, 0, 0);
        else if (st == 0xFAu) put(Q_START, 0, 0);
        else if (st == 0xFBu) put(Q_CONT, 0, 0);
        else if (st == 0xFCu) put(Q_STOP, 0, 0);
        return;
    }
    switch (st & 0xF0u) {
    case 0x80u: note(0, d1, 0); break;
    case 0x90u: note(0, d1, d2); break;
    case 0xB0u: cc(0, d1, d2); break;
    default: break;
    }
}

static int init(const hb_host_t *h)
{
    H = h;
    if (HB_HOST_HAS(h, c1_wait)) {               /* the second core: the odd voices */
        w3_c1_run = h->c1_run;
        w3_c1_wait = h->c1_wait;
    }
    types_init();
    loop_load();
    if (rec_restore() != 0) {                    /* (nothing kept: ALL, its first) */
        type = 0;
        memset(t_pos, 0, sizeof t_pos);
        pick_in_type();
        rec_make(&rec_saved);
    }
    d_full = 0;
    page = 0;
    return 0;
}

static void release(void)
{
    uint32_t i;
    w3_p_panic = 1;
    playing = 0;                                /* (the loop stopped, kept until another module's RAM) */
    rec = R_OFF;
    rec_down = 0;
    players_all_off();
    for (i = 0; i < 5u; i++)
        H->led(PAGE_BTN[i], 0);
    H->led(HB_BTN_ARP, 0); H->led(HB_BTN_SEQ, 0); H->led(HB_BTN_PLAY, 0); H->led(HB_BTN_REC, 0);
    d_led[0] = d_led[1] = d_led[2] = d_led[3] = 9;
    memset(key_lit, 0, sizeof key_lit);
    memset(key_held, 0, sizeof key_held);
    for (i = 0; i < 27u; i++)
        H->led(HB_LED_KEY(i), 0);
    d_full = 0;
}

HB_DESC = {HB_ABI, "M-3RD-VAVE", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
