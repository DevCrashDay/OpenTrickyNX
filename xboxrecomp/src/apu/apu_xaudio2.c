/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2      /* stereo output, as before the 5.1 option */
#define XA2_MAX_CHANNELS  6      /* 5.1 output: FL FR C LFE SL SR */
#define XA2_BUF_SAMPLES   256    /* ~5.3 ms per submission; see XA2_TARGET_QUEUED */
/* The frame thread keeps XA2_TARGET_QUEUED of these filled (see xa2_queued
 * and throttle() in apu_core.c), so there is room for jitter on both sides.
 * With three, and a submit that dropped the buffer whenever all three were
 * queued, every pacing wobble became a click. */
#define XA2_NUM_BUFS      24
static unsigned long g_xa2_underruns = 0, g_xa2_drops = 0;

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES * XA2_MAX_CHANNELS];
static int                     g_xa2_channels = XA2_CHANNELS;  /* source voice: 2 or 6 */
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static uint64_t                g_xa2_samples_sent = 0;
/* Queue latency over the current report window: samples submitted
 * and not yet played, sampled at every submit. */
static uint64_t                g_xa2_lat_sum = 0, g_xa2_lat_n = 0, g_xa2_lat_max = 0;

int xa2_target_queued(void)
{
    static int target = 0;
    if (!target) {
        const char *e = getenv("XBOX_AUDIO_QUEUE");
        int n = (e && *e) ? atoi(e) : XA2_TARGET_QUEUED;
        if (n < XA2_QUEUE_MIN) n = XA2_QUEUE_MIN;
        if (n > XA2_QUEUE_MAX) n = XA2_QUEUE_MAX;
        target = n;
        fprintf(stderr, "[XA2] queue target %d buffers (~%.1f ms)%s\n", target,
                target * XA2_BUF_SAMPLES * 1000.0 / XA2_SAMPLE_RATE,
                (e && *e) ? " from XBOX_AUDIO_QUEUE" : "");
    }
    return target;
}

/* ---- 5.1 output ------------------------------------------------------
 *
 * The title's EA mixer renders 5.1 itself: one APU voice per speaker bin
 * (mixbins 0..5 = FL FR C LFE SL SR), dialogue and countdown in the centre.
 * The console's encoder stage took those bins as they are. With a 5.1 (or
 * larger) Windows device the six bins go out as six channels; otherwise the
 * output stays the stereo fold of apu_dsp.c, unchanged. XAudio2 is never
 * left to fold 6 -> 2 itself: its coefficients differ from ours, and the
 * stereo output would no longer be the same as before.
 *
 * XBOX_AUDIO_OUTPUT: auto (default) = 5.1 when the device has the speakers,
 * stereo = always stereo (the code path of the port before the option),
 * 5.1 = always 5.1, Windows mapping it onto a device without the speakers. */

/* Windows speaker position bits (ksmedia.h), local to need no KS headers. */
#define XA2_SPK_FL   0x001u
#define XA2_SPK_FR   0x002u
#define XA2_SPK_FC   0x004u
#define XA2_SPK_LFE  0x008u
#define XA2_SPK_BL   0x010u
#define XA2_SPK_BR   0x020u
#define XA2_SPK_SL   0x200u
#define XA2_SPK_SR   0x400u

enum { XA2_OUT_AUTO, XA2_OUT_STEREO, XA2_OUT_51 };

/* KSDATAFORMAT_SUBTYPE_PCM, spelled out (no ksguid import library needed). */
static const GUID k_xa2_subtype_pcm =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

/* Mastering voice channels and where each of the six source channels lands
 * in them, for the explicit output matrix. */
static UINT32 g_xa2_master_ch = 0;
static int    g_xa2_dst[XA2_MAX_CHANNELS];
static DWORD  g_xa2_src_mask = 0;

static int xa2_output_mode(void)
{
    const char *e = getenv("XBOX_AUDIO_OUTPUT");
    if (!e || !*e || !_stricmp(e, "auto")) return XA2_OUT_AUTO;
    if (!_stricmp(e, "stereo") || !strcmp(e, "2")) return XA2_OUT_STEREO;
    if (!strcmp(e, "5.1") || !strcmp(e, "51") || !strcmp(e, "6")) return XA2_OUT_51;
    fprintf(stderr, "[XA2] XBOX_AUDIO_OUTPUT=%s not understood (auto, stereo, 5.1): auto\n", e);
    return XA2_OUT_AUTO;
}

