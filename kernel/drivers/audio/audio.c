/*
 * The audio core: /dev/audio, the software mixer and the thread that keeps the hardware ring fed.
 *
 * Every open of /dev/audio is a stream with its own format (rate, channels, sample size), volume and a 256 KiB queue.
 * One kernel thread ("Prismriver" would be fitting; the log says Nitori) mixes all streams into the backend's looping
 * 48 kHz stereo ring a little ahead of the hardware play position:
 *
 *      ring:  [ played, zeroed ][ hw reads here -> mixed audio ... produced ][ zeros (nothing mixed yet) ... ]
 *
 * Frames behind the play position are zeroed as soon as the hardware has passed them, so when this thread is late
 * (a slow machine, an underrun) the hardware plays silence, never a repeat of old sound. All arithmetic is integer:
 * the kernel is built without SSE.
 */
#include "audiodev.h"
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
#include "process.h"
#include "cmdline.h"
#include "initcall.h"

#define MAX_STREAMS   16
#define QUEUE_BYTES   (256 * 1024)
#define MIX_CHUNK     256
#define LEAD_FRAMES   4800              /* keep this much mixed ahead of the hardware: 100 ms */
#define START_MARGIN  960               /* a burst of sound starts this far ahead of the play position: 20 ms */
#define PERIOD_MS     5
#define STALL_MS      400               /* the hardware position has not moved for this long while sound is waiting */
#define LIMIT_KNEE    28000             /* the soft limiter starts here (of 32767) */

typedef struct Stream {
    int      pid;
    char     name[16];
    /* format of the bytes the client writes */
    u32      rate, channels, format, bpf;
    /* byte queue: head is advanced by the writer, tail by the mixer (free running counters, UP kernel: barrier() is enough) */
    u8      *buf;
    volatile u32 head, tail;
    /* resampler: linear interpolation between two source frames, 16.16 position */
    u32      phase;
    s32      cur[2], nxt[2];
    int      have;                      /* how many of cur/nxt hold a frame */
    u32      vol_l, vol_r;              /* 0..100 */
    bool     mute;
    bool     closed;                    /* the client is gone; free the stream when its queue has been played */
    bool     draining;
    u64      out_end;                   /* g.produced after the last chunk this stream took part in */
    u64      frames_in, frames_out;
    u32      underruns;
    WaitQueue wq;                       /* writers wait here for room */
    Mutex    wlock;                     /* one writer at a time (a forked child shares the descriptor) */
} Stream;

static struct {
    AudioBackend *be;
    Mutex     lock;                     /* guards streams[], the mixer state and format changes */
    Stream   *streams[MAX_STREAMS];
    WaitQueue work;                     /* the mixer thread sleeps here while there is nothing to do */
    bool      thread_started;
    u64       played, produced;         /* frames, unwrapped */
    u32       last_pos;
    bool      running, stalled, null_sink, in_burst;
    u64       last_move_ms, last_tick_ms, idle_since, hk_ms;
    u32       master_l, master_r;
    bool      muted;
    u32       idle_ms;
    u64       frames_mixed, underruns, hw_starts;
    char      name[64];
} g;

/* ------------------------------------------------------------------ small helpers */

static u32 gain15(u32 v) {              /* 0..100 -> 0..32768, squared so the slider feels linear to the ear */
    if (v > 100) v = 100;
    return v * v * 32768 / 10000;
}

static s32 limit(s32 x) {               /* soft limiter: linear below the knee, then a rational curve that never reaches 32768 */
    const s64 K = 32767 - LIMIT_KNEE;
    if (x > LIMIT_KNEE) { s64 d = x - LIMIT_KNEE; return (s32)(LIMIT_KNEE + d * K / (d + K)); }
    if (x < -LIMIT_KNEE) { s64 d = (s64)-x - LIMIT_KNEE; return -(s32)(LIMIT_KNEE + d * K / (d + K)); }
    return x;
}

static bool any_pending(void) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream *s = g.streams[i];
        if (s && s->head != s->tail) return true;
    }
    return false;
}

