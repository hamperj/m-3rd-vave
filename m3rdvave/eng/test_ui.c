/* SPDX-License-Identifier: GPL-3.0-only */
/* M-3rd-VAVE's panel on this computer, through its own render(), note(), ui() and draw() on the host's own canvas and
 * fonts (firmware/src/gfx.c): HOME first with the sea rolling (and only there); the section buttons step their pages,
 * PRO-FM-78's order; a knob edits the preset (EDITED); ALGORITHM / PRESETS step types and presets; the keys play
 * through the octave; the chord generator, the arpeggiator and the loop recorder play its voices; the preset,
 * its edits and the players' settings come back after a power-off, and are handed to the host only while nothing
 * sounds. Writes OUT-*.ppm (the screens).
 *   tests/run_host.sh (fm1-dock's firmware/dock, firmware/src, firmware/gen, modules/lib on the include path) */
#define HB_DESC static const hb_module_t hb_module    /* (no .hb_desc section on a computer) */
#include <stdint.h>
static void trace(uint32_t note, uint32_t vel);
#define VOICE_TRACE(n, v) trace(n, v)
#include "m3rdvave.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HALF 256u

/* ---- the screen, as dock.c draws it (gfx.c), into a framebuffer */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t c = px[j * w + i];
                fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);
            }
}
#undef C_DIM
#define CV_MAX (240u * 96u)
#define section(x) aligned(2)
#include "gfx.c"
#undef section
static void h_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { lcd_fill((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, c); }
static void h_text(int32_t x, int32_t y, int32_t w, uint32_t big, const char *s, uint16_t fg, uint32_t center)
{ draw_text_box((uint32_t)x, (uint32_t)y, (uint32_t)w, big ? &FONT_L : &FONT_S, s, fg, center ? 1 : 0); }
static void h_cv_begin(int32_t w, int32_t h, uint16_t bg)
{
    if ((uint32_t)(w * h) > CV_MAX) h = (int32_t)(CV_MAX / (uint32_t)w);
    cv_begin((uint32_t)w, (uint32_t)h, bg);
}
static void h_cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { cv_rect(x, y, w, h, c); }
static void h_cv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) { cv_line(x0, y0, x1, y1, c); }
static void h_cv_pixel(int32_t x, int32_t y, uint16_t c) { cv_pset(x, y, c); }
static void h_cv_text(int32_t x, int32_t y, uint32_t big, const char *s, uint16_t fg) { cv_text(x, y, big ? &FONT_L : &FONT_S, s, fg); }
static void h_cv_blit(int32_t x, int32_t y) { cv_blit((uint32_t)x, (uint32_t)y); }
static int32_t h_text_w(uint32_t big, const char *s) { return text_w(big ? &FONT_L : &FONT_S, s); }
static uint32_t now_ms;
static uint32_t h_ms(void) { return now_ms; }
static uint8_t led_on[41];
static void h_led(uint32_t id, uint32_t on) { if (id < 41u) led_on[id] = (uint8_t)on; }
static void h_log(const char *s) { (void)s; }

/* ---- the host's settings record (a power-off keeps it) and the bank */
static uint8_t rec_mem[240];
static uint32_t rec_n, saves;
static int h_load(void *p, uint32_t n) { if (!rec_n || n != rec_n) return -1; memcpy(p, rec_mem, n); return 0; }
static void h_save(const void *p, uint32_t n) { if (n <= sizeof rec_mem) { memcpy(rec_mem, p, n); rec_n = n; saves++; } }
static uint8_t bank_mem[HB_BANK_MAX];
static uint32_t bank_n;
static int h_bank_save(const void *p, uint32_t n) { if (n > HB_BANK_MAX) return -1; memcpy(bank_mem, p, n); bank_n = n; return 0; }
static int h_bank_load(void *p, uint32_t n) { if (!bank_n || n != bank_n) return -1; memcpy(p, bank_mem, n); return 0; }

/* ---- a second core that runs its job when the render waits for it */
static void (*c1_fn)(uint32_t);
static int h_c1_run(void (*fn)(uint32_t), uint32_t arg) { (void)arg; c1_fn = fn; return 1; }
static int h_c1_wait(void) { c1_fn(0); return 0; }

