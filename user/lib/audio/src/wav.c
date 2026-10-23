/* libaudio: WAV files (RIFF/WAVE, PCM 8/16/24/32-bit, mono or stereo). */
#include <audio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Parses the RIFF header in buf[0..n): the 'fmt ' chunk and where the 'data' chunk starts. n should be a few hundred bytes. */
int audio_wav_parse_header(const uint8_t *buf, size_t n, AudioWav *w) {
    if (n < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return -EINVAL;
    size_t pos = 12;
    bool have_fmt = false;
    uint16_t tag = 0;
    memset(w, 0, sizeof *w);
    w->fd = -1;
    while (pos + 8 <= n) {
        uint32_t size = le32(buf + pos + 4);
        if (!memcmp(buf + pos, "fmt ", 4)) {
            if (size < 16 || pos + 8 + 16 > n) return -EINVAL;
            tag = le16(buf + pos + 8);
            w->channels = le16(buf + pos + 10);
            w->rate = le32(buf + pos + 12);
            w->bits = le16(buf + pos + 22);
            if (tag == 0xFFFE && size >= 26 && pos + 8 + 26 <= n) tag = le16(buf + pos + 8 + 24);   /* WAVE_FORMAT_EXTENSIBLE: the sub format */
            have_fmt = true;
        } else if (!memcmp(buf + pos, "data", 4)) {
            if (!have_fmt) return -EINVAL;
            w->data_start = (uint32_t)(pos + 8);
            w->data_bytes = size;
            break;
        }
        pos += 8 + (size_t)size + (size & 1);
    }
    if (!have_fmt || !w->data_start) return -EINVAL;
    if (tag != 1 || w->channels < 1 || w->channels > 2 || w->rate < AUDIO_MIN_RATE || w->rate > AUDIO_MAX_RATE) return -EINVAL;
    switch (w->bits) {
    case 8:  w->format = AUDIO_FMT_U8; break;
    case 16: w->format = AUDIO_FMT_S16; break;
    case 24: w->format = AUDIO_FMT_S24; break;
    case 32: w->format = AUDIO_FMT_S32; break;
    default: return -EINVAL;
    }
    return 0;
}

int audio_wav_open(AudioWav *w, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    uint8_t head[512];
    ssize_t n = read(fd, head, sizeof head);
    if (n < 12) { close(fd); return -EINVAL; }
    int rc = audio_wav_parse_header(head, (size_t)n, w);
    if (rc) { close(fd); return rc; }
    off_t end = lseek(fd, 0, SEEK_END);
    if (end >= 0 && (off_t)w->data_start + (off_t)w->data_bytes > end) w->data_bytes = end > (off_t)w->data_start ? (uint32_t)(end - (off_t)w->data_start) : 0;
    w->fd = fd;
    w->pos = 0;
    lseek(fd, (off_t)w->data_start, SEEK_SET);
    return 0;
}

ssize_t audio_wav_read(AudioWav *w, void *buf, size_t bytes) {
    size_t frame = (size_t)(w->bits / 8) * w->channels;
    if (w->pos >= w->data_bytes) return 0;
    size_t left = w->data_bytes - w->pos;
    if (bytes > left) bytes = left;
    bytes -= bytes % frame;
    if (!bytes) return 0;
    ssize_t n = read(w->fd, buf, bytes);
    if (n > 0) w->pos += (uint32_t)n;
    return n;
}

uint32_t audio_wav_frames(const AudioWav *w) {
    size_t frame = (size_t)(w->bits / 8) * w->channels;
    return frame ? (uint32_t)(w->data_bytes / frame) : 0;
}

int audio_wav_seek(AudioWav *w, uint32_t frame) {
    size_t fb = (size_t)(w->bits / 8) * w->channels;
    uint64_t off = (uint64_t)frame * fb;
    if (off > w->data_bytes) off = w->data_bytes;
    if (lseek(w->fd, (off_t)(w->data_start + off), SEEK_SET) < 0) return -errno;
    w->pos = (uint32_t)off;
    return 0;
}

void audio_wav_close(AudioWav *w) {
    if (w->fd >= 0) close(w->fd);
    w->fd = -1;
}

size_t audio_wav_make_header(uint8_t out[44], unsigned rate, unsigned channels, unsigned bits, size_t data_bytes) {
    uint32_t block = channels * bits / 8;
    memcpy(out, "RIFF", 4);
    uint32_t v = (uint32_t)(36 + data_bytes);
    memcpy(out + 4, &v, 4);
    memcpy(out + 8, "WAVEfmt ", 8);
    v = 16; memcpy(out + 16, &v, 4);
    uint16_t s = 1; memcpy(out + 20, &s, 2);
    s = (uint16_t)channels; memcpy(out + 22, &s, 2);
    v = rate; memcpy(out + 24, &v, 4);
    v = rate * block; memcpy(out + 28, &v, 4);
    s = (uint16_t)block; memcpy(out + 32, &s, 2);
    s = (uint16_t)bits; memcpy(out + 34, &s, 2);
    memcpy(out + 36, "data", 4);
    v = (uint32_t)data_bytes; memcpy(out + 40, &v, 4);
    return 44;
}

int audio_wav_save(const char *path, unsigned rate, unsigned channels, unsigned bits, const void *pcm, size_t bytes) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    uint8_t h[44];
    audio_wav_make_header(h, rate, channels, bits, bytes);
    int rc = 0;
    if (write(fd, h, 44) != 44) rc = -EIO;
    const uint8_t *p = pcm;
    size_t done = 0;
    while (!rc && done < bytes) {
        ssize_t n = write(fd, p + done, bytes - done);
        if (n <= 0) { rc = -EIO; break; }
        done += (size_t)n;
    }
    close(fd);
    return rc;
}