static u32 bytes_per_frame(u32 channels, u32 format) {
    u32 b = format == AUDIO_FMT_U8 ? 1 : format == AUDIO_FMT_S16 ? 2 : format == AUDIO_FMT_S24 ? 3 : 4;
    return b * channels;
}

static void stream_free(Stream *s) {
    for (int i = 0; i < MAX_STREAMS; i++) if (g.streams[i] == s) g.streams[i] = NULL;
    kfree(s->buf);
    kfree(s);
}

/* ------------------------------------------------------------------ the mixer */

/* Decodes the next source frame of a stream into out[0..1] (16-bit range); false if the queue holds less than a frame. */
static bool fetch(Stream *s, s32 *out) {
    if (s->head - s->tail < s->bpf) return false;
    const u8 *b = s->buf;
    const u32 mask = QUEUE_BYTES - 1;
    u32 t = s->tail;
    s32 v[2] = { 0, 0 };
    for (u32 c = 0; c < s->channels; c++) {
        switch (s->format) {
        case AUDIO_FMT_U8:  v[c] = ((s32)b[t & mask] - 128) << 8; t += 1; break;
        case AUDIO_FMT_S16: v[c] = (s16)(b[t & mask] | (b[(t + 1) & mask] << 8)); t += 2; break;
        case AUDIO_FMT_S24: v[c] = (s16)(b[(t + 1) & mask] | (b[(t + 2) & mask] << 8)); t += 3; break;
        default:            v[c] = (s16)(b[(t + 2) & mask] | (b[(t + 3) & mask] << 8)); t += 4; break;
        }
    }
    out[0] = v[0];
    out[1] = s->channels == 2 ? v[1] : v[0];
    barrier();
    s->tail = t;
    s->frames_out++;
    return true;
}

/* Adds `frames` output frames of this stream to acc. Source frames are interpolated linearly at phase/65536 between
 * cur and nxt; the phase advances by rate/48000 per output frame and whole source frames are dropped from the front. */
static void mix_stream(Stream *s, s32 *acc, u32 frames) {
    u32 step = (u32)(((u64)s->rate << 16) / AUDIO_HW_RATE);
    u32 gl = (gain15(s->vol_l) * gain15(g.master_l)) >> 15;
    u32 gr = (gain15(s->vol_r) * gain15(g.master_r)) >> 15;
    if (s->mute || g.muted) gl = gr = 0;
    u32 i;
    for (i = 0; i < frames; i++) {
        for (;;) {
            while (s->have < 2) {
                s32 f[2];
                if (!fetch(s, f)) goto starved;
                s32 *slot = s->have == 0 ? s->cur : s->nxt;
                slot[0] = f[0]; slot[1] = f[1];
                s->have++;
            }
            if (s->phase < 65536) break;
            s->phase -= 65536;
            s->cur[0] = s->nxt[0]; s->cur[1] = s->nxt[1];
            s->have = 1;
        }
        s32 l = s->cur[0] + (s32)(((s64)(s->nxt[0] - s->cur[0]) * s->phase) >> 16);
        s32 r = s->cur[1] + (s32)(((s64)(s->nxt[1] - s->cur[1]) * s->phase) >> 16);
        acc[2 * i]     += (s32)(((s64)l * gl) >> 15);
        acc[2 * i + 1] += (s32)(((s64)r * gr) >> 15);
        s->phase += step;
    }
starved:
    if (i) s->out_end = g.produced + frames;
}

static void mix_chunk(s16 *out, u32 frames) {
    static s32 acc[MIX_CHUNK * 2];
    memset(acc, 0, (size_t)frames * 2 * sizeof(s32));
    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream *s = g.streams[i];
        if (s && s->head != s->tail) mix_stream(s, acc, frames);
    }
    for (u32 i = 0; i < frames * 2; i++) out[i] = (s16)limit(acc[i]);
    g.frames_mixed += frames;
}

static void zero_frames(u32 start, u32 count) {
    AudioBackend *be = g.be;
    u32 n = be->ring_frames;
    if (count > n) count = n;
    u32 first = n - start < count ? n - start : count;
    memset(be->ring + (size_t)start * 2, 0, (size_t)first * 4);
    if (count > first) memset(be->ring, 0, (size_t)(count - first) * 4);
}

