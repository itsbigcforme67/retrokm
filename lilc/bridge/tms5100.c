/* Fast TMS5100 synthesis for lilc_speakspell.py (same logic as its Python
 * TMS5100 class). Build: gcc -O2 -shared -fPIC -o tms5100.so tms5100.c
 *
 * frames: n rows of 13 ints: energy, pitch period, kind (0 silent, 1 voiced,
 * 2 unvoiced), K1..K10 (already looked up in the chip's tables).
 * out: n * 200 samples. CHIRP / INTERP tables are passed in from Python.
 */
#include <stdint.h>

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void tms_render(const int32_t *frames, int n, const int32_t *chirp,
                const int32_t *interp, int16_t *out) {
  int u[11] = {0}, x[10] = {0};
  int rng = 0x1FFF, pitch_count = 0;
  int cur_e = 0, cur_p = 0, cur_k[10] = {0};
  int prev_voiced = 0, prev_silent = 1;
  int o = 0;
  for (int f = 0; f < n; f++) {
    const int32_t *fr = frames + f * 13;
    int kind = fr[2], silent = kind == 0, voiced = kind == 1;
    int tgt_e = silent ? 0 : fr[0];
    int tgt_p = silent ? cur_p : fr[1];
    int tgt_k[10];
    for (int j = 0; j < 10; j++) tgt_k[j] = silent ? cur_k[j] : fr[3 + j];
    if (prev_silent || silent || voiced != prev_voiced) {
      cur_e = tgt_e; cur_p = tgt_p;
      for (int j = 0; j < 10; j++) cur_k[j] = tgt_k[j];
    }
    for (int ip = 0; ip < 8; ip++) {
      if (ip) {
        int sh = interp[ip];
        cur_e += (tgt_e - cur_e) >> sh;
        cur_p += (tgt_p - cur_p) >> sh;
        for (int j = 0; j < 10; j++) cur_k[j] += (tgt_k[j] - cur_k[j]) >> sh;
      }
      for (int s = 0; s < 25; s++) {
        int period = voiced ? cur_p : 0, exc;
        if (period == 0) {
          for (int r = 0; r < 20; r++) {
            int bit = ((rng >> 12) ^ (rng >> 3) ^ (rng >> 2) ^ rng) & 1;
            rng = ((rng << 1) | bit) & 0x1FFF;
          }
          exc = (rng & 1) ? -0x40 : 0x40;
        } else {
          exc = chirp[pitch_count < 51 ? pitch_count : 51];
          if (++pitch_count >= period) pitch_count = 0;
        }
        u[10] = (cur_e * (exc << 6)) >> 9;
        for (int i = 9; i >= 0; i--)
          u[i] = clampi(u[i + 1] - ((cur_k[i] * x[i]) >> 9), -16384, 16383);
        for (int i = 9; i > 0; i--) x[i] = x[i - 1] + ((cur_k[i - 1] * u[i - 1]) >> 9);
        x[0] = u[0];
        int v = clampi(u[0], -2048, 2047);
        out[o++] = (int16_t)((v >> 4) << 8);
      }
    }
    cur_e = tgt_e; cur_p = tgt_p;
    for (int j = 0; j < 10; j++) cur_k[j] = tgt_k[j];
    prev_voiced = voiced; prev_silent = silent;
  }
}