static struct { uint32_t s, note, vel; } tr[8192];
static uint32_t n_tr;
static void trace(uint32_t note, uint32_t vel)
{
    if (n_tr < 8192u) { tr[n_tr].s = now_s; tr[n_tr].note = note; tr[n_tr++].vel = vel; }
}
static uint32_t ons(uint32_t note)              /* note-ons of note (0: any) since n_tr was cleared */
{
    uint32_t i, n = 0;
    for (i = 0; i < n_tr; i++) n += tr[i].vel && (!note || tr[i].note == note);
    return n;
}

static int failed;
#define CHECK(ok, ...) do { if (!(ok)) { failed = 1; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int32_t peak;
static void play(uint32_t ms)
{
    int32_t out[2 * HALF];
    uint32_t b, i;
    for (b = 0; b < (ms * 44100u / 1000u + HALF - 1u) / HALF; b++) {
        memset(out, 0, sizeof out);
        hb_module.render(out, HALF);
        now_ms += 6u;
        for (i = 0; i < 2u * HALF; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
        if (b % 4u == 0u) hb_module.draw(0);
    }
    hb_module.draw(0);
}
static void press(uint32_t btn) { hb_module.ui(HB_EV_BTN_DOWN | btn, 0); hb_module.ui(HB_EV_BTN_UP | btn, 0); hb_module.draw(0); }
static void turn(uint32_t enc, int32_t clicks) { hb_module.ui(HB_EV_ENC | enc, clicks); now_ms += 100u; play(6); }
static void key(uint32_t k, uint32_t vel) { hb_module.ui((vel ? HB_EV_KEY_DOWN : HB_EV_KEY_UP) | k, (int32_t)vel); play(12); }
static const char *shown(uint32_t k)
{
    static char s[16];
    ctl_text(s, &PAGES[page].c[k], ctl_get(&PAGES[page].c[k]));
    return s;
}
static void shot(const char *base, const char *name)
{
    char path[256];
    FILE *f;
    uint32_t i;
    if (!base) return;
    snprintf(path, sizeof path, "%s-%s.ppm", base, name);
    if (!(f = fopen(path, "wb"))) return;
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = fb[i];
        fputc((c >> 11) * 255 / 31, f);
        fputc((c >> 5 & 63) * 255 / 63, f);
        fputc((c & 31) * 255 / 31, f);
    }
    fclose(f);
}
static uint32_t sea_crc(void)                   /* the sea's pixels */
{
    uint32_t x, y, h = 0;
    for (y = Y_PROG + 40u; y < Y_HEAD; y++)
        for (x = CHEEK; x < 240u - CHEEK; x++) h = h * 31u + fb[y * 240u + x];
    return h;
}
static uint32_t held(uint32_t note)
{
    uint32_t k, n = 0;
    for (k = 0; k < W3_NV; k++) n += w3_v[k].gate && w3_v[k].key == note;
    return n;
}