static bool start_hw(void) {
    AudioBackend *be = g.be;
    u64 now = uptime_ms();
    memset(be->ring, 0, (size_t)be->ring_frames * 4);
    g.played = g.produced = 0;
    g.last_pos = 0;
    g.in_burst = false;
    g.stalled = false;
    g.last_move_ms = g.last_tick_ms = now;
    for (int i = 0; i < MAX_STREAMS; i++) if (g.streams[i]) g.streams[i]->out_end = 0;
    if (!g.null_sink && be->start(be) != 0) {
        klog("Nitori", "audio: the %s would not start; sound is discarded at the right speed instead", be->name);
        g.null_sink = true;
    }
    g.running = true;
    g.hw_starts++;
    return true;
}

static void stop_hw(void) {
    AudioBackend *be = g.be;
    if (!g.null_sink && be->stop) be->stop(be);
    memset(be->ring, 0, (size_t)be->ring_frames * 4);
    g.running = false;
    g.stalled = false;
    g.in_burst = false;
    g.played = g.produced = 0;
}

/* One refill pass. Called with g.lock held. */
static void pass(void) {
    AudioBackend *be = g.be;
    u64 now = uptime_ms();
    bool pending = any_pending();
    if (!g.running) {
        if (!pending) return;
        start_hw();
    }
    const u32 n = be->ring_frames, mask = n - 1;

    /* how far has the hardware got since the last look? */
    u32 delta = 0;
    if (!g.null_sink) {
        u32 pos = be->position(be) & mask;
        delta = (pos - g.last_pos) & mask;
        if (delta) { g.last_move_ms = now; if (g.stalled) { g.stalled = false; klog("Nitori", "audio: the hardware is moving again"); } }
        g.last_pos = pos;
    }
    if (!delta && (g.null_sink || (g.produced > g.played && now - g.last_move_ms > STALL_MS))) {
        if (!g.null_sink && !g.stalled) {
            g.stalled = true;
            klog("Nitori", "audio: the hardware stopped reading its buffer; sound is discarded at the right speed instead");
        }
        u64 ms = now - g.last_tick_ms;
        delta = (u32)(ms * (AUDIO_HW_RATE / 1000));
        if (delta > n / 2) delta = n / 2;
        if (g.null_sink) g.last_pos = (g.last_pos + delta) & mask;
    }
    g.last_tick_ms = now;
    if (delta) {
        zero_frames((u32)(g.played & mask), delta);
        g.played += delta;
    }

    if (g.produced < g.played) {
        if (g.in_burst) {
            /* the hardware ran out of mixed audio: an underrun if some client was still supposed to be streaming */
            bool streaming = false;
            for (int i = 0; i < MAX_STREAMS; i++) {
                Stream *s = g.streams[i];
                if (s && !s->closed && !s->draining && s->frames_in && s->head == s->tail) { s->underruns++; streaming = true; }
            }
            if (streaming) g.underruns++;
            g.in_burst = false;
        }
        g.produced = g.played;
    }
    if (pending && !g.in_burst) {
        if (g.produced < g.played + START_MARGIN) g.produced = g.played + START_MARGIN;
        g.in_burst = true;
    }

    u64 target = g.played + LEAD_FRAMES;
    while (g.in_burst && g.produced < target && any_pending()) {
        u32 chunk = (u32)(target - g.produced);
        if (chunk > MIX_CHUNK) chunk = MIX_CHUNK;
        u32 at = (u32)(g.produced & mask);
        if (at + chunk > n) chunk = n - at;
        mix_chunk(be->ring + (size_t)at * 2, chunk);
        g.produced += chunk;
    }
    if (g.in_burst && g.produced <= g.played) g.in_burst = false;

    /* streams that have been played out: wake writers, free the ones whose client is gone */
    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream *s = g.streams[i];
        if (!s) continue;
        if (QUEUE_BYTES - (s->head - s->tail) >= QUEUE_BYTES / 4) wq_wake_all(&s->wq);
        if (s->closed && s->head == s->tail) stream_free(s);
    }

    if (pending || g.produced > g.played) g.idle_since = now;
    else if (now - g.idle_since > g.idle_ms) {
        stop_hw();
        return;
    }
    if (be->housekeeping && now - g.hk_ms >= 250) { g.hk_ms = now; be->housekeeping(be); }
}

