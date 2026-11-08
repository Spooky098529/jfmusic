/* Audio playback.
 *
 * Each track is a Stream shared by two threads:
 *   download thread  GETs /Audio/{id}/universal into a growing memory buffer
 *   decode thread    runs a miniaudio decoder over that buffer (reads block
 *                    until the bytes have arrived) and pushes PCM into a FIFO
 * The device callback only copies from the FIFO, so a slow network gives
 * silence ("buffering"), never a stalled audio thread.
 *
 * The server sends MP3, FLAC and WAV untouched and transcodes everything else
 * to MP3, so miniaudio's built-in decoders cover every file.
 */
#include "jf.h"
#include "ma_cfg.h"

typedef struct Stream {
    volatile LONG refs, cancel;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    unsigned char *buf;
    size_t len, cap;
    long long total;            /* Content-Length, or -1 */
    int hdr, done, failed;      /* headers known / download finished / failed */
    char ctype[64];
    char err[256];
    size_t rpos;                /* decoder read position (decode thread only) */
    char itemid[40];
} Stream;

static ma_context g_ctx;
static ma_device g_dev;
static int g_ctx_ok, g_dev_ok, g_running;
static ma_uint32 g_rate;

/* FIFO of interleaved stereo float frames, and the playback state; all
   guarded by g_fcs. */
static CRITICAL_SECTION g_fcs;
static float *g_fifo;
static ma_uint32 g_fcap, g_frd, g_fcount;
static Stream *g_cur;
static long long g_base, g_played;  /* position = g_base + g_played frames */
static int g_eof, g_epoch, g_seek_pending, g_state;
static long long g_seek_to;
static char g_err[256];
static HANDLE g_space;              /* set when the callback frees FIFO space */

static void stream_release(Stream *st)
{
    if (InterlockedDecrement(&st->refs)) return;
    DeleteCriticalSection(&st->cs);
    free(st->buf);
    free(st);
}

static void stream_cancel(Stream *st)
{
    InterlockedExchange(&st->cancel, 1);
    EnterCriticalSection(&st->cs);
    WakeAllConditionVariable(&st->cv);
    LeaveCriticalSection(&st->cs);
}

/* ------------------------------------------------------------------ */
/* Device                                                              */
/* ------------------------------------------------------------------ */

static void on_data(ma_device *d, void *out, const void *in, ma_uint32 frames)
{
    (void)d; (void)in;
    float *o = out;
    EnterCriticalSection(&g_fcs);
    ma_uint32 n = g_state == PS_PLAYING ? (frames < g_fcount ? frames : g_fcount) : 0;
    ma_uint32 first = n < g_fcap - g_frd ? n : g_fcap - g_frd;
    memcpy(o, g_fifo + (size_t)g_frd * 2, (size_t)first * 2 * sizeof(float));
    memcpy(o + (size_t)first * 2, g_fifo, (size_t)(n - first) * 2 * sizeof(float));
    g_frd = (g_frd + n) % g_fcap;
    g_fcount -= n;
    g_played += n;
    LeaveCriticalSection(&g_fcs);
    if (n < frames) memset(o + (size_t)n * 2, 0, (size_t)(frames - n) * 2 * sizeof(float));
    if (n) SetEvent(g_space);
}

/* Caller holds g_fcs. */
static void fifo_clear(void) { g_frd = g_fcount = 0; }

static ma_uint32 fifo_push(const float *src, ma_uint32 n)
{
    ma_uint32 space = g_fcap - g_fcount;
    if (n > space) n = space;
    ma_uint32 wr = (g_frd + g_fcount) % g_fcap;
    ma_uint32 first = n < g_fcap - wr ? n : g_fcap - wr;
    memcpy(g_fifo + (size_t)wr * 2, src, (size_t)first * 2 * sizeof(float));
    memcpy(g_fifo, src + (size_t)first * 2, (size_t)(n - first) * 2 * sizeof(float));
    g_fcount += n;
    return n;
}

/* Opens the output device on first use, so the app still starts (and can
   play later) when no sound device is present at launch. */
static int ensure_device(char *err, size_t errsz)
{
    if (g_dev_ok) return 1;
    if (!g_ctx_ok) { snprintf(err, errsz, "Audio system unavailable"); return 0; }
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.sampleRate = 0;                  /* device's native rate */
    dc.dataCallback = on_data;
    if (ma_device_init(&g_ctx, &dc, &g_dev) != MA_SUCCESS) {
        snprintf(err, errsz, "Could not open the audio device");
        return 0;
    }
    if (!g_fifo) {
        g_rate = g_dev.sampleRate;
        g_fcap = g_rate;                /* one second */
        g_fifo = malloc((size_t)g_fcap * 2 * sizeof(float));
        if (!g_fifo) {
            ma_device_uninit(&g_dev);
            snprintf(err, errsz, "Out of memory");
            return 0;
        }
    }
    g_dev_ok = 1;
    return 1;
}