int main(int argc, char **argv)
{
    static const hb_host_t host = {.abi = HB_ABI, .size = sizeof(hb_host_t), .ms = h_ms, .log = h_log, .fill = h_fill,
                                   .text = h_text, .cv_begin = h_cv_begin, .cv_rect = h_cv_rect, .cv_line = h_cv_line,
                                   .cv_pixel = h_cv_pixel, .cv_text = h_cv_text, .cv_blit = h_cv_blit, .led = h_led,
                                   .load = h_load, .save = h_save, .text_w = h_text_w, .c1_run = h_c1_run,
                                   .c1_wait = h_c1_wait, .bank_load = h_bank_load, .bank_save = h_bank_save};
    const char *base = argc > 1 ? argv[1] : 0;
    uint32_t a, b, n, cut;

    hb_module.init(&host);
    hb_module.draw(1);
    shot(base, "home");
    CHECK(hb_module.flags == (HB_F_SCREEN | HB_F_PANEL) && hb_module.midi, "the screen, the panel and MIDI are M-3rd-VAVE's");
    CHECK(!strcmp(PAGES[page].title, "HOME") && want == members[0] && !strcmp(type_name(type), "ALL") && !edited,
          "starts on HOME, ALL, its first (%s)", W3_PRESET[want].name);
    CHECK(led_on[HB_BTN_EDIT] == 2u && led_on[HB_BTN_FX] == 2u, "HOME: every section glows");
    CHECK(PAGES[0].c[2].idx == P_LAMT && !strcmp(shown(2), "100%"), "HOME KNOB3: LFO AMT %s", shown(2));
    press(HB_BTN_LFO);
    CHECK(!strcmp(PAGES[page].title, "LFO") && !strcmp(shown(0), "2 BARS"), "LFO RATE in beats: %s", shown(0));
    turn(HB_ENC_KNOB1, 6);
    CHECK(!strcmp(shown(0), "1/8"), "RATE +6: %s", shown(0));
    {   /* 1/4 at 120 BPM: 2 Hz, a cycle every 22050 samples */
        uint32_t c0 = 0, i2;
        float last = 0;
        turn(HB_ENC_KNOB1, -2);
        CHECK(!strcmp(shown(0), "1/4"), "RATE -2: %s", shown(0));
        for (i2 = 0; i2 < 172u; i2++) { play(6);   /* (play(6): two blocks) */ if (last < 0 && w3_ctl.lfo[0] >= 0) c0++; last = w3_ctl.lfo[0]; }
        CHECK(c0 >= 3u && c0 <= 5u, "2 s of 1/4 at 120 BPM: %u cycles", c0);
        turn(HB_ENC_KNOB1, -4);
    }
    press(HB_BTN_HOME);

    /* the sea rolls on HOME, and only there */
    now_ms += 200u; hb_module.draw(0); a = sea_crc();
    now_ms += 200u; hb_module.draw(0); b = sea_crc();
    CHECK(a != b, "HOME: the sea moves");
    shot(base, "home2");

    /* the keys, through the octave and the keys' force */
    press(HB_BTN_OCT_UP);
    key(7, 0 + 90);
    CHECK(held(72) == 1u && led_on[HB_LED_KEY(7)] == 1u, "OCT+, key 7 at 90: C5, its light on");
    key(7, 0);
    CHECK(held(72) == 0u, "let go: C5 released");
    press(HB_BTN_OCT_DOWN);

    /* the sections: PRO-FM-78's order, a press the next page, after the last HOME */
    press(HB_BTN_EDIT);
    CHECK(!strcmp(PAGES[page].title, "FILTER") && led_on[HB_BTN_EDIT] == 1u, "EDIT: FILTER, its light");
    a = sea_crc(); now_ms += 300u; hb_module.draw(0);
    CHECK(sea_crc() != a, "off HOME: the sea still rolls");
    cut = (uint32_t)ctl_get(&PAGES[page].c[0]);
    turn(HB_ENC_KNOB1, -10);
    CHECK(ctl_get(&PAGES[page].c[0]) == (int32_t)cut - 10 && edited, "KNOB1: cutoff down 10 (%s), EDITED", shown(0));
    shot(base, "filter");
    press(HB_BTN_EDIT);
    CHECK(!strcmp(PAGES[page].title, "OSCILLATOR 1"), "EDIT again: OSCILLATOR 1 (%s %s)", shown(0), shown(1));
    shot(base, "osc1");
    press(HB_BTN_EDIT); press(HB_BTN_EDIT); press(HB_BTN_EDIT);
    CHECK(!strcmp(PAGES[page].title, "FINE TUNE"), "EDIT x5: FINE TUNE");
    press(HB_BTN_EDIT);
    CHECK(page == 0u, "past the last: HOME");
    press(HB_BTN_FX); press(HB_BTN_FX); press(HB_BTN_FX);
    CHECK(!strcmp(PAGES[page].title, "VOICES") && !strcmp(shown(0), "OFF"), "FX x3: VOICES, UNISON %s", shown(0));
    shot(base, "voices");
    press(HB_BTN_ENV); press(HB_BTN_ENV); press(HB_BTN_ENV);
    CHECK(!strcmp(PAGES[page].title, "WAVE ENVELOPE"), "ENV x3: WAVE ENVELOPE");
    shot(base, "wave-env");

    /* kept over a power-off: handed to the host only once nothing sounds */
    key(12, 100);
    n = saves; now_ms += 1000u; play(12);
    CHECK(saves == n, "a note sounding: nothing handed to the host");
    key(12, 0);
    play(4000);
    CHECK(saves > n && rec_n == sizeof(rec_t) && rec_n <= 240u, "quiet: the record handed over (%u bytes)", rec_n);
    a = want; b = (uint32_t)w3_p[P_CUT];
    turn(HB_ENC_PRESETS, 3);
    CHECK(want != a && !edited, "PRESETS +3: another preset (%s)", W3_PRESET[want].name);
    {
        uint8_t keep[240]; uint32_t kn = rec_n;
        memcpy(keep, rec_mem, kn);
        hb_module.release();
        memcpy(rec_mem, keep, kn); rec_n = kn;
        hb_module.init(&host);
        hb_module.draw(1);
    }
    CHECK(want == a && edited && (uint32_t)w3_p[P_CUT] == b, "after a power-off: the preset, its edit (cutoff %d), EDITED",
          w3_p[P_CUT]);

    /* ALGORITHM: the types */
    turn(HB_ENC_ALGORITHM, 1);
    CHECK(!strcmp(type_name(type), "PAD") && W3_PRESET[want].type == W3_PAD, "ALGORITHM +1: PAD (%s)", W3_PRESET[want].name);
    turn(HB_ENC_ALGORITHM, -2);
    CHECK(!strcmp(type_name(type), "INIT") && !strcmp(W3_PRESET[want].name, "INIT"), "ALGORITHM -2: INIT");
    turn(HB_ENC_ALGORITHM, 2);

    /* the chord generator: MAJOR, a key a chord */
    press(HB_BTN_SEL);
    turn(HB_ENC_KNOB1, 1);
    CHECK(!strcmp(PAGES[page].title, "CHORD") && !strcmp(shown(0), "MAJOR"), "SEL: CHORD %s", shown(0));
    key(7, 100);
    CHECK(held(60) == 1u && held(64) == 1u && held(67) == 1u, "key C4: C E G");
    shot(base, "chord");
    key(7, 0);
    turn(HB_ENC_KNOB1, -1);

    /* the arpeggiator: UP at 1/16, 120 BPM, three keys held */
    press(HB_BTN_ARP);
    turn(HB_ENC_KNOB1, 1);
    CHECK(!strcmp(shown(0), "UP") && !strcmp(shown(1), "1/16"), "ARP: UP 1/16");
    n_tr = 0;
    hb_module.ui(HB_EV_KEY_DOWN | 7u, 100); hb_module.ui(HB_EV_KEY_DOWN | 11u, 100); hb_module.ui(HB_EV_KEY_DOWN | 14u, 100);
    play(1000);
    n = ons(0);
    CHECK(n >= 7u && n <= 9u && ons(60) && ons(64) && ons(67), "1 s of UP 1/16: %u notes, C E G", n);
    shot(base, "arp");
    hb_module.ui(HB_EV_KEY_UP | 7u, 0); hb_module.ui(HB_EV_KEY_UP | 11u, 0); hb_module.ui(HB_EV_KEY_UP | 14u, 0);
    play(50);
    press(HB_BTN_HOME); press(HB_BTN_ARP);
    turn(HB_ENC_KNOB1, -1);
    CHECK(!strcmp(shown(0), "OFF"), "ARP mode back OFF");
    press(HB_BTN_HOME);

    /* the loop recorder: armed, the first key starts it, played back on the next pass */
    press(HB_BTN_SEQ);
    CHECK(!strcmp(PAGES[page].title, "LOOP RECORDER"), "SEQ: LOOP RECORDER (%s %s)", shown(0), shown(1));
    hb_module.ui(HB_EV_BTN_DOWN | HB_BTN_REC, 0); now_ms += 100u; hb_module.ui(HB_EV_BTN_UP | HB_BTN_REC, 0); play(12);
    CHECK(rec == R_ARMED, "REC: armed");
    key(7, 100); play(200); key(7, 0);
    CHECK(playing && rec == R_ON, "the first key: playing, recording");
    shot(base, "loop");
    play(1900);                                   /* (2 bars at 120 BPM: 4 s; the first pass ends past 2 s) */
    hb_module.ui(HB_EV_BTN_DOWN | HB_BTN_REC, 0); now_ms += 100u; hb_module.ui(HB_EV_BTN_UP | HB_BTN_REC, 0); play(12);
    n_tr = 0;
    play(4200);
    CHECK(ons(60) >= 1u, "the next pass: C4 played back (%u)", ons(60));
    press(HB_BTN_PLAY); play(50);
    CHECK(!playing, "PLAY: stopped");
    press(HB_BTN_SAVE); play(3000); press(HB_BTN_SAVE);
    CHECK(bank_n > 0u, "SAVE: the loop in the bank");
    CHECK(peak < 32767, "never full scale (peak %d)", peak);

    if (failed) printf("FAILED\n");
    return failed;
}
