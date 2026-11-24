/* SPDX-License-Identifier: GPL-3.0-only */
/* M-3rd-VAVE's engine on the build machine: every preset plays a chord, then a line, then a low note, in 256-frame
 * blocks as the host calls it. Checks the level and that nothing clips or sticks; writes OUT/NN-NAME.wav to listen
 * to and prints how long a block of all the voices takes here (MELLO-1's test prints the same for 8 tapes: the two side
 * by side say whether it fits the chip).
 *   tests/run_host.sh (cc -O2 -ffp-contract=off -Im3rdvave/eng m3rdvave/eng/test_engine.c, then OUT: build/host/wav) */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "w3_engine.c"

static void wav(const char *path, const int16_t *s, uint32_t frames)
{
    FILE *f = fopen(path, "wb");
    uint32_t v, bytes = frames * 4u;
    if (!f) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, f); v = 36u + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); v = 1u | 2u << 16; fwrite(&v, 4, 1, f); v = 44100; fwrite(&v, 4, 1, f);
    v = 44100u * 4u; fwrite(&v, 4, 1, f); v = 4u | 16u << 16; fwrite(&v, 4, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f); fwrite(s, 4, frames, f);
    fclose(f);
}

#define SECS 16u
static int16_t pcm[2u * 44100u * SECS + 512u];   /* + a block: the last one runs past SECS */
static const struct { float at; uint8_t key, vel; } SCORE[] = {
    {0.0f, 48, 100}, {0.0f, 55, 100}, {0.0f, 60, 100}, {0.0f, 64, 100}, {0.0f, 71, 100},          /* Cmaj7, 4 s */
    {4.0f, 48, 0}, {4.0f, 55, 0}, {4.0f, 60, 0}, {4.0f, 64, 0}, {4.0f, 71, 0},
    {5.5f, 45, 90}, {5.5f, 52, 90}, {5.5f, 57, 90}, {5.5f, 60, 90}, {5.5f, 64, 90},              /* Am9-ish, 3 s */
    {8.5f, 45, 0}, {8.5f, 52, 0}, {8.5f, 57, 0}, {8.5f, 60, 0}, {8.5f, 64, 0},
    {9.0f, 72, 110}, {9.4f, 72, 0}, {9.4f, 74, 70}, {9.8f, 74, 0}, {9.8f, 76, 120}, {10.6f, 76, 0},   /* a line */
    {10.6f, 79, 60}, {11.0f, 79, 0}, {11.0f, 76, 100}, {11.8f, 76, 0},
    {12.0f, 36, 110}, {13.0f, 36, 0}, {13.2f, 43, 80}, {14.0f, 43, 0},                          /* low notes */
};

static double rms(uint32_t from, uint32_t to)
{
    double s = 0;
    uint32_t i;
    for (i = from; i < to; i++)
        s += ((double)pcm[2u * i] * pcm[2u * i] + (double)pcm[2u * i + 1u] * pcm[2u * i + 1u]) * 0.5;
    return sqrt(s / (to - from)) / 32768.0;
}

/* core 1, played here: its job run at once (before core 0's voices: they share nothing), timed */
static double c1_us;
static int fake_run(void (*fn)(uint32_t), uint32_t arg) { clock_t t0 = clock(); fn(arg); c1_us += (double)(clock() - t0); return 1; }
static int fake_wait(void) { return 0; }

static int play(uint32_t g)                      /* the score with preset g into pcm; its peak */
{
    uint32_t frames = 0, ev = 0, i;
    int peak = 0;
    w3_load(g);
    w3_p_panic = 1;
    memset(w3_v, 0, sizeof w3_v);
    memset(w3_dl, 0, sizeof w3_dl);
    memset(w3_dr, 0, sizeof w3_dr);
    w3_dlp = w3_lph = 0;
    while (frames < 44100u * SECS) {
        int32_t blk[512] = {0};
        while (ev < sizeof SCORE / sizeof *SCORE && SCORE[ev].at * 44100.0f <= (float)frames) {
            w3_note(SCORE[ev].key, SCORE[ev].vel);
            ev++;
        }
        w3_render(blk, 256);
        for (i = 0; i < 512u; i++) {
            assert(blk[i] >= -32767 && blk[i] <= 32767);
            if (abs(blk[i]) > peak) peak = abs(blk[i]);
            pcm[2u * frames + i] = (int16_t)blk[i];
        }
        frames += 256u;
    }
    return peak;
}

