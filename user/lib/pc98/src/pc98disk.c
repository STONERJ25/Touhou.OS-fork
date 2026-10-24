/*
 * D88, FDI, HDI and raw disk images (see include/pc98disk.h). All arithmetic on header fields is done in 64 bits and
 * checked against the image size before anything is dereferenced.
 */
#include <pc98disk.h>
#include <string.h>

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

const char *pc98_disk_format_name(Pc98DiskFormat f) {
    switch (f) {
    case PC98_DISK_RAW: return "raw";
    case PC98_DISK_D88: return "D88";
    case PC98_DISK_FDI: return "FDI";
    case PC98_DISK_HDI: return "HDI";
    default: return "none";
    }
}

/* ------------------------------------------------------------------ D88 */

static int open_d88(Pc98Disk *d) {
    const uint8_t *p = d->data;
    if (d->size < PC98_D88_HEADER) return PC98_DISK_EFORMAT;
    uint32_t disk_size = le32(p + 0x1C);
    uint8_t media = p[0x1B], wp = p[0x1A];
    if (media != 0x00 && media != 0x10 && media != 0x20 && media != 0x30 && media != 0x40) return PC98_DISK_EFORMAT;
    if (wp != 0x00 && wp != PC98_D88_WP) return PC98_DISK_EFORMAT;
    if (disk_size < PC98_D88_HEADER - 4u * PC98_D88_MAXTRACKS + 4u || disk_size > d->size) return PC98_DISK_EFORMAT;
    /* The table ends where the first track begins (some writers use 160 entries, most 164). */
    uint32_t first = 0;
    for (uint32_t i = 0; i < PC98_D88_MAXTRACKS; i++) {
        uint32_t off = le32(p + 0x20 + 4 * i);
        if (off && (!first || off < first)) first = off;
        if (first && 0x20 + 4 * (i + 1) >= first) break;
    }
    uint32_t n = PC98_D88_MAXTRACKS;
    if (first) {
        if (first < 0x20 + 4) return PC98_DISK_ECORRUPT;
        if ((first - 0x20) / 4 < n) n = (first - 0x20) / 4;
    }
    d->ntracks = n;
    memset(d->track, 0, sizeof d->track);
    uint32_t last = 0, odd = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = le32(p + 0x20 + 4 * i);
        if (!off) continue;
        if (off < 0x20 + 4 * n || (uint64_t)off + 16 > disk_size) return PC98_DISK_ECORRUPT;
        d->track[i] = off;
        last = i;
        if (i & 1) odd = 1;
    }
    d->size = disk_size;                    /* a multi-disk file: this image ends here */
    d->format = PC98_DISK_D88;
    d->media = media;
    d->write_protect = wp == PC98_D88_WP;
    memcpy(d->name, p, 16);
    d->name[16] = 0;
    for (int i = 0; i < 16; i++) if (!d->name[i]) break; else if ((uint8_t)d->name[i] < 0x20) d->name[i] = '?';
    d->heads = odd ? 2 : 1;
    d->cylinders = first ? last / 2 + 1 : 0;
    d->sectors = d->sector_size = 0;
    d->data_offset = 0;
    Pc98Sector s;
    if (d->track[0] && pc98_disk_sector_at(d, 0, 0, 0, &s) == PC98_DISK_OK) {
        d->sectors = le16(p + d->track[0] + 4);
        d->sector_size = s.n <= 7 ? 128u << s.n : s.size;
    }
    return PC98_DISK_OK;
}

