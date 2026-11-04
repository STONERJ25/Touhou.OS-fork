/*
 * pacer: keeps the emulated machine in step with the monotonic clock. The machine's time is its own CPU clock (one frame of the
 * games' display is 283,573 clocks at 16 MHz, 56.4 times a second); the pacer says how many of those clocks are owed at this
 * moment and never decides anything about drawing: emulation is never skipped, only pictures are (the caller draws only the
 * last frame of whatever it just ran).
 *
 *     due = pacer_due(&p, now_ns, max);       // clocks to run now (0 when the machine is ahead)
 *     ... run the machine for `due` clocks ...
 *     pacer_ran(&p, clocks_actually_run);
 *     wait = pacer_wait_ms(&p, now_ns);       // how long the caller may sleep before more is due
 *
 * Three refinements:
 *  - `lead`: the machine runs this far ahead of the wall clock. The sound card plays what the emulation produced a moment ago;
 *    a cushion of a few frames of audio absorbs the jitter of a desktop that is busy with something else.
 *  - catch-up limit: when the machine fell more than `max_behind` behind (a host that is too slow, a window that was stuck), the
 *    debt is forgiven and counted in `resyncs`: slow motion, not a burst of ten seconds of game that nobody could watch.
 *  - speed: 100 = real time, 200 = twice as fast, 0 = no limit (the machine always owes `max`). Changing it re-bases the
 *    clock so that nothing jumps.
 * The pacer also measures what the status line shows: the speed achieved (machine seconds per wall second) and the machine's
 * frames per wall second, over windows of about a second.
 *
 * Pure C with no dependencies; time is a plain nanosecond count the caller supplies (so the host tests drive it with a fake clock).
 */
#ifndef PC98APP_PACER_H
#define PC98APP_PACER_H

#include <stdint.h>

typedef struct {
    uint32_t cpu_hz;                /* machine clocks per machine second */
    uint32_t speed;                 /* percent of real time, 0 = unlimited */
    uint64_t lead_ns;
    uint64_t max_behind_ns;
    uint64_t t0, c0;                /* wall time and machine clocks done at the base point */
    uint64_t done;                  /* machine clocks done in all */
    int started;
    /* the measuring window */
    uint64_t m_t, m_c, m_f;
    uint32_t speed_x10;             /* achieved speed in tenths of a percent (1000 = real time) */
    uint32_t fps_x10;               /* machine frames per wall second, times 10 */
    uint32_t resyncs;
} Pacer;

void pacer_init(Pacer *p, uint32_t cpu_hz, uint32_t speed_percent, uint64_t lead_ns, uint64_t max_behind_ns);
/* Starts (or restarts) the clock at `now`: after the machine was paused, reset, or the speed changed. `done` is kept. */
void pacer_rebase(Pacer *p, uint64_t now_ns);
void pacer_set_speed(Pacer *p, uint32_t speed_percent, uint64_t now_ns);
/* Machine clocks to run now, at most `max_clocks`. */
uint64_t pacer_due(Pacer *p, uint64_t now_ns, uint64_t max_clocks);
/* Tells the pacer what the machine has actually done (the CPU's own cycle count, however it got there). */
void pacer_set_done(Pacer *p, uint64_t machine_clocks);
/* Milliseconds the caller may sleep before at least `step_ns` of machine time is owed again (0: more is due already; capped at 20). */
uint32_t pacer_wait_ms(const Pacer *p, uint64_t now_ns, uint64_t step_ns);
/* Feeds the measurement; call once per pass with the machine's frame counter. */
void pacer_measure(Pacer *p, uint64_t now_ns, uint64_t machine_frames);

#endif
