# SPDX-License-Identifier: GPL-3.0-only
# M-3rd-VAVE's tables from the PPG Wave 2.2's EPROM data, as Jacajack/avr-ppg (GPL-3.0, ab19d9d) extracted it
# (ref/ppg_data.c): 256 half-waves of 64 bytes and 28 wavetables of key waves. Writes eng/w3_tables.h:
#   every wave the tables use (and the triangle, square and saw of positions 61-63), as a full cycle (the half
#   mirrored, as the PPG plays it), band-limited to 63, 32, 16, 8, 4 and 2 harmonics (no DC), kept in 8 bits
#   (the PPG's own grit); each level one guard sample longer, for the linear read
#   the 28 tables as 64 positions: wave A, wave B and how far between (0..255), positions 0..60 from the keys
#   python3 -I m3rdvave/gen_tables.py m3rdvave/ref/ppg_data.c m3rdvave/eng/w3_tables.h
import math, re, sys

NAMES = ["RESONANT", "RESONANT2", "MALLETSYN", "SQR-SWEEP", "BELLISH", "PUL-SWEEP", "SAW-SWEEP", "MELLOWSAW",
         "FEEDBACK", "ADD HARM", "RESO 3 HP", "WIND SYN", "HIGH HARM", "CLIPPER", "ORGAN SYN", "SQUARESAW",
         "FORMANT1", "POLATED", "TRANSIENT", "ELECTRICP", "ROBOTIC", "STRONGHRM", "PERCORGAN", "CLIPSWEEP",
         "RESOHARMS", "2 ECHOES", "FORMANT 2", "FMNTVOCAL"]
HARM = [63, 32, 16, 8, 4, 2]
SIZE = [128, 128, 64, 32, 16, 16]


def arrays(src):
    s = open(src).read()
    wt = re.search(r"ppg_wavetable\[\] PROGMEM = \{(.*?)\};", s, re.S).group(1)
    wf = re.search(r"ppg_waveforms\[\] PROGMEM = \{(.*?)\};", s, re.S).group(1)
    wt = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2}),", wt)]
    wf = [int(x) for x in re.findall(r"^\s*(\d+),", wf, re.M)]
    assert len(wf) == 256 * 64, len(wf)
    return wt, wf


def tables(wt):
    out, i = [], 0
    for _ in range(len(NAMES)):
        i += 1                                   # the table's own byte
        keys = []
        while True:
            w, p = wt[i], wt[i + 1]
            i += 2
            keys.append((p, w))
            if p >= 60:
                break
        assert keys[0][0] == 0 and all(a[0] < b[0] for a, b in zip(keys, keys[1:])), keys
        out.append(keys)
    return out


def cycle(wf, w):                                # avr-ppg's mirroring: the first half the second one turned over
    h = wf[64 * w:64 * w + 64]
    return [(255 - h[63 - p]) - 127.5 for p in range(64)] + [h[p] - 127.5 for p in range(64)]


def harmonics(x):
    n = len(x)
    return [(sum(x[t] * math.cos(2 * math.pi * k * t / n) for t in range(n)) * 2 / n,
             sum(x[t] * math.sin(2 * math.pi * k * t / n) for t in range(n)) * 2 / n) for k in range(n // 2)]


def synth(hs, top, n):
    return [sum(hs[k][0] * math.cos(2 * math.pi * k * t / n) + hs[k][1] * math.sin(2 * math.pi * k * t / n)
                for k in range(1, min(top, len(hs) - 1) + 1)) for t in range(n)]


def upper(kind):                                 # positions 61-63, at about the ROM waves' level
    a = 90.0
    if kind == "tri":
        return [(k, (0.0, a * 8 / math.pi ** 2 * (-1) ** ((k - 1) // 2) / k ** 2) if k % 2 else (0.0, 0.0)) for k in range(64)]
    if kind == "square":
        return [(k, (0.0, a * 4 / math.pi / k) if k % 2 else (0.0, 0.0)) for k in range(64)]
    return [(k, (0.0, a * 2 / math.pi / k) if k else (0.0, 0.0)) for k in range(64)]


def main(src, dst):
    wt, wf = arrays(src)
    tabs = tables(wt)
    used = sorted({w for t in tabs for _, w in t})
    spectra = [harmonics(cycle(wf, w)) for w in used] + [[h for _, h in upper(k)] for k in ("tri", "square", "saw")]
    index = {w: i for i, w in enumerate(used)}
    ntri = len(used)
    per = sum(s + 1 for s in SIZE)
    o = ["/* SPDX-License-Identifier: GPL-3.0-only */",
         "/* made by m3rdvave/gen_tables.py from the PPG Wave 2.2's EPROM waves (Jacajack/avr-ppg ab19d9d's",
         " * src/ppg_data.c): do not edit */",
         "#define W3_NWAVE %du" % len(spectra), "#define W3_NTAB %du" % len(tabs), "#define W3_WAVE_BYTES %du" % per,
         "static const uint8_t W3_LVL_N[6] = {%s};" % ", ".join(map(str, SIZE)),
         "static const uint16_t W3_LVL_AT[6] = {%s};" % ", ".join(str(sum(s + 1 for s in SIZE[:i])) for i in range(6)),
         "static const uint8_t W3_LVL_H[6] = {%s};" % ", ".join(map(str, HARM)),
         "static const int8_t W3_WAVE[W3_NWAVE * W3_WAVE_BYTES] = {"]
    peak = 0
    for hs in spectra:
        row = []
        for top, n in zip(HARM, SIZE):
            x = synth(hs, top, n)
            peak = max(peak, max(map(abs, x)) * 0.77)
            q = [max(-127, min(127, round(v * 0.77))) for v in x]   # the Gibbs overshoot kept under 8 bits
            row += q + [q[0]]
        o.append("    " + ", ".join(map(str, row)) + ",")
    o.append("};")
    o.append("typedef struct { uint8_t a, b, f; } w3_pos_t;    /* wave a, wave b, b's share / 256 */")
    o.append("static const w3_pos_t W3_TAB[W3_NTAB][64] = {")
    for t in tabs:
        pos = []
        for p in range(61):
            j = max(i for i, k in enumerate(t) if k[0] <= p)
            if j == len(t) - 1:
                pos.append((index[t[j][1]], index[t[j][1]], 0))
            else:
                (p0, w0), (p1, w1) = t[j], t[j + 1]
                pos.append((index[w0], index[w1], (p - p0) * 256 // (p1 - p0)))
        pos += [(ntri + i, ntri + i, 0) for i in range(3)]
        o.append("    {" + ", ".join("{%d, %d, %d}" % x for x in pos) + "},")
    o.append("};")
    o.append("static const char *const W3_TAB_NAME[W3_NTAB] = {%s};" % ", ".join('"%s"' % n for n in NAMES))
    open(dst, "w").write("\n".join(o) + "\n")
    print("%d waves (%d from the ROM), %d bytes, peak %.1f" % (len(spectra), ntri, len(spectra) * per, peak))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