static void audio_thread(void *arg) {
    (void)arg;
    for (;;) {
        u64 f = irq_save();
        while (!g.running && !any_pending()) wq_block(&g.work);
        irq_restore(f);
        mutex_lock(&g.lock);
        pass();
        mutex_unlock(&g.lock);
        task_sleep_ms(PERIOD_MS);
    }
}

/* ------------------------------------------------------------------ /dev/audio */

static void audio_opened(VNode *v, File *f) {
    (void)v;
    f->priv = NULL;
    Stream *s = kzalloc(sizeof *s);
    if (!s) return;
    s->buf = kmalloc(QUEUE_BYTES);
    if (!s->buf) { kfree(s); return; }
    s->rate = AUDIO_HW_RATE; s->channels = 2; s->format = AUDIO_FMT_S16; s->bpf = 4;
    s->vol_l = s->vol_r = 100;
    s->pid = current->proc ? current->proc->pid : 0;
    strlcpy(s->name, current->proc ? current->proc->name : "kernel", sizeof s->name);
    wq_init(&s->wq);
    mutex_init(&s->wlock);
    mutex_lock(&g.lock);
    int slot = -1;
    for (int i = 0; i < MAX_STREAMS; i++) if (!g.streams[i]) { slot = i; break; }
    if (slot >= 0) { g.streams[slot] = s; f->priv = s; }
    mutex_unlock(&g.lock);
    if (slot < 0) { kfree(s->buf); kfree(s); }
}

static void audio_closed(VNode *v, File *f) {
    (void)v;
    Stream *s = f->priv;
    if (!s) return;
    mutex_lock(&g.lock);
    s->closed = true;                   /* what is queued still plays; the mixer frees the stream when it is empty */
    if (s->head == s->tail) stream_free(s);
    mutex_unlock(&g.lock);
}

static ssize_t audio_write(VNode *v, File *f, const void *buf, size_t n, u64 off) {
    (void)v; (void)off;
    Stream *s = f->priv;
    if (!s) return -EBUSY;
    if (!g.be) return -ENODEV;
    const u8 *in = buf;
    size_t done = 0;
    ssize_t rc = 0;
    mutex_lock(&s->wlock);
    while (done < n) {
        u32 space = QUEUE_BYTES - (s->head - s->tail);
        if (space) {
            size_t chunk = MIN((size_t)space, n - done);
            u32 h = s->head;
            for (size_t i = 0; i < chunk; i++) s->buf[(h + (u32)i) & (QUEUE_BYTES - 1)] = in[done + i];
            barrier();
            s->head = h + (u32)chunk;
            s->frames_in += chunk / s->bpf;
            done += chunk;
            wq_wake_all(&g.work);
            continue;
        }
        if (f->flags & O_NONBLOCK) { if (!done) rc = -EAGAIN; break; }
        if (signal_pending()) { if (!done) rc = -EINTR; break; }
        u64 fl = irq_save();
        if (QUEUE_BYTES - (s->head - s->tail) == 0) {
            current->intr = true;
            wq_block_timeout(&s->wq, 100);
            current->intr = false;
        }
        irq_restore(fl);
    }
    mutex_unlock(&s->wlock);
    return done ? (ssize_t)done : rc;
}

static int audio_poll(VNode *v, File *f) {
    (void)v;
    Stream *s = f->priv;
    if (!s) return POLLERR;
    return (QUEUE_BYTES - (s->head - s->tail) >= QUEUE_BYTES / 4) ? POLLOUT : 0;
}

static void fill_volume(struct audio_volume *o, u32 l, u32 r, bool m) { o->left = l; o->right = r; o->mute = m; }