static int16_t one[sizeof pcm / sizeof *pcm];

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    uint32_t g, fails = 0;
    w3_c1_run = fake_run;                        /* the presets as the FM-1 plays them: two cores */
    w3_c1_wait = fake_wait;
    for (g = 0; g < W3_NPRESET; g++) {
        uint32_t i;
        int peak = play(g);
        double chord, end;
        char path[512];
        chord = fmax(rms(0, 22050), rms(44100u, 44100u * 3u));   /* a struck sound at its start, a pad held */
        end = rms(44100u * 31u / 2u, 44100u * 16u);
        snprintf(path, sizeof path, "%s/%02u-%s.wav", out, g + 1u, W3_PRESET[g].name);
        for (i = strlen(out) + 4u; path[i]; i++)
            if (path[i] == ' ' || path[i] == '.') path[i] = path[i] == '.' && !strcmp(path + i, ".wav") ? '.' : '_';
        wav(path, pcm, 44100u * SECS);
        printf("%-10s chord %5.1f dB  peak %5.1f dB  tail %6.1f dB  -> %s\n", W3_PRESET[g].name,
               20 * log10(chord + 1e-9), 20 * log10(peak / 32768.0 + 1e-9), 20 * log10(end + 1e-9), path);
        if (!(chord > 0.025 && chord < 0.5)) { printf("  FAIL: the chord's level\n"); fails++; }
        if (!(end < 0.01)) { printf("  FAIL: it does not die away\n"); fails++; }
    }
    {   /* the voices split between the two cores sound as all on one (but for the order of a sum) */
        uint32_t i;
        int d = 0;
        w3_c1_run = 0;
        play(5);                                 /* ORGAN SYN: short tails, no note stolen with 12 voices */
        memcpy(one, pcm, sizeof pcm);
        w3_c1_run = fake_run;
        play(5);
        for (i = 0; i < sizeof pcm / sizeof *pcm; i++)
            if (abs(pcm[i] - one[i]) > d) d = abs(pcm[i] - one[i]);
        printf("two cores against one: at most %d LSB apart\n", d);
        if (d > 2) { printf("  FAIL: the split changes the sound\n"); fails++; }
    }
    {   /* one core: never more than W3_NV1 voices sound; two: all W3_NV (SUPER SAW: 4 a key, 6 keys held) */
        uint32_t s, i, k, on;
        for (s = 0; s < 2u; s++) {
            int32_t blk[512];
            w3_c1_run = s ? fake_run : 0;
            for (g = 0; g < W3_NPRESET && strcmp(W3_PRESET[g].name, "SUPER SAW"); g++) ;
            assert(g < W3_NPRESET);
            w3_load(g);
            memset(w3_v, 0, sizeof w3_v);
            for (i = 0; i < 2u; i++) { memset(blk, 0, sizeof blk); w3_render(blk, 256); }   /* (the cap is the render's) */
            for (i = 0; i < 6u; i++) w3_note(48u + 4u * i, 100);
            for (i = 0; i < 20u; i++) { memset(blk, 0, sizeof blk); w3_render(blk, 256); }
            for (k = on = 0; k < W3_NV; k++) on += w3_v[k].st[0] != 0;
            printf("%s core%s, 6 keys x 4: %u voices sound\n", s ? "two" : "one", s ? "s" : "", on);
            if (on != (s ? W3_NV : W3_NV1)) { printf("  FAIL: the voice cap\n"); fails++; }
            for (i = 0; i < 6u; i++) w3_note(48u + 4u * i, 0);
        }
    }
    {   /* every voice, three oscillators, the delay on (the chip is far slower: a relative figure) */
        int32_t blk[512];
        clock_t t0;
        double all, us;
        uint32_t i, s;
        for (s = 0; s < 2u; s++) {
            w3_c1_run = s ? fake_run : 0;
            w3_load(1);
            w3_p_panic = 1;
            memset(blk, 0, sizeof blk);
            w3_render(blk, 256);                 /* (the cap follows the cores) */
            for (i = 0; i < W3_NV; i++)
                w3_note(43u + 3u * i, 100);
            c1_us = 0;
            t0 = clock();
            for (i = 0; i < 4000u; i++) { memset(blk, 0, sizeof blk); w3_render(blk, 256); }
            all = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / 4000.0;
            us = c1_us / CLOCKS_PER_SEC * 1e6 / 4000.0;
            if (!s)
                printf("%u voices x 3 oscillators, one core: %.1f us a block here (a block is 5805 us)\n", W3_NV1, all);
            else
                printf("%u voices, two cores: core 0 %.1f us, core 1 %.1f us\n", W3_NV, all - us, us);
        }
    }
    return fails ? 1 : 0;
}
