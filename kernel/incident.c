/* Flavour text for fatal errors. The technical description always prints too; this is the Gensokyo commentary. */
#include "kernel.h"
#include "arch.h"

static const char *const generic_quips[] = {
    "Yukari opened a gap in the kernel.",
    "A fairy tripped over a pointer.",
    "Reimu is napping on the porch; nobody is guarding the shrine.",
    "Sakuya stopped time, and the kernel did not notice.",
    "Eiki has reviewed the evidence. The verdict is not good.",
    "Marisa borrowed something and has not given it back.",
    "The Hakurei Boundary has been breached.",
};

static unsigned g_quip_counter;

const char *incident_quip(u64 vector, u64 cr2, u64 error) {
    switch (vector) {
    case 0:  return "Cirno attempted arithmetic. Dividing by zero is not on the strongest curriculum.";
    case 6:  return "An unknown spell card was cast (invalid opcode).";
    case 8:  return "An incident occurred while reporting an incident. Eiki is overworked.";
    case 12: return "Marisa borrowed too much stack.";
    case 13: return "A privileged spell was cast without a licence from Eiki.";
    case 14:
        if (cr2 < 4096) return "Address zero is the edge of Gensokyo. Nothing lives there.";
        if (error & 16) return "Someone tried to execute the unexecutable. The Boundary held.";
        if (error & 1)  return "The Boundary held: that page is not yours to touch that way.";
        return "Yukari's gap swallowed that address. Nothing is mapped there.";
    default:
        return generic_quips[g_quip_counter++ % ARRAY_SIZE(generic_quips)];
    }
}