/* Channel index of speaker `spk` in a device mask (channels are in bit
 * order), or -1 when the device does not have it. */
static int xa2_spk_index(DWORD mask, DWORD spk)
{
    DWORD b;
    int n = 0;
    if (!(mask & spk)) return -1;
    for (b = 1; b < spk; b <<= 1)
        if (mask & b) n++;
    return n;
}

/* Try a mastering voice for 5.1. Returns 1 with g_xa2_master open and the
 * routing set, or 0 with no mastering voice (the caller opens stereo). */
static int xa2_open_master_51(int mode)
{
    XAUDIO2_VOICE_DETAILS det;
    DWORD mask = 0, sl, sr;
    HRESULT hr;

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XAUDIO2_DEFAULT_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] device query failed (0x%08lX): stereo output\n", hr);
        g_xa2_master = NULL;
        return 0;
    }
    memset(&det, 0, sizeof(det));
    IXAudio2MasteringVoice_GetVoiceDetails(g_xa2_master, &det);
    if (FAILED(IXAudio2MasteringVoice_GetChannelMask(g_xa2_master, &mask)))
        mask = 0;

    /* Surrounds: the side pair when the device has it (5.1 "surround",
     * 7.1: the ITU 5.1 surrounds sit at +-110 degrees, the sides of a 7.1
     * room), else the back pair (5.1 "back"). */
    if ((mask & XA2_SPK_SL) && (mask & XA2_SPK_SR)) { sl = XA2_SPK_SL; sr = XA2_SPK_SR; }
    else                                             { sl = XA2_SPK_BL; sr = XA2_SPK_BR; }

    if (det.InputChannels >= XA2_MAX_CHANNELS &&
        (mask & XA2_SPK_FL) && (mask & XA2_SPK_FR) && (mask & XA2_SPK_FC) &&
        (mask & XA2_SPK_LFE) && (mask & sl) && (mask & sr)) {
        g_xa2_master_ch = det.InputChannels;
        g_xa2_src_mask = XA2_SPK_FL | XA2_SPK_FR | XA2_SPK_FC | XA2_SPK_LFE | sl | sr;
        g_xa2_dst[0] = xa2_spk_index(mask, XA2_SPK_FL);
        g_xa2_dst[1] = xa2_spk_index(mask, XA2_SPK_FR);
        g_xa2_dst[2] = xa2_spk_index(mask, XA2_SPK_FC);
        g_xa2_dst[3] = xa2_spk_index(mask, XA2_SPK_LFE);
        g_xa2_dst[4] = xa2_spk_index(mask, sl);
        g_xa2_dst[5] = xa2_spk_index(mask, sr);
        fprintf(stderr, "[XA2] output device: %u channels, speaker mask 0x%lX -> 5.1 "
                "(surrounds on the %s pair)\n", (unsigned)det.InputChannels,
                (unsigned long)mask, sl == XA2_SPK_SL ? "side" : "back");
        return 1;
    }

    IXAudio2MasteringVoice_DestroyVoice(g_xa2_master);
    g_xa2_master = NULL;
    if (mode == XA2_OUT_AUTO) {
        fprintf(stderr, "[XA2] output device: %u channels, speaker mask 0x%lX -> "
                "no 5.1 speakers, stereo output\n", (unsigned)det.InputChannels,
                (unsigned long)mask);
        return 0;
    }

    /* 5.1 asked for on a device without the speakers: a 6-channel mastering
     * voice, which Windows maps onto the device (its own downmix). */
    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_MAX_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] 5.1 asked for, device has %u channels (mask 0x%lX) and a "
                "6-channel output failed (0x%08lX): stereo output\n",
                (unsigned)det.InputChannels, (unsigned long)mask, hr);
        g_xa2_master = NULL;
        return 0;
    }
    g_xa2_master_ch = XA2_MAX_CHANNELS;
    g_xa2_src_mask = XA2_SPK_FL | XA2_SPK_FR | XA2_SPK_FC | XA2_SPK_LFE | XA2_SPK_BL | XA2_SPK_BR;
    {
        int c;
        for (c = 0; c < XA2_MAX_CHANNELS; c++) g_xa2_dst[c] = c;
    }
    fprintf(stderr, "[XA2] 5.1 asked for: device has %u channels (mask 0x%lX), "
            "6-channel output mapped by Windows\n", (unsigned)det.InputChannels,
            (unsigned long)mask);
    return 1;
}

