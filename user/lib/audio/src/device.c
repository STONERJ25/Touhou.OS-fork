/* libaudio: the /dev/audio wrapper. */
#include <audio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <signal.h>

bool audio_present(void) {
    struct stat st;
    return stat("/dev/audio", &st) == 0;
}

const char *audio_strerror(int err) {
    if (err < 0) err = -err;
    switch (err) {
    case ENOENT:
    case ENODEV:  return "this machine has no sound device (no Intel HD Audio or AC'97 controller was found)";
    case EACCES:
    case EPERM:   return "you are not allowed to use the sound device (it belongs to the shrine group)";
    case EBUSY:   return "the sound device has too many programs playing already";
    case EINVAL:  return "the sound device does not support that format";
    case EAGAIN:  return "the sound queue is full";
    case EINTR:   return "interrupted";
    case ENOMEM:  return "out of memory";
    default:      return "the sound device reported an error";
    }
}

Audio *audio_open(int flags) {
    int of = (flags & AUDIO_CONTROL) ? O_RDONLY : O_WRONLY;
    if (flags & AUDIO_NONBLOCK) of |= O_NONBLOCK;
    int fd = open("/dev/audio", of);
    if (fd < 0) return NULL;
    Audio *a = calloc(1, sizeof *a);
    if (!a) { close(fd); errno = ENOMEM; return NULL; }
    a->fd = fd;
    a->fmt.rate = AUDIO_HW_RATE;
    a->fmt.channels = 2;
    a->fmt.format = AUDIO_FMT_S16;
    return a;
}

void audio_close(Audio *a) {
    if (!a) return;
    close(a->fd);
    free(a);
}

int audio_fd(const Audio *a) { return a->fd; }

int audio_set_format(Audio *a, unsigned rate, unsigned channels, unsigned format) {
    struct audio_format f = { rate, channels, format };
    if (ioctl(a->fd, AUDIO_SET_FORMAT, &f) < 0) return -errno;
    a->fmt = f;
    return 0;
}

size_t audio_frame_bytes(const Audio *a) {
    size_t b = a->fmt.format == AUDIO_FMT_U8 ? 1 : a->fmt.format == AUDIO_FMT_S16 ? 2 : a->fmt.format == AUDIO_FMT_S24 ? 3 : 4;
    return b * a->fmt.channels;
}

ssize_t audio_write(Audio *a, const void *data, size_t bytes) {
    const uint8_t *p = data;
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = write(a->fd, p + done, bytes - done);
        if (n < 0) {
            if (errno == EINTR) { if (done) break; return -EINTR; }
            if (errno == EAGAIN) { if (done) break; return -EAGAIN; }
            return done ? (ssize_t)done : -errno;
        }
        done += (size_t)n;
        if (n == 0) break;
        if (done < bytes && (fcntl(a->fd, F_GETFL, 0) & O_NONBLOCK)) break;
    }
    return (ssize_t)done;
}

int audio_drain(Audio *a) { return ioctl(a->fd, AUDIO_DRAIN, NULL) < 0 ? -errno : 0; }
int audio_flush(Audio *a) { return ioctl(a->fd, AUDIO_FLUSH, NULL) < 0 ? -errno : 0; }

int audio_set_volume(Audio *a, int left, int right, int mute) {
    struct audio_volume v = { (uint32_t)(left < 0 ? 0 : left), (uint32_t)(right < 0 ? 0 : right), (uint32_t)(mute != 0) };
    return ioctl(a->fd, AUDIO_SET_VOLUME, &v) < 0 ? -errno : 0;
}

int audio_status(Audio *a, struct audio_status *st) { return ioctl(a->fd, AUDIO_GET_STATUS, st) < 0 ? -errno : 0; }
int audio_info(Audio *a, struct audio_info *info) { return ioctl(a->fd, AUDIO_GET_INFO, info) < 0 ? -errno : 0; }

int audio_get_master(Audio *a, int *left, int *right, int *mute) {
    struct audio_volume v;
    memset(&v, 0, sizeof v);
    if (ioctl(a->fd, AUDIO_GET_MASTER, &v) < 0) return -errno;
    if (left) *left = (int)v.left;
    if (right) *right = (int)v.right;
    if (mute) *mute = (int)v.mute;
    return 0;
}

int audio_set_master(Audio *a, int left, int right, int mute) {
    struct audio_volume v = { (uint32_t)(left < 0 ? 0 : left), (uint32_t)(right < 0 ? 0 : right), (uint32_t)(mute != 0) };
    return ioctl(a->fd, AUDIO_SET_MASTER, &v) < 0 ? -errno : 0;
}
