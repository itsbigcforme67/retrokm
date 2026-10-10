"""Hearing where things are: lil' C's microphones as a sonar.

Two experiments, both built on "record both microphones for a few seconds"
on the Stack-chan or the Tab5 (the device sees a "record" request in its
/api/presence answer, says when it starts, then uploads the recording):

- "speakers": once the device is recording, a RetroKM agent plays a chirp
  from each speaker in turn (for now the Dell, whose sound card has the 5.1
  speakers; "speakers_on" in config.json "locate"). How late each chirp
  arrives compared with the schedule gives how much further away that
  speaker is than the first one. With the speakers' positions in
  "speakers" (desk cm, like lilc_place.py), that solves for where the
  device is.
- "voice": you talk while it records; the difference in arrival time
  between the two microphones (GCC-PHAT) says which side you're on.

Results (and the recordings, as WAV) go to ~/.cache/lilc-workspace/locate/.
Start one with locate.py, or POST /api/locate.
"""
import json
import math
import os
import threading
import time
import wave

import numpy as np

SOUND_C = 34300.0  # cm/s

LOCATE_DEFAULTS = {
    "speakers_on": "precision",           # the RetroKM screen whose agent plays them
    # which speakers, in turn (WAVE order: 0 FL, 1 FR, 2 C, 3 sub, 4 RL, 5 RR).
    # The desk has 4 speakers on the Dell's analog outputs for now; add 2
    # (centre) once it's true 5.1
    "order": "0,1,4,5",
    "rate": 48000,
    "chirp_ms": 60, "gap_ms": 340, "lead_ms": 400,
    "lo_hz": 2000, "hi_hz": 12000, "level": 25,
    # where each speaker is: [x, y, z] in desk cm (lilc_place.py: x right, y up
    # from the desk top, z away from the chair; x = 0 in front of the tall
    # monitor). Rough guesses from the user's photo of the desk (2026-10-10):
    # the fronts sit high on the hutch tops, the rears on the front corners of
    # the two wings, the centre on the shelf above the Extron. Measure them
    # for better positions.
    "speakers": {"0": [-110, 95, 40], "1": [150, 95, 40], "2": [20, 55, 75],
                 "4": [-120, 10, -40], "5": [160, 10, -40]},
    "mic_spacing_cm": 0,                  # between the two microphones, if known
    "device_y_cm": 8,                     # the device's microphones above the desk
}
LAYOUTS = {6: (0, 1, 2, 3, 4, 5), 4: (0, 1, 4, 5), 2: (0, 1)}  # what each card layout plays
NAMES = {0: "front left", 1: "front right", 2: "centre", 3: "sub", 4: "rear left", 5: "rear right"}


def chirp(p, rate):
    n = int(rate * p["chirp_ms"] / 1000)
    t = np.arange(n) / rate
    T = p["chirp_ms"] / 1000
    ph = 2 * np.pi * (p["lo_hz"] * t + (p["hi_hz"] - p["lo_hz"]) * t * t / (2 * T))
    return np.sin(ph) * (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(n) / (n - 1)))


def xcorr(x, tpl):
    """Cross-correlation of x with tpl (same length as x), via FFT."""
    n = 1 << int(math.ceil(math.log2(len(x) + len(tpl))))
    r = np.fft.irfft(np.fft.rfft(x, n) * np.conj(np.fft.rfft(tpl, n)), n)
    return r[: len(x)]


def envelope(r, rate):
    k = max(1, int(rate * 0.0005))  # 0.5 ms
    return np.convolve(np.abs(r), np.ones(k) / k, mode="same")


def first_arrival(env, lo, hi):
    """Where the direct sound starts in env[lo:hi]: the first point at half
    the window's peak, then the top of that first bump (echoes come later)."""
    w = env[lo:hi]
    if len(w) < 3:
        return None, 0.0
    top = w.max()
    i = int(np.argmax(w >= 0.5 * top))
    j = i
    while j + 1 < len(w) and w[j + 1] >= w[j]:
        j += 1
    # parabolic refinement of the bump's top
    if 0 < j < len(w) - 1:
        a, b, c = w[j - 1], w[j], w[j + 1]
        d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) else 0.0
    else:
        d = 0.0
    noise = np.median(env) or 1e-9
    return lo + j + d, float(top / noise)