/* Each source channel to its own speaker at full level, nothing else, so
 * XAudio2 does no mixing of its own. */
static void xa2_route_51(void)
{
    float *m;
    UINT32 c;
    HRESULT hr;

    if (!g_xa2_master_ch) return;
    m = (float *)calloc((size_t)XA2_MAX_CHANNELS * g_xa2_master_ch, sizeof(float));
    if (!m) return;
    for (c = 0; c < XA2_MAX_CHANNELS; c++)
        if (g_xa2_dst[c] >= 0 && (UINT32)g_xa2_dst[c] < g_xa2_master_ch)
            m[(UINT32)g_xa2_dst[c] * XA2_MAX_CHANNELS + c] = 1.0f;
    hr = IXAudio2SourceVoice_SetOutputMatrix(g_xa2_source, NULL, XA2_MAX_CHANNELS,
                                             g_xa2_master_ch, m, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr))
        fprintf(stderr, "[XA2] SetOutputMatrix failed (0x%08lX): XAudio2's own 5.1 mapping\n", hr);
    free(m);
}

int xa2_channels(void)
{
    return g_xa2_initialized ? g_xa2_channels : XA2_CHANNELS;
}

int xa2_init(void)
{
    HRESULT hr;
    WAVEFORMATEX wfx = { 0 };
    int mode;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != (HRESULT)0x80010106 /* RPC_E_CHANGED_MODE */ && hr != S_FALSE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }

    /* XAudio2 ships as a COM DLL and MinGW provides the header but no import
     * library, so resolve the entry point at runtime. 2.9 is present on
     * Windows 10 and later; 2.8 covers Windows 8. */
    {
        typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT32, UINT32);
        static const char *const dlls[] = { "xaudio2_9.dll", "xaudio2_8.dll" };
        PFN_XAudio2Create create = NULL;
        HMODULE lib = NULL;
        size_t i;

        for (i = 0; i < sizeof(dlls) / sizeof(dlls[0]) && !create; i++) {
            lib = LoadLibraryA(dlls[i]);
            if (!lib) continue;
            create = (PFN_XAudio2Create)(void *)GetProcAddress(lib, "XAudio2Create");
            if (!create) { FreeLibrary(lib); lib = NULL; }
        }
        if (!create) {
            fprintf(stderr, "[XA2] no XAudio2 runtime found -- audio output disabled\n");
            return 0;
        }
        hr = create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    }
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        return 0;
    }

    mode = xa2_output_mode();
    g_xa2_channels = XA2_CHANNELS;
    g_xa2_master = NULL;
    if (mode != XA2_OUT_STEREO && xa2_open_master_51(mode))
        g_xa2_channels = XA2_MAX_CHANNELS;

    /* Stereo: the mastering voice exactly as before the 5.1 option. */
    if (!g_xa2_master) {
        hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
            XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
        if (FAILED(hr)) {
            fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
            IXAudio2_Release(g_xa2);
            g_xa2 = NULL;
            return 0;
        }
    }

    if (g_xa2_channels == XA2_MAX_CHANNELS) {
        WAVEFORMATEXTENSIBLE ext;
        memset(&ext, 0, sizeof(ext));
        ext.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        ext.Format.nChannels       = XA2_MAX_CHANNELS;
        ext.Format.nSamplesPerSec  = XA2_SAMPLE_RATE;
        ext.Format.wBitsPerSample  = 16;
        ext.Format.nBlockAlign     = XA2_MAX_CHANNELS * 2;
        ext.Format.nAvgBytesPerSec = XA2_SAMPLE_RATE * ext.Format.nBlockAlign;
        ext.Format.cbSize          = sizeof(ext) - sizeof(WAVEFORMATEX);
        ext.Samples.wValidBitsPerSample = 16;
        ext.dwChannelMask          = g_xa2_src_mask;
        ext.SubFormat              = k_xa2_subtype_pcm;
        hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
            &ext.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
        if (SUCCEEDED(hr)) {
            xa2_route_51();
        } else {
            /* Back to the stereo output, mastering voice included. */
            fprintf(stderr, "[XA2] 6-channel source voice failed (0x%08lX): stereo output\n", hr);
            g_xa2_source = NULL;
            g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
            g_xa2_master = NULL;
            g_xa2_channels = XA2_CHANNELS;
            hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
                XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
            if (FAILED(hr)) {
                fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
                IXAudio2_Release(g_xa2);
                g_xa2 = NULL;
                return 0;
            }
        }
    }

    if (g_xa2_channels == XA2_CHANNELS) {
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = XA2_CHANNELS;
        wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = XA2_CHANNELS * 2;
        wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

        hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
            &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
        return 0;
    }

    {   /* XBOX_MUTE=1: silence the speakers for test runs (benches, menu
         * checks) started by tools on a shared machine. Only the mastering
         * voice volume changes: the APU, the mixer, the submitted buffers and
         * the XBOX_AUDIO_WAV capture (taken from the submitted buffers, before
         * this volume) stay exactly as without it. Unset or "0": sound on. */
        const char *m = getenv("XBOX_MUTE");
        if (m && *m && strcmp(m, "0") != 0) {
            float vol = -1.0f;
            HRESULT vh = g_xa2_master->lpVtbl->SetVolume(g_xa2_master, 0.0f,
                                                          XAUDIO2_COMMIT_NOW);
            g_xa2_master->lpVtbl->GetVolume(g_xa2_master, &vol);
            fprintf(stderr, "[XA2] Fork: XBOX_MUTE -- output muted "
                    "(mastering volume %.2f, hr 0x%08lX); WAV capture unaffected\n",
                    vol, (unsigned long)vh);
        }
    }

    IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_samples_sent = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz %s 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, g_xa2_channels == XA2_MAX_CHANNELS ? "5.1 (6 ch)" : "stereo",
            XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_xa2_initialized) return;

    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Buffers the device still has to play -- the clock the APU frame thread
 * paces itself by. */