int player_init(char *err, size_t errsz)
{
    InitializeCriticalSection(&g_fcs);
    g_space = CreateEventW(NULL, FALSE, FALSE, NULL);
    ma_context_config cc = ma_context_config_init();
#ifdef JF_NULL_AUDIO
    ma_backend be[] = { ma_backend_null };
    g_ctx_ok = ma_context_init(be, 1, &cc, &g_ctx) == MA_SUCCESS;
#else
    g_ctx_ok = ma_context_init(NULL, 0, &cc, &g_ctx) == MA_SUCCESS;
#endif
    if (!g_ctx_ok) { snprintf(err, errsz, "Audio system unavailable"); return 0; }
    return ensure_device(err, errsz);
}

void player_shutdown(void)
{
    player_stop();
    if (g_dev_ok) ma_device_uninit(&g_dev);
    if (g_ctx_ok) ma_context_uninit(&g_ctx);
    g_dev_ok = g_ctx_ok = 0;
}

/* ------------------------------------------------------------------ */
/* Download thread                                                     */
/* ------------------------------------------------------------------ */

static void dl_fail(Stream *st, const char *msg)
{
    EnterCriticalSection(&st->cs);
    st->failed = st->done = st->hdr = 1;
    snprintf(st->err, sizeof st->err, "%s", msg);
    WakeAllConditionVariable(&st->cv);
    LeaveCriticalSection(&st->cs);
}

static DWORD WINAPI dl_thread(LPVOID arg)
{
    Stream *st = arg;
    Cfg c;
    cfg_get(&c);
    char path[1024], dev[200];
    urlenc(dev, sizeof dev, c.deviceid);
    snprintf(path, sizeof path,
        "/Audio/%s/universal?UserId=%s&DeviceId=%s&Container=mp3,flac,wav"
        "&AudioCodec=mp3&TranscodingContainer=mp3&TranscodingProtocol=http"
        "&MaxStreamingBitrate=999999999&AudioBitRate=320000&MaxAudioChannels=2",
        st->itemid, c.userid, dev);

    DWORD status;
    char err[256];
    HINTERNET hc, hr = http_open_stream(&c, path, &hc, &status, err, sizeof err);
    if (!hr) { dl_fail(st, err); stream_release(st); return 0; }

    WCHAR wv[128];
    DWORD sz = sizeof wv;
    char ctype[64] = "";
    if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                            wv, &sz, WINHTTP_NO_HEADER_INDEX))
        WideCharToMultiByte(CP_UTF8, 0, wv, -1, ctype, sizeof ctype, NULL, NULL);
    ctype[sizeof ctype - 1] = 0;
    long long total = -1;
    sz = sizeof wv;
    if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            wv, &sz, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoi64(wv, NULL, 10);

    EnterCriticalSection(&st->cs);
    snprintf(st->ctype, sizeof st->ctype, "%s", ctype);
    st->total = total;
    if (total > 0 && total < ((long long)1 << 31)) {
        st->buf = malloc((size_t)total);
        st->cap = st->buf ? (size_t)total : 0;
    }
    st->hdr = 1;
    WakeAllConditionVariable(&st->cv);
    LeaveCriticalSection(&st->cs);

    static __thread unsigned char chunk[65536];
    const char *fail = NULL;
    for (;;) {
        DWORD avail = 0, got = 0;
        if (st->cancel) break;
        if (!WinHttpQueryDataAvailable(hr, &avail)) { fail = "Connection to server lost"; break; }
        if (!avail) break;
        if (avail > sizeof chunk) avail = sizeof chunk;
        if (!WinHttpReadData(hr, chunk, avail, &got)) { fail = "Connection to server lost"; break; }
        if (!got) break;
        EnterCriticalSection(&st->cs);
        if (st->len + got > st->cap) {
            size_t nc = st->cap ? st->cap * 2 : (size_t)4 << 20;
            while (nc < st->len + got) nc *= 2;
            unsigned char *nb = realloc(st->buf, nc);
            if (!nb) { LeaveCriticalSection(&st->cs); fail = "Out of memory"; break; }
            st->buf = nb;
            st->cap = nc;
        }
        memcpy(st->buf + st->len, chunk, got);
        st->len += got;
        WakeAllConditionVariable(&st->cv);
        LeaveCriticalSection(&st->cs);
    }
    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    if (fail) dl_fail(st, fail);
    EnterCriticalSection(&st->cs);
    st->done = 1;
    WakeAllConditionVariable(&st->cv);
    LeaveCriticalSection(&st->cs);
    stream_release(st);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Decode thread                                                       */
