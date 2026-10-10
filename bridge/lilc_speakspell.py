"""Speak & Spell voice for lil' C.

Re-encodes ordinary speech the way TI encoded the Speak & Spell's words (LPC-10,
25 ms frames, quantized to the chip's own tables) and plays it back through an
emulation of the TMS5100 / TMC0281 synthesizer from the 1979 Speak & Spell:
chirp-driven voicing, LFSR noise, a 10-stage integer lattice filter and 8 kHz
output with the chip's coarse DAC.

Tables (energy, pitch, K1-K10, chirp, interpolation) are the T0280B/0281A set
from MAME's src/devices/sound/tms5110r.hxx (BSD-3-Clause; Frank Palazzolo,
Couriersud, Jonathan Gevaryahu), verified there against decapped chips.
"""
import subprocess

import numpy as np

RATE = 8000
FRAME = 200          # 25 ms
SUB = 25             # 8 interpolation steps per frame

ENERGY = [0, 0, 1, 1, 2, 3, 5, 7, 10, 15, 21, 30, 43, 61, 86]  # index 15 = stop
PITCH = [0, 41, 43, 45, 47, 49, 51, 53, 55, 58, 60, 63, 66, 70, 73, 76,
         79, 83, 87, 90, 94, 99, 103, 107, 112, 118, 123, 129, 134, 140, 147, 153]
K = [
    [-501, -497, -493, -488, -480, -471, -460, -446, -427, -405, -378, -344,
     -305, -259, -206, -148, -86, -21, 45, 110, 171, 227, 277, 320, 357, 388,
     413, 434, 451, 464, 474, 498],
    [-349, -328, -305, -280, -252, -223, -192, -158, -124, -88, -51, -14, 23,
     60, 97, 133, 167, 199, 230, 259, 286, 310, 333, 354, 372, 389, 404, 417,
     429, 439, 449, 506],
    [-397, -365, -327, -282, -229, -170, -104, -36, 35, 104, 169, 228, 281, 326, 364, 396],
    [-369, -334, -293, -245, -191, -131, -67, -1, 64, 128, 188, 243, 291, 332, 367, 397],
    [-319, -286, -250, -211, -168, -122, -74, -25, 24, 73, 121, 167, 210, 249, 285, 318],
    [-290, -252, -209, -163, -114, -62, -9, 44, 97, 147, 194, 238, 278, 313, 344, 371],
    [-291, -256, -216, -174, -128, -80, -31, 19, 69, 117, 163, 206, 246, 283, 316, 345],
    [-218, -133, -38, 59, 152, 235, 305, 361],
    [-226, -157, -82, -3, 76, 151, 220, 280],
    [-179, -122, -61, 1, 62, 123, 179, 231],
]
CHIRP = [0x00, 0x2a, 0xd4, 0x32, 0xb2, 0x12, 0x25, 0x14, 0x02, 0xe1, 0xc5, 0x02,
         0x5f, 0x5a, 0x05, 0x0f, 0x26, 0xfc, 0xa5, 0xa5, 0xd6, 0xdd, 0xdc, 0xfc,
         0x25, 0x2b, 0x22, 0x21, 0x0f, 0xff, 0xf8, 0xee, 0xed, 0xef, 0xf7, 0xf6,
         0xfa, 0x00, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00]
CHIRP = [c - 256 if c > 127 else c for c in CHIRP]
INTERP = [0, 3, 3, 3, 2, 2, 1, 1]


def _resample(pcm, rate, to_rate):
    """int16 bytes at rate -> float array at to_rate (via ffmpeg)."""
    p = subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-f", "s16le", "-ar", str(rate), "-ac", "1",
         "-i", "-", "-af", "lowpass=f=3600", "-ar", str(to_rate), "-f", "s16le", "-"],
        input=pcm, capture_output=True, check=True)
    return np.frombuffer(p.stdout, dtype="<i2").astype(np.float64) / 32768.0