/* Walks a D88 track to its idx-th sector. */
static int d88_at(const Pc98Disk *d, uint32_t t, uint32_t idx, Pc98Sector *out) {
    if (t >= d->ntracks || !d->track[t]) return PC98_DISK_ENOSECTOR;
    uint64_t off = d->track[t];
    const uint8_t *p = d->data;
    if (off + 16 > d->size) return PC98_DISK_ECORRUPT;
    uint32_t nsec = le16(p + off + 4);
    if (idx >= nsec) return PC98_DISK_ENOSECTOR;
    for (uint32_t i = 0;; i++) {
        if (off + 16 > d->size) return PC98_DISK_ECORRUPT;
        uint32_t len = le16(p + off + 14);
        if (off + 16 + len > d->size) return PC98_DISK_ECORRUPT;
        if (i == idx) {
            out->c = p[off]; out->h = p[off + 1]; out->r = p[off + 2]; out->n = p[off + 3];
            out->density = p[off + 6];
            out->deleted = p[off + 7] != 0;
            out->status = p[off + 8];
            out->data = p + off + 16;
            out->size = len;
            return PC98_DISK_OK;
        }
        off += 16 + len;
    }
}

/* ------------------------------------------------------------------ FDI / HDI / raw */

static int open_anex(Pc98Disk *d, Pc98DiskFormat f) {
    const uint8_t *p = d->data;
    if (d->size < 32 || le32(p) != 0) return PC98_DISK_EFORMAT;
    uint32_t type = le32(p + 4), hdr = le32(p + 8), datasz = le32(p + 12), ssz = le32(p + 16);
    uint32_t spt = le32(p + 20), heads = le32(p + 24), cyls = le32(p + 28);
    if (hdr < 32 || (uint64_t)hdr + datasz > d->size) return PC98_DISK_EFORMAT;
    if (ssz < 128 || ssz > 8192 || (ssz & (ssz - 1))) return PC98_DISK_EFORMAT;
    if (!spt || !heads || !cyls || spt > 255 || heads > 255 || cyls > 65535) return PC98_DISK_EFORMAT;
    if ((uint64_t)spt * heads * cyls * ssz > datasz) return PC98_DISK_ECORRUPT;
    if (f == PC98_DISK_NONE) f = (heads <= 2 && cyls <= 90) ? PC98_DISK_FDI : PC98_DISK_HDI;
    d->format = f;
    d->media = type;
    d->data_offset = hdr;
    d->sector_size = ssz;
    d->sectors = spt;
    d->heads = heads;
    d->cylinders = cyls;
    return PC98_DISK_OK;
}

static const struct { uint32_t size, cyl, heads, spt, ssz; } raw_sizes[] = {
    { 77u * 2 * 8 * 1024, 77, 2, 8, 1024 },     /* 2HD 1.2 MB (PC-98 native) */
    { 80u * 2 * 18 * 512, 80, 2, 18, 512 },     /* 1.44 MB */
    { 80u * 2 * 15 * 512, 80, 2, 15, 512 },     /* 2HC 1.2 MB (IBM-style) */
    { 80u * 2 * 9 * 512, 80, 2, 9, 512 },       /* 720 KB */
    { 80u * 2 * 8 * 512, 80, 2, 8, 512 },       /* 640 KB 2DD */
};

static int open_raw(Pc98Disk *d) {
    for (size_t i = 0; i < sizeof raw_sizes / sizeof raw_sizes[0]; i++) {
        if (d->size != raw_sizes[i].size) continue;
        d->format = PC98_DISK_RAW;
        d->cylinders = raw_sizes[i].cyl;
        d->heads = raw_sizes[i].heads;
        d->sectors = raw_sizes[i].spt;
        d->sector_size = raw_sizes[i].ssz;
        d->data_offset = 0;
        return PC98_DISK_OK;
    }
    return PC98_DISK_EFORMAT;
}

int pc98_disk_open(Pc98Disk *d, const uint8_t *data, size_t size, Pc98DiskFormat hint) {
    memset(d, 0, sizeof *d);
    d->data = data;
    d->size = size;
    int rc;
    switch (hint) {
    case PC98_DISK_D88: rc = open_d88(d); break;
    case PC98_DISK_FDI: case PC98_DISK_HDI: rc = open_anex(d, hint); break;
    case PC98_DISK_RAW: rc = open_raw(d); break;
    default:
        rc = open_d88(d);
        if (rc == PC98_DISK_EFORMAT) { d->size = size; rc = open_anex(d, PC98_DISK_NONE); }
        if (rc == PC98_DISK_EFORMAT) rc = open_raw(d);
        break;
    }
    if (rc != PC98_DISK_OK) {
        memset(d, 0, sizeof *d);
        d->format = PC98_DISK_NONE;
    }
    return rc;
}

