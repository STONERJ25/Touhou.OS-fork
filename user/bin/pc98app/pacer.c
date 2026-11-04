/* pacer: the machine's clock against the wall clock (pacer.h). 64-bit integer arithmetic only: the program is linked without the compiler's
 * 128-bit division, so nothing here may need it. The wall time since the base point is kept below about two seconds by moving the base
 * forward a whole second at a time (an exact number of machine clocks), which keeps every product below 2^63. */
#include "pacer.h"

#define NS 1000000000ull

/* Machine clocks that `ns` nanoseconds (under a few seconds) of wall time are worth at the pacer's speed. cpu_hz is a whole number of kilohertz. */
static uint64_t clocks_for(const Pacer *p, uint64_t ns) {
    if (ns > 4 * NS) ns = 4 * NS;
    return ns * (p->cpu_hz / 1000) * (p->speed ? p->speed : 100u) / 100000000ull;
}

void pacer_init(Pacer *p, uint32_t cpu_hz, uint32_t speed_percent, uint64_t lead_ns, uint64_t max_behind_ns) {
    *p = (Pacer){ 0 };
    p->cpu_hz = cpu_hz ? cpu_hz : 1000;
    p->speed = speed_percent;
    p->lead_ns = lead_ns;
    p->max_behind_ns = max_behind_ns;
}

void pacer_rebase(Pacer *p, uint64_t now_ns) {
    p->t0 = now_ns;
    p->c0 = p->done;
    p->started = 1;
    p->m_t = now_ns;
    p->m_c = p->done;
    p->m_f = 0;                                 /* the next pacer_measure() takes the frame counter as it finds it */
    p->speed_x10 = 0;
    p->fps_x10 = 0;
}

void pacer_set_speed(Pacer *p, uint32_t speed_percent, uint64_t now_ns) {
    p->speed = speed_percent;
    pacer_rebase(p, now_ns);
}

void pacer_set_done(Pacer *p, uint64_t machine_clocks) { p->done = machine_clocks; }

/* Where the machine ought to be at `now`, without the lead. The base moves forward by whole seconds first. */
static uint64_t owed_at(Pacer *p, uint64_t now_ns) {
    while (now_ns > p->t0 && now_ns - p->t0 >= 2 * NS) {
        p->t0 += NS;
        p->c0 += (uint64_t)(p->cpu_hz / 1000) * (p->speed ? p->speed : 100u) * 10;      /* one second: cpu_hz * speed / 100 clocks */
    }
    return p->c0 + clocks_for(p, now_ns > p->t0 ? now_ns - p->t0 : 0);
}

uint64_t pacer_due(Pacer *p, uint64_t now_ns, uint64_t max_clocks) {
    if (!p->started) pacer_rebase(p, now_ns);
    if (!p->speed) return max_clocks;
    uint64_t owed = owed_at(p, now_ns);
    if (owed > p->done && owed - p->done > clocks_for(p, p->max_behind_ns)) {     /* hopelessly behind: forgive the debt */
        p->t0 = now_ns;
        p->c0 = p->done;
        p->resyncs++;
        owed = p->done;
    }
    uint64_t target = owed + clocks_for(p, p->lead_ns);
    if (target <= p->done) return 0;
    uint64_t due = target - p->done;
    return due < max_clocks ? due : max_clocks;
}

uint32_t pacer_wait_ms(const Pacer *p, uint64_t now_ns, uint64_t step_ns) {
    if (!p->started || !p->speed) return 0;
    Pacer q = *p;                               /* (owed_at may move the base: on a copy, so that this stays a const query) */
    uint64_t target = owed_at(&q, now_ns) + clocks_for(&q, q.lead_ns);
    uint64_t need = q.done + clocks_for(&q, step_ns);
    if (need <= target) return 0;
    uint64_t diff = need - target, per_ms = (uint64_t)(q.cpu_hz / 1000) * q.speed / 100;
    if (!per_ms || diff > 20 * per_ms) return 20;
    return (uint32_t)((diff + per_ms - 1) / per_ms);       /* wall milliseconds until `diff` more clocks are owed */
}

void pacer_measure(Pacer *p, uint64_t now_ns, uint64_t machine_frames) {
    if (!p->started) return;
    if (!p->m_f) p->m_f = machine_frames + 1;   /* (+1 keeps "not set" apart from a counter that is really 0) */
    if (now_ns < p->m_t + NS) return;
    uint64_t dt = now_ns - p->m_t;
    uint64_t machine_us = (p->done - p->m_c) * 1000 / (p->cpu_hz / 1000);
    p->speed_x10 = (uint32_t)(machine_us * 1000000ull / dt);
    p->fps_x10 = (uint32_t)((machine_frames + 1 - p->m_f) * NS * 10u / dt);
    p->m_t = now_ns;
    p->m_c = p->done;
    p->m_f = machine_frames + 1;
}