static int audio_ioctl(VNode *v, File *f, unsigned long req, void *arg) {
    (void)v;
    Stream *s = f->priv;
    switch (req) {
    case AUDIO_SET_FORMAT: {
        struct audio_format *a = arg;
        if (!s) return -EBUSY;
        if (a->rate < AUDIO_MIN_RATE || a->rate > AUDIO_MAX_RATE || a->channels < 1 || a->channels > 2 ||
            a->format < AUDIO_FMT_U8 || a->format > AUDIO_FMT_S32) return -EINVAL;
        mutex_lock(&g.lock);
        int rc = 0;
        if (s->head != s->tail) rc = -EBUSY;          /* the queued bytes belong to the old format */
        else {
            s->rate = a->rate; s->channels = a->channels; s->format = a->format;
            s->bpf = bytes_per_frame(a->channels, a->format);
            s->have = 0; s->phase = 0;
        }
        mutex_unlock(&g.lock);
        return rc;
    }
    case AUDIO_GET_FORMAT: {
        struct audio_format *a = arg;
        if (!s) return -EBUSY;
        a->rate = s->rate; a->channels = s->channels; a->format = s->format;
        return 0;
    }
    case AUDIO_SET_VOLUME: {
        struct audio_volume *a = arg;
        if (!s) return -EBUSY;
        s->vol_l = MIN(a->left, 100u); s->vol_r = MIN(a->right, 100u); s->mute = a->mute != 0;
        return 0;
    }
    case AUDIO_GET_VOLUME:
        if (!s) return -EBUSY;
        fill_volume(arg, s->vol_l, s->vol_r, s->mute);
        return 0;
    case AUDIO_SET_MASTER: {
        struct audio_volume *a = arg;
        g.master_l = MIN(a->left, 100u); g.master_r = MIN(a->right, 100u); g.muted = a->mute != 0;
        return 0;
    }
    case AUDIO_GET_MASTER:
        fill_volume(arg, g.master_l, g.master_r, g.muted);
        return 0;
    case AUDIO_GET_INFO: {
        struct audio_info *i = arg;
        memset(i, 0, sizeof *i);
        strlcpy(i->name, g.name, sizeof i->name);
        if (g.be) strlcpy(i->desc, g.be->desc, sizeof i->desc);
        i->hw_rate = AUDIO_HW_RATE;
        i->ring_frames = g.be ? g.be->ring_frames : 0;
        i->lead_frames = LEAD_FRAMES;
        for (int k = 0; k < MAX_STREAMS; k++) if (g.streams[k] && !g.streams[k]->closed) i->streams++;
        i->state = g.stalled || g.null_sink ? AUDIO_STATE_STALLED : g.running ? AUDIO_STATE_RUNNING : AUDIO_STATE_IDLE;
        i->frames_mixed = g.frames_mixed;
        i->underruns = g.underruns;
        return 0;
    }
    case AUDIO_GET_STATUS: {
        struct audio_status *o = arg;
        if (!s) return -EBUSY;
        memset(o, 0, sizeof *o);
        u32 q = s->head - s->tail;
        o->queue_bytes = QUEUE_BYTES;
        o->queued_bytes = q;
        o->space_bytes = QUEUE_BYTES - q;
        o->queued_ms = (u32)((u64)(q / s->bpf) * 1000 / s->rate);
        o->frames_written = s->frames_in;
        o->frames_played = s->frames_out;
        o->underruns = s->underruns;
        o->hw_running = g.running;
        o->hw_frames = g.played;
        o->pending_hw_frames = g.produced > g.played ? g.produced - g.played : 0;
        return 0;
    }
    case AUDIO_DRAIN: {
        if (!s) return -EBUSY;
        s->draining = true;
        for (;;) {
            bool empty = s->head == s->tail;
            if (empty && (!g.running || g.played >= s->out_end)) return 0;
            if (f->flags & O_NONBLOCK) return -EAGAIN;
            if (signal_pending()) return -EINTR;
            task_sleep_ms(5);
        }
    }
    case AUDIO_FLUSH:
        if (!s) return -EBUSY;
        mutex_lock(&g.lock);
        s->tail = s->head;
        s->have = 0; s->phase = 0;
        mutex_unlock(&g.lock);
        wq_wake_all(&s->wq);
        return 0;
    default:
        return -ENOTTY;
    }
}

static const VNodeOps audio_ops = {
    .write = audio_write, .ioctl = audio_ioctl, .poll = audio_poll, .opened = audio_opened, .closed = audio_closed,
};