/* ------------------------------------------------------------------ */

/* Blocks until all requested bytes have downloaded, so decoders only see a
   short read at the real end of the file. */
static ma_result on_read(ma_decoder *d, void *out, size_t want, size_t *got)
{
    Stream *st = d->pUserData;
    size_t n = 0;
    EnterCriticalSection(&st->cs);
    while ((st->len < st->rpos || st->len - st->rpos < want) && !st->done && !st->cancel)
        SleepConditionVariableCS(&st->cv, &st->cs, 200);
    if (!st->cancel && st->rpos < st->len) {
        n = st->len - st->rpos;
        if (n > want) n = want;
        memcpy(out, st->buf + st->rpos, n);
        st->rpos += n;
    }
    LeaveCriticalSection(&st->cs);
    *got = n;
    return n ? MA_SUCCESS : MA_AT_END;
}

static ma_result on_seek(ma_decoder *d, ma_int64 off, ma_seek_origin origin)
{
    Stream *st = d->pUserData;
    long long base = 0;
    EnterCriticalSection(&st->cs);
    if (origin == ma_seek_origin_current) base = (long long)st->rpos;
    else if (origin == ma_seek_origin_end) {
        while (st->total < 0 && !st->done && !st->cancel)
            SleepConditionVariableCS(&st->cv, &st->cs, 200);
        base = st->total >= 0 ? st->total : (long long)st->len;
    }
    long long np = base + off;
    if (np >= 0) st->rpos = (size_t)np;
    LeaveCriticalSection(&st->cs);
    return np >= 0 ? MA_SUCCESS : MA_INVALID_ARGS;
}

static ma_encoding_format guess_format(const char *ct)
{
    if (strstr(ct, "mpeg") || strstr(ct, "mp3")) return ma_encoding_format_mp3;
    if (strstr(ct, "flac")) return ma_encoding_format_flac;
    if (strstr(ct, "wav")) return ma_encoding_format_wav;
    return ma_encoding_format_unknown;  /* let miniaudio probe */
}

/* Reports an error for st if it is still the current stream, and marks the
   track as finished so the UI moves on. */
static void dec_fail(Stream *st, const char *msg)
{
    EnterCriticalSection(&g_fcs);
    if (g_cur == st) {
        snprintf(g_err, sizeof g_err, "%s", msg);
        g_eof = 1;
    }
    LeaveCriticalSection(&g_fcs);
}

#define CHUNK 1024

static DWORD WINAPI dec_thread(LPVOID arg)
{
    Stream *st = arg;
    EnterCriticalSection(&st->cs);
    while (!st->hdr && !st->cancel) SleepConditionVariableCS(&st->cv, &st->cs, 200);
    char ctype[64], err[256];
    snprintf(ctype, sizeof ctype, "%s", st->ctype);
    snprintf(err, sizeof err, "%s", st->err);
    int failed = st->failed;
    LeaveCriticalSection(&st->cs);
    if (st->cancel) goto out;
    if (failed) { dec_fail(st, err); goto out; }

    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, g_rate);
    dc.encodingFormat = guess_format(ctype);
    ma_decoder dec;
    if (ma_decoder_init(on_read, on_seek, st, &dc, &dec) != MA_SUCCESS) {
        if (!st->cancel) {
            EnterCriticalSection(&st->cs);
            snprintf(err, sizeof err, "%s", st->failed ? st->err : "Can't decode this track");
            LeaveCriticalSection(&st->cs);
            dec_fail(st, err);
        }
        goto out;
    }

    static __thread float pcm[CHUNK * 2];
    ma_uint64 have = 0, off = 0;
    int atend = 0;
    for (;;) {
        if (st->cancel) break;
        int doseek = 0, ep;
        long long to = 0;
        EnterCriticalSection(&g_fcs);
        if (g_cur != st) { LeaveCriticalSection(&g_fcs); break; }
        if (g_seek_pending) { doseek = 1; to = g_seek_to; g_seek_pending = 0; }
        ep = g_epoch;
        LeaveCriticalSection(&g_fcs);

        if (doseek) {
            ma_decoder_seek_to_pcm_frame(&dec, (ma_uint64)to);
            have = 0;
            atend = 0;
            continue;
        }
        if (!have && !atend) {
            ma_uint64 got = 0;
            ma_decoder_read_pcm_frames(&dec, pcm, CHUNK, &got);
            have = got;
            off = 0;
            if (got < CHUNK) atend = 1;
        }
        if (!have) {
            /* Track fully decoded: flag it and idle until a seek or a new track. */
            EnterCriticalSection(&g_fcs);
            if (g_cur == st && g_epoch == ep) {
                g_eof = 1;
                EnterCriticalSection(&st->cs);
                if (st->failed && !g_err[0]) snprintf(g_err, sizeof g_err, "%s", st->err);
                LeaveCriticalSection(&st->cs);
            }
            LeaveCriticalSection(&g_fcs);
            Sleep(30);
            continue;
        }
        EnterCriticalSection(&g_fcs);
        if (g_cur != st) { LeaveCriticalSection(&g_fcs); break; }
        if (g_epoch != ep) {        /* seeked since this chunk was decoded */
            LeaveCriticalSection(&g_fcs);
            have = 0;
            atend = 0;
            continue;
        }
        ma_uint32 n = fifo_push(pcm + off * 2, (ma_uint32)have);
        LeaveCriticalSection(&g_fcs);
        off += n;
        have -= n;
        if (have) WaitForSingleObject(g_space, 50);
    }
    ma_decoder_uninit(&dec);