def _reflection(frame):
    """Autocorrelation + Levinson-Durbin -> 10 reflection coefficients."""
    n = len(frame)
    r = np.array([np.dot(frame[: n - i], frame[i:]) for i in range(11)])
    if r[0] <= 1e-9:
        return np.zeros(10)
    r[0] *= 1.0001  # a touch of white noise for stability
    a = np.zeros(11)
    a[0] = 1.0
    err = r[0]
    ks = np.zeros(10)
    for i in range(1, 11):
        acc = r[i] + np.dot(a[1:i], r[i - 1:0:-1])
        k = -acc / err
        ks[i - 1] = k
        a_new = a.copy()
        for j in range(1, i):
            a_new[j] = a[j] + k * a[i - j]
        a_new[i] = k
        a = a_new
        err *= 1 - k * k
        if err <= 0:
            break
    return ks


def _pitch(win, lo=20, hi=160):
    """Autocorrelation pitch period (samples at 8 kHz) and its strength."""
    x = win - win.mean()
    x = np.convolve(x, np.ones(6) / 6, mode="same")  # crude 1 kHz low-pass
    # centre clipping makes the autocorrelation peak cleaner
    c = 0.3 * np.max(np.abs(x)) if len(x) else 0
    x = np.where(x > c, x - c, np.where(x < -c, x + c, 0))
    r0 = np.dot(x, x)
    if r0 <= 1e-12:
        return 0, 0.0
    full = np.correlate(x, x, mode="full")[len(x) - 1:]  # all lags at once
    ac = full[lo:hi + 1]
    lag = int(np.argmax(ac))
    return lo + lag, ac[lag] / r0


def _nearest(table, v):
    return int(np.argmin(np.abs(np.asarray(table) - v)))