int xa2_queued(void)
{
    XAUDIO2_VOICE_STATE state;
    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)state.BuffersQueued;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, 0);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) { g_xa2_drops++; return 0; }
    if (g_xa2_samples_sent >= state.SamplesPlayed) {
        uint64_t q = g_xa2_samples_sent - state.SamplesPlayed;
        g_xa2_lat_sum += q;
        g_xa2_lat_n++;
        if (q > g_xa2_lat_max) g_xa2_lat_max = q;
    }
    if (state.BuffersQueued == 0 && g_xa2_frames_written > XA2_NUM_BUFS)
        g_xa2_underruns++;          /* the device ran dry before this arrived */

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * g_xa2_channels * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * g_xa2_channels * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);

    {   /* XBOX_AUDIO_WAV=<path> (diagnostic): append exactly what is
         * submitted to a 48 kHz 16-bit WAV, header patched as it grows, so a
         * run's sound can be inspected rather than described. */
        static FILE *wf = NULL;
        static int init = 0;
        static uint32_t bytes = 0;
        if (!init) {
            const char *p = getenv("XBOX_AUDIO_WAV");
            init = 1;
            if (p && *p) {
                wf = fopen(p, "wb");
                if (wf) {
                    uint8_t h[44] = {0};
                    fwrite(h, 1, 44, wf);
                }
            }
        }
        if (wf) {
            uint32_t n = (uint32_t)xbuf.AudioBytes;
            fwrite(xbuf.pAudioData, 1, n, wf);
            bytes += n;
            if ((g_xa2_frames_written & 31) == 0) {
                long pos = ftell(wf);
                uint32_t rate = 48000, byterate = 48000 * g_xa2_channels * 2, v;
                uint16_t s;
                fseek(wf, 0, SEEK_SET);
                fwrite("RIFF", 1, 4, wf); v = 36 + bytes; fwrite(&v, 4, 1, wf);
                fwrite("WAVEfmt ", 1, 8, wf); v = 16; fwrite(&v, 4, 1, wf);
                s = 1; fwrite(&s, 2, 1, wf); s = (uint16_t)g_xa2_channels; fwrite(&s, 2, 1, wf);
                fwrite(&rate, 4, 1, wf); fwrite(&byterate, 4, 1, wf);
                s = (uint16_t)(g_xa2_channels * 2); fwrite(&s, 2, 1, wf); s = 16; fwrite(&s, 2, 1, wf);
                fwrite("data", 1, 4, wf); fwrite(&bytes, 4, 1, wf);
                fseek(wf, pos, SEEK_SET);
                fflush(wf);
            }
        }
    }

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    g_xa2_samples_sent += (uint64_t)copy_samples;

    /* Say whether what reaches the speakers is sound or silence. A buffer
     * count alone cannot tell -- the pipeline submits zeros whenever no voice
     * is playing, so "frames written" rises either way. Reported every 5 s
     * while anything non-silent has been heard, and once when output first
     * stops being silent. */
    {
        static int peak = 0, announced = 0;
        static unsigned long loud = 0, last_tick = 0;
        int k, bpeak = 0;
        for (k = 0; k < copy_samples * g_xa2_channels; k++) {
            int v = samples[k] < 0 ? -samples[k] : samples[k];
            if (v > bpeak) bpeak = v;
        }
        if (bpeak > 64) loud++;
        if (bpeak > peak) peak = bpeak;
        if (loud && !announced) {
            announced = 1;
            fprintf(stderr, "[XA2] first non-silent buffer (peak %d) after %d buffers\n",
                    bpeak, g_xa2_frames_written);
            fflush(stderr);
        }
        if (loud) {
            unsigned long now = GetTickCount();
            if (now - last_tick >= 5000) {
                last_tick = now;
                XAUDIO2_PERFORMANCE_DATA perf;
                memset(&perf, 0, sizeof(perf));
                IXAudio2_GetPerformanceData(g_xa2, &perf);
                fprintf(stderr, "[XA2] %d buffers sent, %lu non-silent, peak %d, "
                        "%lu underruns, %lu dropped\n",
                        g_xa2_frames_written, loud, peak, g_xa2_underruns, g_xa2_drops);
                /* Queue target, measured queue latency over the last
                 * window (mean / max, submitted but unplayed), the engine's
                 * own device latency and its glitch count. */
                fprintf(stderr, "[XA2] queue %d: latency mean %.1f ms max %.1f ms, "
                        "device %.1f ms, glitches %u\n", xa2_target_queued(),
                        g_xa2_lat_n ? (double)g_xa2_lat_sum / (double)g_xa2_lat_n * 1000.0 / XA2_SAMPLE_RATE : 0.0,
                        (double)g_xa2_lat_max * 1000.0 / XA2_SAMPLE_RATE,
                        perf.CurrentLatencyInSamples * 1000.0 / XA2_SAMPLE_RATE,
                        (unsigned)perf.GlitchesSinceEngineStarted);
                g_xa2_lat_sum = g_xa2_lat_n = g_xa2_lat_max = 0;
                fflush(stderr);
            }
        }
    }
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- SDL2 audio (Linux, the Switch) */