out:
    stream_release(st);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Control (UI thread)                                                 */
/* ------------------------------------------------------------------ */

static void set_running(int on)
{
    if (!g_dev_ok || g_running == on) return;
    if (on) g_running = ma_device_start(&g_dev) == MA_SUCCESS;
    else { ma_device_stop(&g_dev); g_running = 0; }
}

void player_play(const char *itemid)
{
    char err[256];
    if (!ensure_device(err, sizeof err)) {
        player_stop();
        EnterCriticalSection(&g_fcs);
        snprintf(g_err, sizeof g_err, "%s", err);
        LeaveCriticalSection(&g_fcs);
        return;
    }
    Stream *st = calloc(1, sizeof *st);
    if (!st) return;
    st->refs = 3;                       /* player + two threads */
    st->total = -1;
    InitializeCriticalSection(&st->cs);
    InitializeConditionVariable(&st->cv);
    snprintf(st->itemid, sizeof st->itemid, "%s", itemid);

    EnterCriticalSection(&g_fcs);
    Stream *old = g_cur;
    g_cur = st;
    fifo_clear();
    g_base = g_played = 0;
    g_eof = g_seek_pending = 0;
    g_epoch++;
    g_state = PS_PLAYING;
    g_err[0] = 0;
    LeaveCriticalSection(&g_fcs);
    if (old) { stream_cancel(old); stream_release(old); }

    HANDLE h1 = CreateThread(NULL, 0, dl_thread, st, 0, NULL);
    if (!h1) { stream_release(st); dl_fail(st, "Could not start download"); }
    else CloseHandle(h1);
    HANDLE h2 = CreateThread(NULL, 0, dec_thread, st, 0, NULL);
    if (!h2) { stream_release(st); dec_fail(st, "Could not start decoder"); }
    else CloseHandle(h2);
    set_running(1);
}

void player_stop(void)
{
    EnterCriticalSection(&g_fcs);
    Stream *old = g_cur;
    g_cur = NULL;
    fifo_clear();
    g_base = g_played = 0;
    g_eof = g_seek_pending = 0;
    g_epoch++;
    g_state = PS_STOPPED;
    LeaveCriticalSection(&g_fcs);
    if (old) { stream_cancel(old); stream_release(old); }
    set_running(0);
}

void player_pause(int paused)
{
    EnterCriticalSection(&g_fcs);
    int was = g_state;
    if (paused && was == PS_PLAYING) g_state = PS_PAUSED;
    else if (!paused && was == PS_PAUSED) g_state = PS_PLAYING;
    int now = g_state;
    LeaveCriticalSection(&g_fcs);
    /* Stop the device while paused, so Windows can sleep. */
    if (now != was) set_running(now == PS_PLAYING);
}

void player_seek(double sec)
{
    EnterCriticalSection(&g_fcs);
    if (g_cur && g_rate) {
        long long f = (long long)(sec * g_rate);
        if (f < 0) f = 0;
        g_seek_to = f;
        g_seek_pending = 1;
        g_epoch++;
        fifo_clear();
        g_base = f;
        g_played = 0;
        g_eof = 0;
        if (g_state == PS_ENDED) g_state = PS_PLAYING;
    }
    LeaveCriticalSection(&g_fcs);
}

void player_volume(float v)
{
    if (g_dev_ok) ma_device_set_master_volume(&g_dev, v);
}

void player_get(PlayerState *s)
{
    EnterCriticalSection(&g_fcs);
    if (g_state == PS_PLAYING && g_eof && !g_fcount) g_state = PS_ENDED;
    s->state = g_state;
    s->buffering = g_state == PS_PLAYING && !g_fcount && !g_eof;
    s->pos = g_rate ? (double)(g_base + g_played) / g_rate : 0;
    snprintf(s->err, sizeof s->err, "%s", g_err);
    LeaveCriticalSection(&g_fcs);
}