static int linear_sector(const Pc98Disk *d, uint32_t cyl, uint32_t head, uint32_t r, Pc98Sector *out) {
    if (cyl >= d->cylinders || head >= d->heads || r < 1 || r > d->sectors) return PC98_DISK_ENOSECTOR;
    uint64_t off = d->data_offset + (((uint64_t)cyl * d->heads + head) * d->sectors + (r - 1)) * d->sector_size;
    if (off + d->sector_size > d->size) return PC98_DISK_ECORRUPT;
    out->c = (uint8_t)cyl;
    out->h = (uint8_t)head;
    out->r = (uint8_t)r;
    out->n = 0;
    while ((128u << out->n) < d->sector_size && out->n < 7) out->n++;
    out->deleted = out->status = out->density = 0;
    out->data = d->data + off;
    out->size = d->sector_size;
    return PC98_DISK_OK;
}

uint32_t pc98_disk_track_sectors(const Pc98Disk *d, uint32_t cyl, uint32_t head) {
    if (d->format == PC98_DISK_D88) {
        uint64_t t = (uint64_t)cyl * 2 + head;
        if (head > 1 || t >= d->ntracks || !d->track[t]) return 0;
        return le16(d->data + d->track[t] + 4);
    }
    return (cyl < d->cylinders && head < d->heads) ? d->sectors : 0;
}

int pc98_disk_sector_at(const Pc98Disk *d, uint32_t cyl, uint32_t head, uint32_t idx, Pc98Sector *out) {
    if (d->format == PC98_DISK_D88) {
        if (head > 1 || cyl >= PC98_D88_MAXTRACKS) return PC98_DISK_ENOSECTOR;
        return d88_at(d, cyl * 2 + head, idx, out);
    }
    if (d->format == PC98_DISK_NONE) return PC98_DISK_ENOSECTOR;
    return linear_sector(d, cyl, head, idx + 1, out);
}

int pc98_disk_sector(const Pc98Disk *d, uint32_t cyl, uint32_t head, uint32_t r, Pc98Sector *out) {
    if (d->format != PC98_DISK_D88) {
        if (d->format == PC98_DISK_NONE) return PC98_DISK_ENOSECTOR;
        return linear_sector(d, cyl, head, r, out);
    }
    /* Like an FDC: the first sector on the track whose record number matches, preferring one whose C and H match. */
    uint32_t n = pc98_disk_track_sectors(d, cyl, head);
    int found = 0;
    Pc98Sector s;
    for (uint32_t i = 0; i < n; i++) {
        int rc = pc98_disk_sector_at(d, cyl, head, i, &s);
        if (rc != PC98_DISK_OK) return rc;
        if (s.r != r) continue;
        if (s.c == cyl && s.h == head) { *out = s; return PC98_DISK_OK; }
        if (!found) { *out = s; found = 1; }
    }
    return found ? PC98_DISK_OK : PC98_DISK_ENOSECTOR;
}

int pc98_disk_read_lba(const Pc98Disk *d, uint32_t lba, Pc98Sector *out) {
    if (!d->sectors || !d->heads) return PC98_DISK_ENOSECTOR;
    uint32_t per_cyl = d->sectors * d->heads;
    uint32_t cyl = lba / per_cyl, head = (lba / d->sectors) % d->heads, r = lba % d->sectors + 1;
    if (d->format == PC98_DISK_D88) return pc98_disk_sector(d, cyl, head, r, out);
    return linear_sector(d, cyl, head, r, out);
}
