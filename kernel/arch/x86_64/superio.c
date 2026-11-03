/*
 * The Super I/O probe (kernel/include/superio.h): which chip is on the LPC bus at 0x2E and 0x4E, and where is its hardware monitor.
 *
 * Written from the public descriptions of the configuration protocols: Winbond/Nuvoton and Fintek chips are unlocked by writing 0x87 twice to
 * the index port and locked by writing 0xAA; ITE chips by 0x87, 0x01, 0x55 and then 0x55 (index port 0x2E) or 0xAA (0x4E), and locked by writing 0x02
 * to register 0x02; SMSC/Microchip chips by 0x55 and locked by 0xAA. All of them show a device id in registers 0x20 and 0x21 and select a logical
 * device with register 0x07 (activation in 0x30, I/O base in 0x60 and 0x61). The chip names come from the id registers of the vendors' datasheets
 * as the authors remember them; the raw id is always printed, so a wrong or missing name costs nothing.
 *
 * The one rule that matters: nothing is written to a DATA port unless a chip that is in the table has just answered (the select of the hardware
 * monitor's logical device, and ITE's lock). A device that merely sits at 0x2E/0x2F and ignores the keys sees index writes and reads, nothing else.
 */
#include "superio.h"

typedef struct { u16 id, mask; const char *chip; int hwm_ldn; } Entry;     /* hwm_ldn -1: the monitor is not an ordinary logical device */

static const Entry g_nuvoton[] = {
    { 0xB470, 0xFFF0, "NCT6775F", 0x0B }, { 0xC330, 0xFFF0, "NCT6776F", 0x0B }, { 0xC560, 0xFFF0, "NCT6779D", 0x0B },
    { 0xC800, 0xFFF0, "NCT6791D", 0x0B }, { 0xC910, 0xFFF0, "NCT6792D", 0x0B }, { 0xD120, 0xFFF0, "NCT6793D", 0x0B },
    { 0xD350, 0xFFF0, "NCT6795D", 0x0B }, { 0xD420, 0xFFF8, "NCT6796D", 0x0B }, { 0xD428, 0xFFF8, "NCT6798D", 0x0B },
    { 0xD450, 0xFFF0, "NCT6797D", 0x0B },
    { 0xC730, 0xFFF0, "NCT6683D", -1 },   { 0xD440, 0xFFF0, "NCT6686D", -1 },   { 0xD590, 0xFFF0, "NCT6687D", -1 },
    { 0xA020, 0xFFF0, "W83627DHG", 0x0B }, { 0xB070, 0xFFF0, "W83627DHG-P", 0x0B }, { 0xA510, 0xFFF0, "W83667HG", 0x0B },
    { 0x0541, 0xFFFF, "F71882FG", 0x04 }, { 0x0909, 0xFFFF, "F71889ED", 0x04 }, { 0x0901, 0xFFFF, "F71808E", 0x04 }, { 0x0507, 0xFFFF, "F71858", 0x04 },
    { 0, 0, 0, 0 },
};

static const Entry g_ite[] = {
    { 0x8705, 0xFFFF, "IT8705F", 0x04 }, { 0x8712, 0xFFFF, "IT8712F", 0x04 }, { 0x8716, 0xFFFF, "IT8716F", 0x04 }, { 0x8718, 0xFFFF, "IT8718F", 0x04 },
    { 0x8720, 0xFFFF, "IT8720F", 0x04 }, { 0x8721, 0xFFFF, "IT8721F", 0x04 }, { 0x8726, 0xFFFF, "IT8726F", 0x04 }, { 0x8728, 0xFFFF, "IT8728F", 0x04 },
    { 0x8732, 0xFFFF, "IT8732F", 0x04 }, { 0x8771, 0xFFFF, "IT8771E", 0x04 }, { 0x8772, 0xFFFF, "IT8772E", 0x04 }, { 0x8603, 0xFFFF, "IT8603E", 0x04 },
    { 0x8613, 0xFFFF, "IT8613E", 0x04 }, { 0x8620, 0xFFFF, "IT8620E", 0x04 }, { 0x8625, 0xFFFF, "IT8625E", 0x04 }, { 0x8628, 0xFFFF, "IT8628E", 0x04 },
    { 0x8655, 0xFFFF, "IT8655E", 0x04 }, { 0x8665, 0xFFFF, "IT8665E", 0x04 }, { 0x8686, 0xFFFF, "IT8686E", 0x04 }, { 0x8688, 0xFFFF, "IT8688E", 0x04 },
    { 0x8790, 0xFFFF, "IT8790E", 0x04 }, { 0x8733, 0xFFFF, "IT8792E", 0x04 },
    { 0, 0, 0, 0 },
};