/* ------------------------------------------------------------------ /proc/audio */

static size_t gen_proc(char *b, size_t cap) {
    size_t n = 0;
    if (!g.be) return (size_t)snprintf(b, cap, "no sound device\n");
    AudioBackend *be = g.be;
    const char *state = g.null_sink ? "no hardware (discarding)" : g.stalled ? "stalled (discarding)" : g.running ? "running" : "idle";
    n += (size_t)snprintf(b + n, cap - n, "device:   %s\n", g.name);
    n += (size_t)snprintf(b + n, cap - n, "path:     %s\n", be->desc);
    n += (size_t)snprintf(b + n, cap - n, "state:    %s\n", state);
    n += (size_t)snprintf(b + n, cap - n, "format:   %u Hz, 16-bit, stereo; ring %u frames (%u ms), mixed %u ms ahead\n", AUDIO_HW_RATE, be->ring_frames,
                          be->ring_frames * 1000 / AUDIO_HW_RATE, LEAD_FRAMES * 1000 / AUDIO_HW_RATE);
    n += (size_t)snprintf(b + n, cap - n, "master:   left %u%% right %u%%, %s\n", g.master_l, g.master_r, g.muted ? "muted" : "not muted");
    n += (size_t)snprintf(b + n, cap - n, "counters: mixed %lu frames, played %lu frames, underruns %lu, starts %lu\n", (unsigned long)g.frames_mixed,
                          (unsigned long)g.played, (unsigned long)g.underruns, (unsigned long)g.hw_starts);
    int open = 0;
    for (int i = 0; i < MAX_STREAMS; i++) if (g.streams[i] && !g.streams[i]->closed) open++;
    n += (size_t)snprintf(b + n, cap - n, "streams:  %d open\n", open);
    for (int i = 0; i < MAX_STREAMS && n + 160 < cap; i++) {
        Stream *s = g.streams[i];
        if (!s) continue;
        static const char *const fmt[] = { "?", "u8", "s16", "s24", "s32" };
        n += (size_t)snprintf(b + n, cap - n, "  #%d pid %d %s: %u Hz %uch %s, queued %u bytes, volume %u/%u%s, written %lu frames, underruns %u%s\n", i, s->pid,
                              s->name, s->rate, s->channels, fmt[s->format <= 4 ? s->format : 0], (u32)(s->head - s->tail), s->vol_l, s->vol_r,
                              s->mute ? " muted" : "", (unsigned long)s->frames_in, s->underruns, s->closed ? " (closed, playing out)" : "");
    }
    if (be->describe && n + 64 < cap) n += be->describe(be, b + n, cap - n);
    return n;
}

/* ------------------------------------------------------------------ registration */

void audio_get_master(u32 *left, u32 *right, bool *mute) {
    if (left) *left = g.master_l;
    if (right) *right = g.master_r;
    if (mute) *mute = g.muted;
}

bool audio_registered(void) { return g.be != NULL; }

int audio_register(AudioBackend *b) {
    if (g.be) return -EBUSY;
    if (!b || !b->ring || !b->start || !b->position || (b->ring_frames & (b->ring_frames - 1)) || b->ring_frames < 4096) return -EINVAL;
    mutex_init(&g.lock);
    wq_init(&g.work);
    g.master_l = g.master_r = 60;
    g.idle_ms = (u32)cmdline_int("audio.idle", 3000);
    strlcpy(g.name, b->name, sizeof g.name);
    g.be = b;
    memset(b->ring, 0, (size_t)b->ring_frames * 4);
    int rc = devfs_register("audio", S_IFCHR | 0660, &audio_ops, NULL, 0x0E03);
    if (rc == 0) devfs_chown("audio", 0, GID_SHRINE);
    else { klog("Nitori", "audio: cannot create /dev/audio (%d)", rc); g.be = NULL; return rc; }
    procfs_register("audio", gen_proc);
    Task *t = task_create_detached("audio", audio_thread, NULL);
    if (!t) klog("Nitori", "audio: cannot start the mixer thread");
    klog("Nitori", "/dev/audio: %s -- %s", b->name, b->desc);
    return 0;
}