/*
 * The same contract as the XAudio2 path above, on an SDL2 audio device: the
 * APU frame thread submits 256-sample buffers and paces itself by how many
 * are still waiting (xa2_queued). Stereo by default; XBOX_AUDIO_OUTPUT=5.1
 * asks the device for six channels (FL FR C LFE SL SR, the order SDL and
 * the title's mix bins share) and falls back to stereo if it has fewer.
 */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SWITCH__)
#include <malloc.h>
#include <switch.h>
#endif

#define XA2_SAMPLE_RATE   48000
#define XA2_BUF_SAMPLES   256
#define XA2_NUM_BUFS      24
#define XA2_FIFO          (XA2_NUM_BUFS * XA2_BUF_SAMPLES * 6)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The device pulls from this FIFO in its callback (SDL's own queue leaves no
 * way into the device thread, and on the Switch that thread needs a raised
 * priority, below). Guarded by XA2_LOCK. Counts in int16 values. */
static SDL_AudioDeviceID g_dev;
#if defined(__SWITCH__)
static Mutex             g_mx;
#define XA2_LOCK()   mutexLock(&g_mx)
#define XA2_UNLOCK() mutexUnlock(&g_mx)
#else
#define XA2_LOCK()   SDL_LockAudioDevice(g_dev)
#define XA2_UNLOCK() SDL_UnlockAudioDevice(g_dev)
#endif
static int               g_channels = 2;
static int16_t           g_fifo[XA2_FIFO];
static int               g_rd, g_fill;
static unsigned long     g_drops, g_buffers, g_callbacks, g_underruns;
static int               g_tone;             /* XBOX_AUDIO_TONE=1 */
static double            g_tone_phase;