def encode(x, pitch_scale=1.0):
    """8 kHz float speech -> list of (energy, pitch, k-index tuple) frames."""
    pre = np.append(x[0], x[1:] - 0.9373 * x[:-1])  # pre-emphasis, as TI did
    nfr = len(x) // FRAME + 1
    pad = np.pad(pre, (FRAME, 2 * FRAME))
    padx = np.pad(x, (FRAME, 2 * FRAME))
    ham = np.hamming(2 * FRAME)
    rms = []
    for i in range(nfr):
        # loudness after pre-emphasis, so s/f/t sounds aren't lost
        seg = pad[i * FRAME + FRAME // 2: i * FRAME + FRAME // 2 + FRAME]
        rms.append(np.sqrt(np.mean(seg ** 2)))
    loud = max(np.percentile(rms, 95), 1e-6)
    frames = []
    for i in range(nfr):
        win = pad[i * FRAME: i * FRAME + 2 * FRAME]  # centred on the frame
        e = _nearest(ENERGY, min(1.0, rms[i] / loud) * 86)
        if rms[i] / loud < 0.03 or e == 0:
            frames.append((0, 0, None))
            continue
        ks = _reflection(win * ham)
        # Same sign convention as the chip's lattice (checked against spectra)
        kq = [_nearest(K[j], ks[j] * 512) for j in range(10)]
        raw = padx[i * FRAME: i * FRAME + 2 * FRAME]
        period, strength = _pitch(raw)
        # voiced = periodic, low-frequency heavy (k1 < 0) and not hissy
        zc = np.mean(np.abs(np.diff(np.sign(raw[FRAME // 2:-FRAME // 2])))) / 2
        voiced = strength > 0.2 and ks[0] < 0 and zc < 0.3
        p = _nearest(PITCH[1:], period * pitch_scale) + 1 if voiced else 0
        frames.append((e, p, tuple(kq)))
    return frames


class TMS5100:
    """Integer model of the TMS5100 synthesis path (after MAME's tms5110.cpp)."""

    def __init__(self):
        self.u = [0] * 11
        self.x = [0] * 10
        self.rng = 0x1FFF
        self.pitch_count = 0

    @staticmethod
    def _mul(a, b):
        return (a * b) >> 9

    def _sample(self, energy, period, k):
        if period == 0:  # unvoiced: LFSR noise
            for _ in range(20):
                bit = ((self.rng >> 12) ^ (self.rng >> 3) ^ (self.rng >> 2) ^ self.rng) & 1
                self.rng = ((self.rng << 1) | bit) & 0x1FFF
            exc = -0x40 if self.rng & 1 else 0x40
        else:
            exc = CHIRP[min(self.pitch_count, 51)]
            self.pitch_count += 1
            if self.pitch_count >= period:
                self.pitch_count = 0
        u, x = self.u, self.x
        u[10] = self._mul(energy, exc << 6)
        for i in range(9, -1, -1):
            u[i] = u[i + 1] - self._mul(k[i], x[i])
            u[i] = max(-16384, min(16383, u[i]))
        for i in range(9, 0, -1):
            x[i] = x[i - 1] + self._mul(k[i - 1], u[i - 1])
        x[0] = u[0]
        # The chip's DAC: clip to 12 bits, keep the top 8
        out = max(-2048, min(2047, u[0]))
        return (out >> 4) << 8

    def render(self, frames):
        out = []
        cur_e, cur_p, cur_k = 0, 0, [0] * 10
        prev_voiced, prev_silent = False, True
        for e, p, kq in frames:
            if kq is None:  # silence
                tgt_e, tgt_p, tgt_k = 0, cur_p, cur_k
            else:
                tgt_e, tgt_p = ENERGY[e], PITCH[p]
                tgt_k = [K[j][kq[j]] for j in range(10)]
                if p == 0:  # unvoiced frames use only K1-K4
                    tgt_k = tgt_k[:4] + [0] * 6
            voiced = bool(kq is not None and p)
            # The chip skips interpolation across voicing changes / from silence
            if prev_silent or kq is None or voiced != prev_voiced:
                cur_e, cur_p, cur_k = tgt_e, tgt_p, list(tgt_k)
            for ip in range(8):
                if ip:
                    sh = INTERP[ip]
                    cur_e += (tgt_e - cur_e) >> sh
                    cur_p += (tgt_p - cur_p) >> sh
                    cur_k = [c + ((t - c) >> sh) for c, t in zip(cur_k, tgt_k)]
                for _ in range(SUB):
                    out.append(self._sample(cur_e, cur_p if voiced else 0, cur_k))
            cur_e, cur_p, cur_k = tgt_e, tgt_p, list(tgt_k)
            prev_voiced, prev_silent = voiced, kq is None
        return np.array(out, dtype=np.int32)


_LIB = None


def _render_c(frames):
    """Same as TMS5100().render, in C (tms5100.so, built on first use)."""
    global _LIB
    import ctypes
    import os
    if _LIB is None:
        here = os.path.dirname(os.path.abspath(__file__))
        so = os.path.join(here, "tms5100.so")
        src = os.path.join(here, "tms5100.c")
        if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(src):
            subprocess.run(["gcc", "-O2", "-shared", "-fPIC", "-o", so, src], check=True)
        _LIB = ctypes.CDLL(so)
    rows = np.zeros((len(frames), 13), dtype=np.int32)
    for i, (e, p, kq) in enumerate(frames):
        if kq is None:
            continue
        rows[i, 0] = ENERGY[e]
        rows[i, 1] = PITCH[p]
        rows[i, 2] = 1 if p else 2
        ks = [K[j][kq[j]] for j in range(10)]
        rows[i, 3:13] = ks if p else ks[:4] + [0] * 6
    out = np.zeros(len(frames) * FRAME, dtype=np.int16)
    chirp = np.array(CHIRP, dtype=np.int32)
    interp = np.array(INTERP, dtype=np.int32)
    ptr = lambda a, t: a.ctypes.data_as(ctypes.POINTER(t))
    _LIB.tms_render(ptr(rows, ctypes.c_int32), ctypes.c_int(len(frames)),
                    ptr(chirp, ctypes.c_int32), ptr(interp, ctypes.c_int32),
                    ptr(out, ctypes.c_int16))
    return out.astype(np.int32)


def speak_and_spell(pcm, rate, pitch_scale=2.0):
    """int16 mono bytes at `rate` -> (8000, int16 bytes) in the Speak & Spell voice."""
    x = _resample(pcm, rate, RATE)
    frames = encode(x, pitch_scale)
    try:
        y = _render_c(frames)
    except Exception:  # no compiler: the (slow) Python model
        y = TMS5100().render(frames)
    y = y.astype(np.float64)
    peak = np.max(np.abs(y)) or 1
    y = np.clip(y * (0.9 * 32767 / peak), -32768, 32767).astype("<i2")
    return RATE, y.tobytes()