def analyze_speakers(pcm, rate, channels, p, played_channels):
    x = np.frombuffer(pcm, dtype="<i2").astype(np.float64).reshape(-1, channels).T
    tpl = chirp(p, rate)
    order = [int(c) for c in p["order"].split(",")]
    period = int(rate * (p["chirp_ms"] + p["gap_ms"]) / 1000)
    envs = [envelope(xcorr(ch, tpl), rate) for ch in x]
    env = np.sum(envs, axis=0)
    # where the sequence starts: the offset that lines every slot up with sound
    best, t0 = -1, 0
    win = int(rate * 0.04)
    for off in range(0, max(1, len(env) - period * (len(order) - 1) - win), max(1, rate // 4000)):
        sc = sum(env[off + k * period: off + k * period + win].max() for k in range(len(order))
                 if off + k * period + win < len(env))
        if sc > best:
            best, t0 = sc, off
    t0 = max(0, t0 - win // 2)
    slots = []
    for k, c in enumerate(order):
        lo, hi = t0 + k * period, t0 + k * period + 2 * win
        per_mic = [first_arrival(e, lo, min(hi, len(e))) for e in envs]
        heard = [m for m in per_mic if m[0] is not None]
        arr = np.mean([m[0] for m in heard]) if heard else None
        snr = max((m[1] for m in heard), default=0.0)
        slots.append({"channel": c, "speaker": NAMES.get(c, str(c)),
                      "played": c in LAYOUTS.get(played_channels, ()), "arrival_ms": None if arr is None else
                      round((arr - k * period) * 1000 / rate, 3),
                      "snr": round(snr, 1),
                      "mic_lr_us": None if len(heard) < 2 else round((per_mic[0][0] - per_mic[1][0]) * 1e6 / rate, 1)})
    good = [s for s in slots if s["played"] and s["arrival_ms"] is not None and s["snr"] > 6]
    if good:
        ref = good[0]["arrival_ms"]
        for s in good:
            s["further_cm"] = round((s["arrival_ms"] - ref) / 1000 * SOUND_C, 1)  # than the first speaker
    out = {"kind": "speakers", "rate": rate, "slots": slots}
    pos = solve_position(good, p)
    if pos:
        out["position_cm"] = pos
    return out


def solve_position(good, p):
    """Where the device is across the desk (x, z) from the arrival times, if
    the speakers' positions are known. Its height is taken as known (it sits
    on the desk: "device_y_cm"); speakers at about one height can't tell
    height apart anyway. Needs three speakers; more makes it sturdier."""
    spk = {int(k): np.array(v, dtype=float) for k, v in (p.get("speakers") or {}).items()}
    use = [s for s in good if s["channel"] in spk]
    if len(use) < 3:
        return None
    S = np.array([spk[s["channel"]] for s in use])
    d = np.array([s["arrival_ms"] / 1000 * SOUND_C for s in use])  # distance + unknown offset
    y = float(p.get("device_y_cm", 8))
    best = None
    for x0, z0 in ((0, 40), (-60, 40), (60, 40), (0, 0)):  # a few starts, keep the best fit
        q = np.array([x0, z0, float(d.min() - 100)])  # x, z, offset (cm)
        lam = 1.0
        for _ in range(100):
            P = np.array([q[0], y, q[1]])
            v = P - S
            r = np.linalg.norm(v, axis=1) + 1e-9
            res = r - (d - q[2])
            J = np.stack([v[:, 0] / r, v[:, 2] / r, np.ones(len(use))], axis=1)
            A = J.T @ J + lam * np.eye(3)
            step = np.linalg.lstsq(A, -J.T @ res, rcond=None)[0]
            q2 = q + step
            P2 = np.array([q2[0], y, q2[1]])
            res2 = np.linalg.norm(P2 - S, axis=1) - (d - q2[2])
            if res2 @ res2 < res @ res:
                q, lam = q2, max(1e-6, lam * 0.3)
            else:
                lam *= 5
                if lam > 1e9:
                    break
            if np.linalg.norm(step) < 0.01:
                break
        P = np.array([q[0], y, q[1]])
        err = float(np.sqrt(np.mean((np.linalg.norm(P - S, axis=1) - (d - q[2])) ** 2)))
        if best is None or err < best[1]:
            best = (q.copy(), err)
    q, err = best
    return {"x": round(float(q[0]), 1), "y": y, "z": round(float(q[1]), 1),
            "rms_error_cm": round(err, 1), "speakers_used": len(use)}


def analyze_voice(pcm, rate, channels, p):
    x = np.frombuffer(pcm, dtype="<i2").astype(np.float64).reshape(-1, channels).T
    if channels < 2:
        return {"kind": "voice", "error": "mono recording"}
    a, b = x[0], x[1]
    if np.allclose(a, b):
        return {"kind": "voice", "error": "both channels are the same: no second microphone in this recording"}
    n = 1 << int(math.ceil(math.log2(len(a) * 2)))
    A, B = np.fft.rfft(a, n), np.fft.rfft(b, n)
    G = A * np.conj(B)
    r = np.fft.irfft(G / (np.abs(G) + 1e-9), n)
    m = int(rate * 0.001)  # +-1 ms is far more than two mics on one board
    r = np.concatenate([r[-m:], r[: m + 1]])
    lag = int(np.argmax(r)) - m
    us = lag * 1e6 / rate
    out = {"kind": "voice", "lag_samples": lag, "lag_us": round(us, 1),
           "peak": round(float(r.max() / (np.mean(np.abs(r)) + 1e-9)), 1),
           "level_l": round(float(np.sqrt(np.mean(a * a))), 1),
           "level_r": round(float(np.sqrt(np.mean(b * b))), 1)}
    sp = p.get("mic_spacing_cm") or 0
    if sp:
        s = max(-1.0, min(1.0, us / 1e6 * SOUND_C / sp))
        out["angle_deg"] = round(math.degrees(math.asin(s)), 1)
    out["side"] = "left mic first" if lag > 0 else "right mic first" if lag < 0 else "both together"
    return out


class Locator:
    def __init__(self, cfg, desk):
        self.p = dict(LOCATE_DEFAULTS)
        self.p.update(cfg.get("locate") or {})
        self.desk = desk
        self.jobs = {}
        self.next_id = int(time.time()) % 10000 * 10
        self.lock = threading.Lock()
        self.dir = os.path.join(os.path.expanduser("~"), ".cache", "lilc-workspace", "locate")
        os.makedirs(self.dir, exist_ok=True)
        desk.on_sound = self._sound_event

    def start(self, dev, kind):
        with self.lock:
            self.next_id += 1
            n = self.next_id
            ms = 1000 * 4 if kind == "voice" else int(
                self.p["lead_ms"] + len(self.p["order"].split(",")) * (self.p["chirp_ms"] + self.p["gap_ms"]) + 1500)
            self.jobs[n] = {"id": n, "dev": dev, "kind": kind, "ms": ms, "rate": self.p["rate"],
                            "state": "waiting for the device", "t": time.time(), "sound": None}
        print("locate %d: %s on %s" % (n, kind, dev))
        return n

    def request_for(self, dev):
        """What /api/presence tells this device to record, if anything."""
        with self.lock:
            for j in self.jobs.values():
                if j["dev"] == dev and j["state"] == "waiting for the device" and time.time() - j["t"] < 20:
                    return {"id": j["id"], "ms": j["ms"], "rate": j["rate"]}
        return None

    def started(self, n):
        j = self.jobs.get(n)
        if not j:
            return
        j["state"] = "recording"
        if j["kind"] == "speakers":
            p = self.p
            # the device needs a moment after saying it has started
            threading.Timer(0.3, self.desk.send, args=(
                "sound %s %d %d %d %d %d %d %d %d %s" % (
                    p["speakers_on"], n % 65536, p["rate"], p["chirp_ms"], p["gap_ms"], p["lo_hz"],
                    p["hi_hz"], p["level"], p["lead_ms"], p["order"]),)).start()

    def _sound_event(self, screen, n, status, channels, rate):
        for j in self.jobs.values():
            if j["id"] % 65536 == n:
                if status == "playing":
                    j["sound"] = {"channels": channels, "rate": rate}
                elif status == "failed":
                    j["sound"] = {"failed": True}
                print("locate %d: %s %s (%s channels)" % (j["id"], screen, status, channels))

    def upload(self, n, pcm, rate, channels):
        j = self.jobs.get(n)
        if not j:
            return None
        base = os.path.join(self.dir, "%d-%s-%s" % (n, j["dev"], j["kind"]))
        with wave.open(base + ".wav", "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(pcm)
        try:
            if j["kind"] == "speakers":
                if j["sound"] is None:
                    res = {"kind": "speakers", "error": "the speakers never said they played "
                           "(is the %s agent new enough and connected?)" % self.p["speakers_on"]}
                elif j["sound"].get("failed"):
                    res = {"kind": "speakers", "error": "the sound card would not play the chirps"}
                else:
                    res = analyze_speakers(pcm, rate, channels, self.p, j["sound"]["channels"])
                    res["played"] = j["sound"]
            else:
                res = analyze_voice(pcm, rate, channels, self.p)
        except Exception as e:
            res = {"error": "analysis failed: %s" % e}
        res["wav"] = base + ".wav"
        with open(base + ".json", "w") as f:
            json.dump(res, f, indent=1)
        j["state"] = "done"
        j["result"] = res
        print("locate %d: %s" % (n, json.dumps(res)[:600]))
        return res