int xa2_target_queued(void)
{
    static int target = 0;
    if (!target) {
        const char *e = getenv("XBOX_AUDIO_QUEUE");
        int n = (e && *e) ? atoi(e) : XA2_TARGET_QUEUED;
        if (n < XA2_QUEUE_MIN) n = XA2_QUEUE_MIN;
        if (n > XA2_QUEUE_MAX) n = XA2_QUEUE_MAX;
        target = n;
        fprintf(stderr, "[XA2] queue target %d buffers (~%.1f ms)%s\n", target,
                target * XA2_BUF_SAMPLES * 1000.0 / XA2_SAMPLE_RATE,
                (e && *e) ? " from XBOX_AUDIO_QUEUE" : "");
    }
    return target;
}

/* SDL's Switch port starts the device thread at 0x3B, the lowest application
 * priority and the one every game thread runs at: with the game busy it
 * barely ran, audren got next to no buffers and nothing was heard. fo2x hit
 * the same and raises it to 0x2B (OT_AUDIO_PRIO overrides, 0 leaves it). The
 * thread sleeps between device periods, so it costs the game nothing. */
static void xa2_device_thread_setup(void)
{
#if defined(__SWITCH__)
    const char *e = getenv("OT_AUDIO_PRIO");
    int prio = (e && *e) ? (int)strtol(e, NULL, 0) : 0x2B;
    if (prio > 0) {
        Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
        fprintf(stderr, "[XA2] audio device thread at priority 0x%X (rc 0x%X)\n", prio, rc);
    }
#endif
}

static void SDLCALL xa2_callback(void *user, Uint8 *stream, int len)
{
    int16_t *out = (int16_t *)stream;
    int n = len / (int)sizeof(int16_t), take, first, i;
    (void)user;
    if (!g_callbacks++) xa2_device_thread_setup();
#if defined(__SWITCH__)
    XA2_LOCK();                         /* SDL holds its device lock here */
#endif
    take = n < g_fill ? n : g_fill;
    first = XA2_FIFO - g_rd < take ? XA2_FIFO - g_rd : take;
    memcpy(out, g_fifo + g_rd, (size_t)first * sizeof(int16_t));
    memcpy(out + first, g_fifo, (size_t)(take - first) * sizeof(int16_t));
    g_rd = (g_rd + take) % XA2_FIFO;
    g_fill -= take;
    if (take < n) {
        memset(out + take, 0, (size_t)(n - take) * sizeof(int16_t));
        if (g_buffers) g_underruns++;
    }
#if defined(__SWITCH__)
    XA2_UNLOCK();
#endif
    if (g_tone) {
        /* 440 Hz on every channel, over whatever the game plays: proves the
         * device path on its own. */
        for (i = 0; i < n / g_channels; i++) {
            int c, v = (int)(sin(g_tone_phase) * 6000.0);
            g_tone_phase += 2.0 * M_PI * 440.0 / XA2_SAMPLE_RATE;
            if (g_tone_phase >= 2.0 * M_PI) g_tone_phase -= 2.0 * M_PI;
            for (c = 0; c < g_channels; c++) {
                int s = out[i * g_channels + c] + v;
                out[i * g_channels + c] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
            }
        }
    }
}

#if defined(__SWITCH__)
/* The Switch: audout directly, with XA2_NX_BUFS buffers queued at the device.
 * SDL's Switch driver (SDL2 2.28, devkitPro) hands audren one buffer and waits
 * until it has finished playing before it asks for the next, so between any
 * two buffers the output stopped for a wake-up and a callback: 1.7 ms of
 * silence per 10.7 ms in Ash's log (810 buffers played per 10 s instead of
 * 938), heard as constant stutter, and the game's mix, produced at the real
 * rate, overflowed the queue and was dropped in chunks. */