static const Entry g_smsc[] = {                         /* register 0x20 is the device id, 0x21 the revision: the table matches the device id only */
    { 0x5900, 0xFF00, "LPC47M10x/112", -1 }, { 0x5100, 0xFF00, "LPC47B37x", -1 }, { 0x7C00, 0xFF00, "SCH3112", -1 }, { 0x7D00, 0xFF00, "SCH3114", -1 },
    { 0x7F00, 0xFF00, "SCH3116", -1 }, { 0x8900, 0xFF00, "SCH5027", -1 }, { 0x8600, 0xFF00, "SCH5127", -1 }, { 0x8500, 0xFF00, "SCH5317", -1 },
    { 0, 0, 0, 0 },
};

const char *superio_family_name(int family) {
    switch (family) {
    case SIO_NUVOTON: return "Nuvoton/Winbond/Fintek";
    case SIO_ITE:     return "ITE";
    case SIO_SMSC:    return "SMSC/Microchip";
    default:          return "none";
    }
}

static const Entry *table_for(int family) {
    return family == SIO_NUVOTON ? g_nuvoton : family == SIO_ITE ? g_ite : family == SIO_SMSC ? g_smsc : 0;
}

bool superio_lookup(int family, u16 id, const char **chip, int *hwm_ldn) {
    const Entry *t = table_for(family);
    for (; t && t->mask; t++) {
        if ((id & t->mask) != t->id) continue;
        if (chip) *chip = t->chip;
        if (hwm_ldn) *hwm_ldn = t->hwm_ldn;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ the protocol */

#define OUT(port, v) io->out(io->ctx, (u16)(port), (u8)(v))
#define IN(port)     io->in(io->ctx, (u16)(port))

static u8 reg(const SioPorts *io, u16 base, u8 r) {            /* an index write, then a data read */
    OUT(base, r);
    return IN(base + 1);
}

static u16 read_id(const SioPorts *io, u16 base) {
    u8 hi = reg(io, base, 0x20);
    u8 lo = reg(io, base, 0x21);
    return (u16)(hi << 8 | lo);
}

static void unlock(const SioPorts *io, u16 base, int family) {
    switch (family) {
    case SIO_NUVOTON: OUT(base, 0x87); OUT(base, 0x87); break;
    case SIO_ITE:     OUT(base, 0x87); OUT(base, 0x01); OUT(base, 0x55); OUT(base, base == 0x2E ? 0x55 : 0xAA); break;
    case SIO_SMSC:    OUT(base, 0x55); break;
    }
}

/* `known`: the chip's id is in the table, so it surely is what the key says it is. Only then ITE's lock, which is a DATA write (register 2, bit 1), is
 * sent: to a chip of another family that the sequence happened to unlock, register 2 may mean something else. The other families lock with a write to
 * the index port, which is harmless wherever it lands. (An ITE chip that is not in the table is left in configuration mode: it costs nothing, since
 * everybody who uses it sends the key first, and the next reset puts it right.) */
static void lock(const SioPorts *io, u16 base, int family, bool known) {
    switch (family) {
    case SIO_NUVOTON: OUT(base, 0xAA); break;
    case SIO_SMSC:    OUT(base, 0xAA); break;
    case SIO_ITE:
        if (known) { OUT(base, 0x02); OUT(base + 1, 0x02); }
        break;
    }
}

static bool blank(u16 id) { return id == 0x0000 || id == 0xFFFF; }

static void copy_name(char *dst, size_t cap, const char *src) {
    size_t k = 0;
    while (src[k] && k + 1 < cap) { dst[k] = src[k]; k++; }
    dst[k] = 0;
}

static void read_monitor(const SioPorts *io, u16 base, int ldn, SioResult *r) {
    OUT(base, 0x07);
    OUT(base + 1, ldn);                                         /* the one write to a data port: which logical device the next reads are about */
    r->hwm_ldn = ldn;
    r->hwm_active = reg(io, base, 0x30) & 1;
    r->hwm_base = (u16)(reg(io, base, 0x60) << 8 | reg(io, base, 0x61));
}

static void probe_port(const SioPorts *io, SioResult *r) {
    u16 base = r->port;
    u16 quiet = read_id(io, base);                              /* what the registers say with no key: a quiet bus, or a device that needs none */
    /* The order matters. ITE's key (87 01 55 55) contains SMSC's whole key (55): an SMSC chip would be unlocked by it, so SMSC is tried first, and a
     * chip that is found is never sent another family's key. Nuvoton's (87 87) is the start of ITE's: a chip waiting for the rest just starts over. */
    static const int families[3] = { SIO_NUVOTON, SIO_SMSC, SIO_ITE };
    int guess_family = SIO_NONE;
    u16 guess_id = 0;
    for (int i = 0; i < 3; i++) {
        int fam = families[i];
        unlock(io, base, fam);
        u16 id = read_id(io, base);
        const char *chip = 0;
        int ldn = -1;
        /* An id that is in the table is a chip, whether the key unlocked it or the firmware had left it open. An id that is not in the table
         * counts only when the key changed what the registers say (a bus that is quiet reads the same with and without it). */
        if (!blank(id) && superio_lookup(fam, id, &chip, &ldn)) {
            r->family = fam;
            r->id = id;
            copy_name(r->chip, sizeof r->chip, chip);
            r->hwm_known = ldn >= 0;
            if (ldn >= 0) read_monitor(io, base, ldn, r);
            lock(io, base, fam, true);
            return;
        }
        bool reacts = !blank(id) && id != quiet;
        if (reacts && guess_family == SIO_NONE) { guess_family = fam; guess_id = id; }
        lock(io, base, fam, false);
    }
    if (guess_family != SIO_NONE) {                             /* something answered to a key but the table does not know the id */
        r->family = guess_family;
        r->id = guess_id;
        OUT(base, 0xAA);                                        /* (and if a later key unlocked it again, this puts it back: an index write, harmless to anything else) */
    }
}

int superio_probe(const SioPorts *io, SioResult res[2]) {
    static const u16 ports[2] = { 0x2E, 0x4E };
    int answered = 0;
    for (int i = 0; i < 2; i++) {
        SioResult *r = &res[i];
        char *p = (char *)r;
        for (size_t k = 0; k < sizeof *r; k++) p[k] = 0;
        r->port = ports[i];
        r->family = SIO_NONE;
        probe_port(io, r);
        if (r->family != SIO_NONE) answered++;
    }
    return answered;
}

/* ------------------------------------------------------------------ the report */

typedef struct { char *b; size_t cap, n; } Out;

static void put(Out *o, const char *s) {
    while (*s && o->n + 1 < o->cap) o->b[o->n++] = *s++;
    o->b[o->n < o->cap ? o->n : o->cap - 1] = 0;
}

static void put_hex(Out *o, u32 v, int digits) {
    char t[12];
    int n = 0;
    t[n++] = '0';
    t[n++] = 'x';
    for (int i = digits - 1; i >= 0; i--) t[n++] = "0123456789abcdef"[(v >> (4 * i)) & 15];
    t[n] = 0;
    put(o, t);
}

size_t superio_format(const SioResult res[2], int answered, char *buf, size_t cap) {
    Out o = { buf, cap, 0 };
    if (cap == 0) return 0;
    buf[0] = 0;
    put(&o, answered ? "superio: a chip answered\n" : "superio: nothing answered\n");
    for (int i = 0; i < 2; i++) {
        const SioResult *r = &res[i];
        put(&o, "port.");
        put_hex(&o, r->port, 2);
        if (r->family == SIO_NONE) { put(&o, ": nothing answered\n"); continue; }
        put(&o, ".family: ");
        put(&o, superio_family_name(r->family));
        put(&o, "\nport.");
        put_hex(&o, r->port, 2);
        put(&o, ".id: ");
        put_hex(&o, r->id, 4);
        put(&o, "\nport.");
        put_hex(&o, r->port, 2);
        put(&o, ".chip: ");
        put(&o, r->chip[0] ? r->chip : "unknown (the id is not in the table)");
        if (r->hwm_known) {
            put(&o, "\nport.");
            put_hex(&o, r->port, 2);
            put(&o, ".hwm: logical device ");
            put_hex(&o, (u32)r->hwm_ldn, 2);
            put(&o, r->hwm_active ? ", active, io base " : ", NOT active, io base ");
            put_hex(&o, r->hwm_base, 4);
        }
        put(&o, "\n");
    }
    put(&o, "fans: not read (see docs/SENSORS.md: the monitor's registers have to be checked against the chip's datasheet first)\n");
    return o.n;
}
