/*
 * Test chirps for lil' C (RKM_SOUND), for the Windows agent.
 *
 * lil' C's bridge works out where he is on the desk by timing chirps played
 * from each speaker in turn, as heard by his microphones.  The hub sends the
 * recipe (rate, chirp length and sweep, gap, level, lead-in, which channels
 * in which order); this builds the sound here and plays it with waveOut.
 * It tries 5.1 first (WAVE_FORMAT_EXTENSIBLE, then plain 6-channel PCM),
 * then quad (front and rear pairs), then stereo, and says how many channels
 * it got in RKM_SOUND_EVENT.  A chirp for a speaker the layout doesn't
 * have is left out (that slot is silence).
 *
 * C89, Win32 waveOut (Windows 95 and up; 5.1 needs a driver that takes it,
 * usually Windows 2000/ME and later).  Included by rkm_win32.c.
 */
#include <mmsystem.h>
#include <math.h>

#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif

typedef struct {                 /* WAVEFORMATEXTENSIBLE, which old headers lack */
    WAVEFORMATEX Format;
    WORD wValidBitsPerSample;
    DWORD dwChannelMask;
    GUID SubFormat;
} RKM_WFX_EXT;

static const GUID rkm_pcm_guid = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static HWAVEOUT snd_out;
static WAVEHDR snd_hdr;
static HGLOBAL snd_mem;
static unsigned snd_id;
static int snd_busy;

static void sound_event(unsigned id, int status, int channels, unsigned long rate);  /* in rkm_win32.c */

static int snd_try(HWND w, int ch, unsigned long rate, int ext)
{
    RKM_WFX_EXT f;
    memset(&f, 0, sizeof f);
    f.Format.wFormatTag = (WORD)(ext ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM);
    f.Format.nChannels = (WORD)ch;
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = 16;
    f.Format.nBlockAlign = (WORD)(ch * 2);
    f.Format.nAvgBytesPerSec = rate * ch * 2;
    if (ext) {
        f.Format.cbSize = 22;
        f.wValidBitsPerSample = 16;
        f.dwChannelMask = ch == 6 ? 0x3F : ch == 4 ? 0x33 : 0x03;  /* +FC LFE / BL BR */
        f.SubFormat = rkm_pcm_guid;
    }
    return waveOutOpen(&snd_out, WAVE_MAPPER, (WAVEFORMATEX *)&f, (DWORD)w, 0, CALLBACK_WINDOW) == MMSYSERR_NOERROR;
}

/* p: id16 rate32 chirp16 gap16 lo16 hi16 level lead16 n channel[n] */
static void sound_play(HWND w, const unsigned char *p, unsigned len)
{
    unsigned long rate, frames, i, start, cl;
    unsigned chirp_ms, gap_ms, lo, hi, lead_ms, n, k;
    int level, ch = 0, j;
    short *buf;
    double amp;

    if (len < 18) return;
    snd_id = RKM_GET16(p);
    rate = RKM_GET32(p + 2);
    chirp_ms = RKM_GET16(p + 6);
    gap_ms = RKM_GET16(p + 8);
    lo = RKM_GET16(p + 10);
    hi = RKM_GET16(p + 12);
    level = p[14] > 100 ? 100 : p[14];
    lead_ms = RKM_GET16(p + 15);
    n = p[17];
    if (len < 18 + n || n == 0 || rate < 8000 || rate > 96000 || chirp_ms == 0 || chirp_ms > 2000) {
        sound_event(snd_id, RKM_SOUND_FAILED, 0, 0);
        return;
    }
    if (snd_busy) { sound_event(snd_id, RKM_SOUND_FAILED, 0, 0); return; }

    /* the card: 5.1 if it will, else stereo; the asked rate, else 44.1 kHz */
    for (j = 0; j < 2 && !ch; j++) {
        unsigned long r = j == 0 ? rate : 44100UL;
        if (snd_try(w, 6, r, 1) || snd_try(w, 6, r, 0)) { ch = 6; rate = r; }
        else if (snd_try(w, 4, r, 1) || snd_try(w, 4, r, 0)) { ch = 4; rate = r; }
        else if (snd_try(w, 2, r, 1) || snd_try(w, 2, r, 0)) { ch = 2; rate = r; }
    }
    if (!ch) { sound_event(snd_id, RKM_SOUND_FAILED, 0, 0); return; }

    frames = rate * (lead_ms + n * (unsigned long)(chirp_ms + gap_ms)) / 1000 + rate / 5;
    snd_mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, frames * ch * 2);
    buf = snd_mem ? (short *)GlobalLock(snd_mem) : NULL;
    if (!buf) {
        if (snd_mem) GlobalFree(snd_mem);
        snd_mem = NULL;
        waveOutClose(snd_out);
        sound_event(snd_id, RKM_SOUND_FAILED, 0, 0);
        return;
    }
    /* a linear sweep lo..hi Hz with a Hann window, one per channel in turn */
    cl = rate * chirp_ms / 1000;
    amp = level / 100.0 * 32000.0;
    for (k = 0; k < n; k++) {
        /* where that speaker sits in a frame: 5.1 FL FR FC LFE RL RR, quad FL FR RL RR */
        static const signed char quad[6] = { 0, 1, -1, -1, 2, 3 };
        int c = p[18 + k];
        double T = chirp_ms / 1000.0;
        if (c > 5) continue;
        if (ch == 4) c = quad[c];
        if (c < 0 || c >= ch) continue;           /* the layout has no such speaker */
        start = rate * (lead_ms + k * (unsigned long)(chirp_ms + gap_ms)) / 1000;
        for (i = 0; i < cl && start + i < frames; i++) {
            double t = (double)i / rate;
            double ph = 2 * 3.14159265358979 * (lo * t + (hi - (double)lo) * t * t / (2 * T));
            double win = 0.5 - 0.5 * cos(2 * 3.14159265358979 * i / (cl - 1));
            buf[(start + i) * ch + c] = (short)(amp * win * sin(ph));
        }
    }
    memset(&snd_hdr, 0, sizeof snd_hdr);
    snd_hdr.lpData = (LPSTR)buf;
    snd_hdr.dwBufferLength = frames * ch * 2;
    waveOutPrepareHeader(snd_out, &snd_hdr, sizeof snd_hdr);
    if (waveOutWrite(snd_out, &snd_hdr, sizeof snd_hdr) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(snd_out, &snd_hdr, sizeof snd_hdr);
        waveOutClose(snd_out);
        GlobalUnlock(snd_mem);
        GlobalFree(snd_mem);
        snd_mem = NULL;
        sound_event(snd_id, RKM_SOUND_FAILED, 0, 0);
        return;
    }
    snd_busy = 1;
    sound_event(snd_id, RKM_SOUND_PLAYING, ch, rate);
}

/* MM_WOM_DONE: the sound has finished */
static void sound_done(void)
{
    if (!snd_busy) return;
    waveOutUnprepareHeader(snd_out, &snd_hdr, sizeof snd_hdr);
    waveOutClose(snd_out);
    GlobalUnlock(snd_mem);
    GlobalFree(snd_mem);
    snd_mem = NULL;
    snd_busy = 0;
    sound_event(snd_id, RKM_SOUND_DONE, 0, 0);
}