#define XA2_NX_BUFS    4
#define XA2_NX_SAMPLES 512                      /* 10.7 ms each */
static AudioOutBuffer g_nx_buf[XA2_NX_BUFS];
static Thread         g_nx_thread;
static volatile int   g_nx_stop;

static void xa2_nx_fill(AudioOutBuffer *b)
{
    b->data_size = XA2_NX_SAMPLES * 2 * sizeof(int16_t);
    b->data_offset = 0;
    xa2_callback(NULL, (Uint8 *)b->buffer, (int)b->data_size);
    armDCacheFlush(b->buffer, b->data_size);
}

static void xa2_nx_thread(void *arg)
{
    (void)arg;
    while (!g_nx_stop) {
        AudioOutBuffer *rel = NULL;
        u32 n = 0;
        if (R_FAILED(audoutWaitPlayFinish(&rel, &n, 100000000ull)) || !rel || !n) continue;
        do {
            xa2_nx_fill(rel);
            audoutAppendAudioOutBuffer(rel);
            rel = NULL;
            n = 0;
        } while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&rel, &n)) && rel && n);
    }
}

int xa2_init(void)
{
    const char *t = getenv("XBOX_AUDIO_TONE");
    const char *e = getenv("OT_AUDIO_PRIO");
    int prio = (e && *e) ? (int)strtol(e, NULL, 0) : 0x2B, i;
    Result rc;

    if (g_dev) return 1;
    if (prio <= 0 || prio > 0x3F) prio = 0x3B;
    g_tone = t && t[0] == '1';
    g_channels = 2;
    mutexInit(&g_mx);
    if (R_FAILED(rc = audoutInitialize())) {
        fprintf(stderr, "[XA2] audout unavailable (0x%X)\n", rc);
        return 0;
    }
    if (audoutGetSampleRate() != XA2_SAMPLE_RATE || audoutGetChannelCount() != 2 ||
        audoutGetPcmFormat() != PcmFormat_Int16 || R_FAILED(rc = audoutStartAudioOut())) {
        fprintf(stderr, "[XA2] audout: %u Hz, %u channel(s), format %d (rc 0x%X): no sound\n",
                audoutGetSampleRate(), audoutGetChannelCount(), (int)audoutGetPcmFormat(), rc);
        audoutExit();
        return 0;
    }
    g_dev = 1;                          /* what the queue below tests for */
    for (i = 0; i < XA2_NX_BUFS; i++) {
        size_t sz = (XA2_NX_SAMPLES * 2 * sizeof(int16_t) + 0xFFF) & ~(size_t)0xFFF;
        g_nx_buf[i].buffer = memalign(0x1000, sz);
        g_nx_buf[i].buffer_size = sz;
        memset(g_nx_buf[i].buffer, 0, sz);
        g_nx_buf[i].data_size = XA2_NX_SAMPLES * 2 * sizeof(int16_t);
        armDCacheFlush(g_nx_buf[i].buffer, sz);
        audoutAppendAudioOutBuffer(&g_nx_buf[i]);
    }
    g_nx_stop = 0;
    g_callbacks = 1;                    /* the thread starts at its priority */
    if (R_FAILED(rc = threadCreate(&g_nx_thread, xa2_nx_thread, NULL, NULL, 0x8000, prio, -2)) ||
        R_FAILED(rc = threadStart(&g_nx_thread))) {
        fprintf(stderr, "[XA2] audout thread failed (0x%X)\n", rc);
        audoutStopAudioOut();
        audoutExit();
        g_dev = 0;
        return 0;
    }
    fprintf(stderr, "[XA2] audout: 48000 Hz, 2 channels, %d x %d-sample buffers, thread priority 0x%X%s\n",
            XA2_NX_BUFS, XA2_NX_SAMPLES, prio,
            g_tone ? ", 440 Hz test tone on (XBOX_AUDIO_TONE=1)" : "");
    return 1;
}

