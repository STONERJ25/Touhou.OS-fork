/* The sound device of the PC-98 application: what the machine's sound pipe (audiopipe.h) holds goes to /dev/audio, never blocking the
 * emulation. The stream is opened non-blocking; each pass the pump asks how much the device can take and how much it already holds,
 * and writes only that. The pipe's ring (a third of a second) is what absorbs the difference between the machine's clock and the sound
 * card's: when the pump cannot keep up the ring drops its oldest frames, when the machine is behind the device plays silence (the
 * audio core fills an underrun with zeros: docs/AUDIO.md), neither of which stops anything. */
#include "app.h"
#include <errno.h>

#define QUEUE_MS_MAX 160                /* the device holds this much of our sound at most: more would only be delay */

void sound_open(void) {
    if (!A.emu->cfg.sound) return;
    A.audio = audio_open(AUDIO_NONBLOCK);
    if (!A.audio) {                     /* no sound card (QEMU without one), or none we may use: no sound, and no error */
        emu_set_sound(A.emu, 0);
        say("pc98app: no sound device (%s)", audio_strerror(errno));
        return;
    }
    audio_set_format(A.audio, EMU_AUDIO_RATE, 2, AUDIO_FMT_S16);
    say("pc98app: sound: /dev/audio at %d Hz", EMU_AUDIO_RATE);
}

void sound_flush(void) {
    if (A.audio) audio_flush(A.audio);
}

void sound_pump(void) {
    Emu *e = A.emu;
    int16_t buf[1024][2];
    if (!A.audio || !e->sound || !e->booted) return;
    if (e->cfg.speed != 100) {          /* faster or slower than real time: the pitch would be wrong, so it is silent; keep the ring empty */
        while (emu_audio_take(e, &buf[0][0], 1024) == 1024) { }
        return;
    }
    struct audio_status st;
    if (audio_status(A.audio, &st) != 0) return;
    while (st.queued_ms < QUEUE_MS_MAX && st.space_bytes >= 4) {
        size_t want = st.space_bytes / 4 < 1024 ? st.space_bytes / 4 : 1024;
        size_t n = emu_audio_take(e, &buf[0][0], want);
        if (!n) break;
        ssize_t w = audio_write(A.audio, buf, n * 4);
        if (w <= 0) break;
        st.queued_ms += (uint32_t)(n * 1000 / EMU_AUDIO_RATE);
        st.space_bytes -= (uint32_t)w < st.space_bytes ? (uint32_t)w : st.space_bytes;
        if (n < want) break;
    }
}

void sound_close(void) {
    if (!A.audio) return;
    audio_flush(A.audio);
    audio_close(A.audio);
    A.audio = NULL;
}