void xa2_shutdown(void)
{
    int i;
    if (g_dev) {
        g_nx_stop = 1;
        threadWaitForExit(&g_nx_thread);
        threadClose(&g_nx_thread);
        audoutStopAudioOut();
        audoutExit();
        for (i = 0; i < XA2_NX_BUFS; i++) free(g_nx_buf[i].buffer);
        g_dev = 0;
    }
    fprintf(stderr, "[XA2] %lu buffers sent, %lu dropped, %lu device buffers, %lu underruns\n",
            g_buffers, g_drops, g_callbacks, g_underruns);
}
#else
int xa2_init(void)
{
    SDL_AudioSpec want, have;
    const char *e = getenv("XBOX_AUDIO_OUTPUT");
    const char *t = getenv("XBOX_AUDIO_TONE");
    int six = e && (!strcmp(e, "5.1") || !strcmp(e, "51") || !strcmp(e, "6"));

    if (g_dev) return 1;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[XA2] SDL audio unavailable: %s\n", SDL_GetError());
        return 0;
    }
    g_tone = t && t[0] == '1';
    memset(&want, 0, sizeof want);
    want.freq = XA2_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)(six ? 6 : 2);
    want.samples = 512;
    want.callback = xa2_callback;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev && six) {
        want.channels = 2;
        g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    }
    if (!g_dev) {
        fprintf(stderr, "[XA2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 0;
    }
    g_channels = want.channels;
    SDL_PauseAudioDevice(g_dev, 0);
    fprintf(stderr, "[XA2] SDL audio (%s): %d Hz, %d channel(s), device buffer %d samples%s\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
            have.freq, g_channels, have.samples,
            g_tone ? ", 440 Hz test tone on (XBOX_AUDIO_TONE=1)" : "");
    return 1;
}

void xa2_shutdown(void)
{
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    fprintf(stderr, "[XA2] %lu buffers sent, %lu dropped, %lu device callbacks, %lu underruns\n",
            g_buffers, g_drops, g_callbacks, g_underruns);
}
#endif /* __SWITCH__ */

int xa2_is_active(void) { return g_dev != 0; }
int xa2_channels(void)  { return g_channels; }
int xa2_get_buffer_size(void) { return XA2_BUF_SAMPLES; }

int xa2_queued(void)
{
    int fill;
    if (!g_dev) return 0;
    XA2_LOCK();
    fill = g_fill;
    XA2_UNLOCK();
    return fill / (XA2_BUF_SAMPLES * g_channels);
}

/* Every 10 s: what reached the device. Few callbacks means the device thread
 * is starved; a peak of 0 means the game's mix is silent. */
static void xa2_report(const int16_t *s, int count)
{
    static Uint32 last;
    static unsigned long b0, d0, c0, u0;
    static int peak;
    Uint32 now = SDL_GetTicks();
    int i;
    for (i = 0; i < count; i++) {
        int v = s[i] < 0 ? -s[i] : s[i];
        if (v > peak) peak = v;
    }
    if (!last) last = now;
    if (now - last < 10000) return;
    fprintf(stderr, "[XA2] %u s: %lu buffers sent, %lu dropped, %lu device callbacks, "
            "%lu underruns, %d queued, peak %d\n", (unsigned)((now - last) / 1000),
            g_buffers - b0, g_drops - d0, g_callbacks - c0, g_underruns - u0, xa2_queued(), peak);
    last = now;
    b0 = g_buffers; d0 = g_drops; c0 = g_callbacks; u0 = g_underruns;
    peak = 0;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int n = (num_samples > XA2_BUF_SAMPLES ? XA2_BUF_SAMPLES : num_samples) * g_channels;
    int wr, first, ok = 0;
    if (!g_dev) return 0;
    XA2_LOCK();
    if (g_fill + n <= XA2_NUM_BUFS * XA2_BUF_SAMPLES * g_channels) {
        wr = (g_rd + g_fill) % XA2_FIFO;
        first = XA2_FIFO - wr < n ? XA2_FIFO - wr : n;
        memcpy(g_fifo + wr, samples, (size_t)first * sizeof(int16_t));
        memcpy(g_fifo, samples + first, (size_t)(n - first) * sizeof(int16_t));
        g_fill += n;
        g_buffers++;
        ok = 1;
    } else {
        g_drops++;
    }
    XA2_UNLOCK();
    xa2_report(samples, n);
    return ok;
}

#endif /* _WIN32 */
